/* fabric_warm.c -- v0.4.266: BCM2711 SCB-fabric keep-warm (the "Linux approach"
 * to the ~33s idle-teardown TLBI/DVM whole-system freeze).
 *
 * MECHANISM (candidate 2 -- docs/NEXT_20260619_candidate2_fabric_dvm.md,
 * [[project_stall_hunt]]): the teardown `tlbi ; dsb` emits a DVM Sync that the
 * `dsb` cannot retire until the BCM2711 SCB / 128-bit AMBA fabric returns
 * DVM-Complete (A72 TRM L2ACTLR[11] "generated regardless"; BRESP gated, TRM
 * 7.7.4). After idle the SoC asserts ACINACTM to idle the AXI master snoop
 * interface (TRM 2.4) / clock-starves that fabric, so the FIRST post-idle DVM
 * Sync hangs to a ~33s SoC timeout. Linux never freezes because its constant
 * timer/DMA bus traffic keeps the fabric warm. AIOS's no-WFI busy-`yield` idle
 * keeps the CPU executing but makes NO bus transactions, so the fabric still
 * quiesces.
 *
 * This thread replicates Linux's immunity. When armed it busy-loops on CORE 1
 * (which is idle-only under the all-threads-pinned-to-core-0 model, so
 * monopolizing it starves nothing -- exactly like the proven `core_warm`),
 * issuing a steady stream of LIGHT UNCACHED reads of a VideoCore-side register
 * (GPLEV0). Each read traverses the ARM->VideoCore AXI link and keeps the
 * cluster's snoop/DVM path (shared by all 4 cores) out of the quiesced state, so
 * core 0's teardown DVM-Sync completes. It must NOT run on core 0: a busy-loop
 * there monopolizes the shell/netconsole core and hangs a blocked shell (HW-
 * confirmed -- the first cut on core 0 wedged the board until disarmed). And it
 * is NOT the refuted heavy-cacheable `corewarm` (cores 1-3 256KB RMW, which
 * WORSENED the freeze via DVM contention): GPLEV0 is Device memory, so the read
 * generates fabric traffic with NO cacheable coherency/DVM transactions -- bus
 * warmth without snoop contention.
 *
 * /proc/fabwarm.1 = arm (keep-warm ON), .0 = disarm (parked -> normal idle),
 * bare = status + iteration counter. Default DISARMED (inert -- zero effect on
 * the shipped system) so it can be A/B-tested against the baseline on the SAME
 * boot via /proc/freezes + pingmon.
 */
#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4utils/thread.h>
#include <vspace/vspace.h>
#include "arch.h"                 /* arch_dsb */
#include "aios/root_shared.h"     /* vka, vspace, simple */
#include "aios/device_map.h"      /* dev_gpio_vaddr (RPi4; NULL on QEMU) */
#define LOG_MODULE "fabwarm"
#include "aios/aios_log.h"
#include "aios/cpuacct.h"

/* GPIO input-level register (0x34): read-only, no side effects, VideoCore-side
 * (0xFE200000) so the read crosses the ARM->VC AXI fabric. */
#define GPLEV0_IDX  (0x34 / 4)
/* Fabric-touch rate. One uncached read per ~1ms keeps the SCB AXI link out of
 * the quiesced state (the research minimum is "every tens of ms") while adding
 * negligible fabric load -- far below saturating it against GENET DMA. */
#define FABWARM_HZ  1000u

static volatile int      g_fabwarm_armed;
static seL4_CPtr         g_fabwarm_ntfn;
static volatile uint64_t g_fabwarm_iters;

/* One uncached read of a VideoCore-side register = one ARM->VC AXI transaction.
 * On QEMU dev_gpio_vaddr is NULL (no-op; the fabric freeze does not exist there). */
static inline void fabric_touch(void)
{
    volatile uint32_t *g = dev_gpio_vaddr;
    if (g) {
        (void)g[GPLEV0_IDX];
    }
}

/* Generic-timer counter -- a cheap userspace register read (no syscall, no
 * fabric crossing), used to pace the fabric touches. */
