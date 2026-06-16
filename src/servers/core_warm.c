/* core_warm.c -- v0.4.257 DIAGNOSTIC for the residual RPi4 spawn-storm TLBI stall.
 *
 * project_stall_hunt localized the residual (nodes=4) freeze to the REMOTE TLB
 * shootdown (doRemoteInvalidateTranslationSingle): core 0's teardown `tlbi vae1`
 * hangs ~32s waiting on cores 1-3, which are pure idle-spinners (all AIOS threads
 * pin to core 0) so THEIR DVM/coherency interface has quiesced. seL4 has no dynamic
 * CPU hotplug, so we test the inverse: keep cores 1-3 ACTIVELY executing user code
 * (memory-touch loop = TLB fills + cache-coherency traffic) and see if the remote
 * TLBI then completes fast (stall vanishes).
 *
 * Three threads pin to cores 1/2/3 and PARK on a notification (the core returns to
 * the kernel idle-spin) until armed via /proc/corewarm.1, then busy-touch a private
 * 256 KB working set until /proc/corewarm.0. INERT until armed (zero effect on the
 * shipped system); this is a hunt tool, not a feature.
 */
#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4utils/thread.h>
#include <vspace/vspace.h>
#include "arch.h"                 /* arch_dsb */
#include "aios/root_shared.h"
#define LOG_MODULE "corewarm"
#include "aios/aios_log.h"
#include "aios/cpuacct.h"

#define WARM_CORES  3                                  /* cores 1, 2, 3 */
#define WARM_PAGES  64                                 /* per-thread working set: 256 KB */
#define WARM_WORDS  (WARM_PAGES * 4096 / (int)sizeof(unsigned long))

static volatile int            g_warm_armed;           /* set by /proc (core 0), read by warmers */
static seL4_CPtr               g_warm_ntfn[WARM_CORES]; /* wake a parked warmer */
static volatile unsigned long *g_warm_buf[WARM_CORES];  /* private touch buffer per core */
static volatile uint64_t       g_warm_iters[WARM_CORES];/* full passes (diag readout) */

/* Pinned to core (idx+1). Parks on the notification (core idles) until armed, then
 * strides one word per 64-byte cache line across the 256 KB buffer -- TLB fills + RMW
 * coherency traffic keep this core's DVM interface clocked. Re-parks when disarmed. */
static void core_warm_fn(void *a, void *b, void *c) {
    seL4_CPtr ntfn = (seL4_CPtr)(uintptr_t)a;
    int idx = (int)(uintptr_t)b;
    (void)c;
    volatile unsigned long *buf = g_warm_buf[idx];
    for (;;) {
        seL4_Wait(ntfn, NULL);                 /* park -> core returns to kernel idle-spin */
        while (g_warm_armed) {
            for (int i = 0; i < WARM_WORDS; i += 8) buf[i]++;
            g_warm_iters[idx]++;
        }
    }
}

/* Spawn the three warmers (boot thread: vka/vspace are single-owner, so all allocation
 * happens HERE -- the warmers only touch pre-allocated memory, like tlbi_probe). */
void core_warm_start(void) {
    for (int k = 0; k < WARM_CORES; k++) {
        g_warm_buf[k] = (volatile unsigned long *)
            vspace_new_pages(&vspace, seL4_AllRights, WARM_PAGES, seL4_PageBits);
        if (!g_warm_buf[k]) { printf("[corewarm] no pages for core %d\n", k + 1); return; }
        vka_object_t nobj;
        if (vka_alloc_notification(&vka, &nobj)) { printf("[corewarm] no ntfn for core %d\n", k + 1); return; }
        g_warm_ntfn[k] = nobj.cptr;
        sel4utils_thread_t th;
        if (sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
                simple_get_cnode(&simple), seL4_NilData, &th)) {
            printf("[corewarm] configure failed for core %d\n", k + 1); return;
        }
        seL4_TCB_SetPriority(th.tcb.cptr, simple_get_tcb(&simple), 200);
#if CONFIG_MAX_NUM_NODES > 1
        seL4_TCB_SetAffinity(th.tcb.cptr, k + 1);          /* pin to core 1 / 2 / 3 */
#endif
        sel4utils_start_thread(&th, core_warm_fn,
            (void *)(uintptr_t)g_warm_ntfn[k], (void *)(uintptr_t)k, 1);
        char nm[16]; snprintf(nm, sizeof(nm), "corewarm%d", k + 1);
        aios_acct_register(nm, th.tcb.cptr);
    }
    printf("[corewarm] 3 warmers pinned to cores 1/2/3 (idle; /proc/corewarm.1 to arm)\n");
}

/* /proc/corewarm[.0|.1] -- the live A/B knob. .1 = arm (cores 1-3 busy), .0 = disarm
 * (cores 1-3 idle), bare = status + per-core pass counters. */
int core_warm_cmd(const char *args, char *buf, int bufsize) {
    if (args[0] == '.' && args[1] == '1') {
        g_warm_armed = 1;
        arch_dsb();                                        /* publish the flag before the wakes */
        for (int k = 0; k < WARM_CORES; k++)
            if (g_warm_ntfn[k]) seL4_Signal(g_warm_ntfn[k]);
        return snprintf(buf, bufsize, "corewarm: ARMED -- cores 1/2/3 busy-touching memory\n");
    }
    if (args[0] == '.' && args[1] == '0') {
        g_warm_armed = 0;
        arch_dsb();
        return snprintf(buf, bufsize, "corewarm: disarmed -- cores 1/2/3 returning to idle\n");
    }
    return snprintf(buf, bufsize,
        "corewarm: armed=%d  passes c1=%llu c2=%llu c3=%llu  (.1 arm / .0 disarm)\n",
        g_warm_armed,
        (unsigned long long)g_warm_iters[0],
        (unsigned long long)g_warm_iters[1],
        (unsigned long long)g_warm_iters[2]);
}
