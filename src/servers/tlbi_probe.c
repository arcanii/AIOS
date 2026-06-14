/* tlbi_probe.c -- v0.4.216 diag: hammer seL4_ARM_Page_Unmap/Map on one page
 * and report any single unmap exceeding 250ms. The 32.4/43.2s freezes bracket
 * to the kernel TLBI+DSB; this isolates whether they need system I/O at all. */
#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4utils/thread.h>
#include <vspace/vspace.h>
#include "aios/root_shared.h"
#define LOG_MODULE "tlbi"
#include "aios/aios_log.h"
#include "aios/cpuacct.h"

static uint64_t tp_ticks(void) {
    uint64_t c; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(c)); return c;
}
static uint64_t tp_ms(uint64_t a, uint64_t b) {
    uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (!f) f = 54000000;
    return (b - a) * 1000 / f;
}

/* Page set up on the BOOT thread (vka/vspace are single-owner; allocating from
 * the probe thread would race boot-time allocations -- known abort mode). */
static void *tp_page_v;
static seL4_CPtr tp_page_cap;

static void tlbi_probe_fn(void *a, void *b, void *c) {
    (void)a; (void)b; (void)c;
    void *v = tp_page_v;
    seL4_CPtr cap = tp_page_cap;
    printf("[tlbi] probe up: hammering unmap/map at %p\n", v);
    uint32_t round = 0;
    uint64_t last_beat = tp_ticks();
    for (;;) {
        uint64_t worst = 0;
        for (int i = 0; i < 50; i++) {
            uint64_t t0 = tp_ticks();
            seL4_ARM_Page_Unmap(cap);
            uint64_t t1 = tp_ticks();
            seL4_ARM_Page_Map(cap, seL4_CapInitThreadVSpace, (seL4_Word)v,
                              seL4_AllRights, seL4_ARM_Default_VMAttributes);
            uint64_t dms = tp_ms(t0, t1);
            if (dms > worst) worst = dms;
        }
        round++;
        if (worst > 250)
            printf("[tlbi] SLOW round=%u worst_unmap=%llums\n",
                   round, (unsigned long long)worst);
        if (tp_ms(last_beat, tp_ticks()) > 30000) {
            printf("[tlbi] alive rounds=%u\n", round);
            last_beat = tp_ticks();
        }
        /* ~2s pause between rounds (yield-loop; root threads never block here) */
        uint64_t dl = tp_ticks();
        while (tp_ms(dl, tp_ticks()) < 2000) seL4_Yield();
    }
}

void tlbi_probe_start(void) {
    tp_page_v = vspace_new_pages(&vspace, seL4_AllRights, 1, seL4_PageBits);
    if (!tp_page_v) { printf("[tlbi] probe: no page\n"); return; }
    tp_page_cap = vspace_get_cap(&vspace, tp_page_v);
    if (!tp_page_cap) { printf("[tlbi] probe: no cap\n"); return; }
    sel4utils_thread_t thread;
    if (sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
        simple_get_cnode(&simple), seL4_NilData, &thread)) {
        printf("[tlbi] probe thread configure failed\n");
        return;
    }
    seL4_TCB_SetPriority(thread.tcb.cptr, simple_get_tcb(&simple), 200);
    sel4utils_start_thread(&thread, tlbi_probe_fn, NULL, NULL, 1);
    aios_acct_register("tlbi_probe", thread.tcb.cptr);   /* /proc/cpuacct */
}