static inline uint64_t fabwarm_now(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

/* Pinned to core 1 (idle-only): busy-warms the shared SCB fabric. Parks when
 * disarmed.
 *
 * CRITICAL: this is a PURE USERSPACE spin -- it must NOT call seL4_Yield (or any
 * syscall) in the hot loop. A yield-per-iteration hammers the SMP big-kernel-lock
 * and degrades the whole system. core_warm proves a syscall-free busy-loop on
 * cores 1-3 is safe (the corewarm A/Bs drove fine over netconsole). The only
 * non-idle work on core 1 is this thread, so monopolizing core 1 is harmless --
 * unlike core 0, where a busy-loop hung the blocked shell (HW-confirmed). */
static void fabric_warm_fn(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (!freq) {
        freq = 54000000;
    }
    uint64_t interval = freq / FABWARM_HZ;   /* counter ticks between touches */
    for (;;) {
        seL4_Wait(g_fabwarm_ntfn, NULL);     /* park until armed */
        uint64_t last = fabwarm_now();
        while (g_fabwarm_armed) {
            uint64_t now = fabwarm_now();    /* cheap reg read, no syscall */
            if (now - last >= interval) {
                fabric_touch();
                g_fabwarm_iters++;
                last = now;
            }
        }
    }
}

/* Spawn the keep-warm thread (boot thread: vka/vspace are single-owner, so the
 * one allocation happens HERE). Inert until armed via /proc/fabwarm.1. */
void fabric_warm_start(void)
{
    vka_object_t nobj;
    if (vka_alloc_notification(&vka, &nobj)) {
        printf("[fabwarm] no ntfn -- keep-warm unavailable\n");
        return;
    }
    g_fabwarm_ntfn = nobj.cptr;

    sel4utils_thread_t th;
    if (sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
            simple_get_cnode(&simple), seL4_NilData, &th)) {
        printf("[fabwarm] configure failed -- keep-warm unavailable\n");
        return;
    }
    seL4_TCB_SetPriority(th.tcb.cptr, simple_get_tcb(&simple), 1);   /* lowest non-idle */
#if CONFIG_MAX_NUM_NODES > 1
    seL4_TCB_SetAffinity(th.tcb.cptr, 1);                            /* core 1: idle-only, never starves core 0 */
#endif
    sel4utils_start_thread(&th, fabric_warm_fn, NULL, NULL, 1);
    aios_acct_register("fabwarm", th.tcb.cptr);

    /* v0.4.267: DEFAULT-ON. The "Linux approach" is now the active fix for the
     * idle-teardown DVM freeze -- arm the keep-warm at boot so the SCB fabric
     * stays warm without manual intervention. `/proc/fabwarm.0` disables it
     * (reverts to the mitigations-only baseline -- nodes=4 + masked shootdown +
     * clock floor); `/proc/fabwarm.1` re-arms. Validated empirically via
     * /proc/freezes (the freeze is too rare for a quick A/B). */
    g_fabwarm_armed = 1;
    arch_dsb();                          /* publish the flag before the wake */
    seL4_Signal(g_fabwarm_ntfn);
    printf("[fabwarm] SCB-fabric keep-warm ARMED on core 1 (default-on; /proc/fabwarm.0 to disable)\n");
}

/* /proc/fabwarm[.0|.1] -- the live A/B knob. .1 = arm (core 1 keeps the fabric
 * warm), .0 = disarm (core 1 returns to idle), bare = status. */
int fabric_warm_cmd(const char *args, char *buf, int bufsize)
{
    if (args[0] == '.' && args[1] == '1') {
        g_fabwarm_armed = 1;
        arch_dsb();                                  /* publish the flag before the wake */
        if (g_fabwarm_ntfn) {
            seL4_Signal(g_fabwarm_ntfn);
        }
        return snprintf(buf, bufsize, "fabwarm: ARMED -- core 1 keeping the SCB fabric warm\n");
    }
    if (args[0] == '.' && args[1] == '0') {
        g_fabwarm_armed = 0;
        arch_dsb();
        return snprintf(buf, bufsize, "fabwarm: disarmed -- core 1 returning to idle\n");
    }
    return snprintf(buf, bufsize,
        "fabwarm: armed=%d iters=%llu gpio=%p (.1 arm / .0 disarm)\n",
        g_fabwarm_armed, (unsigned long long)g_fabwarm_iters, (void *)dev_gpio_vaddr);
}
