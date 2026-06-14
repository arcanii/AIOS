/*
 * cpuacct.c -- per-thread CPU-cycle accounting (/proc/cpuacct).
 *
 * The seL4 scheduler, with KernelBenchmarks=track_utilisation, accumulates
 * cycles + schedule counts per TCB on every context switch (a 64-bit per-TCB
 * accumulator of 32-bit CCNT deltas -- essentially wrap-free; BCM2711 has no
 * KERNEL_PMU_IRQ so it loses ~0.1% on a straddled wrap, fine for accounting).
 * This module keeps a name->TCB-cptr registry and renders a top-style table of
 * cycles consumed since the previous read (deltas), the idle thread (free with
 * any query), and an "unaccounted" remainder from an independent PMCCNTR total
 * so an unregistered hog still shows up. Built to find what eats core 0 and to
 * give the DVFS governor a real all-core idle signal.
 */
#include <stdint.h>
#include <stdio.h>
#include <sel4/sel4.h>
#include "aios/cpuacct.h"

#ifdef CONFIG_BENCHMARK_TRACK_UTILISATION
#include <sel4/benchmark_utilisation_types.h>

#define ACCT_MAX 24
static struct acct_ent {
    const char *name;
    seL4_CPtr   tcb;
    uint64_t    last;   /* previous cumulative utilisation (cycles) */
} acct[ACCT_MAX];
static int      acct_n = 0;
static int      acct_started = 0;
static int      acct_primed  = 0;
static uint64_t acct_last_idle = 0;
static uint32_t acct_last_ccnt = 0;

void aios_acct_register(const char *name, seL4_CPtr tcb) {
    if (!tcb || acct_n >= ACCT_MAX) return;
    acct[acct_n].name = name;
    acct[acct_n].tcb  = tcb;
    acct[acct_n].last = 0;
    acct_n++;
}

void aios_acct_init(void) {
    seL4_BenchmarkResetLog();    /* enable per-TCB utilisation accounting */
    acct_started = 1;
}

/* core-0 PMU cycle counter (KernelArmExportPCNTUser exports it to EL0). 32-bit. */
static inline uint32_t rd_ccnt(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, pmccntr_el0" : "=r"(v));
    return (uint32_t)v;
}

static uint64_t thread_util(seL4_CPtr tcb, uint64_t *idle_out) {
    seL4_BenchmarkGetThreadUtilisation(tcb);
    if (idle_out) *idle_out = (uint64_t)seL4_GetMR(BENCHMARK_IDLE_LOCALCPU_UTILISATION);
    return (uint64_t)seL4_GetMR(BENCHMARK_TCB_UTILISATION);
}

int cpuacct_render(char *buf, int sz) {
    if (!acct_started)
        return snprintf(buf, sz, "cpuacct: not started\n");

    uint32_t ccnt = rd_ccnt();

    /* First read just primes the baselines -- a since-boot delta is meaningless. */
    if (!acct_primed) {
        acct_last_ccnt = ccnt;
        for (int i = 0; i < acct_n; i++)
            acct[i].last = thread_util(acct[i].tcb, i == 0 ? &acct_last_idle : 0);
        acct_primed = 1;
        return snprintf(buf, sz,
            "cpuacct: baseline primed (%d threads) -- read again for CPU cycles "
            "over the interval\n", acct_n);
    }

    uint32_t total = ccnt - acct_last_ccnt;   /* modular 32-bit; valid < ~7s */
    acct_last_ccnt = ccnt;

    int w = 0;
    w += snprintf(buf + w, sz - w,
        "cpuacct: core-0 CPU cycles since last read.  total=%u\n", total);
    w += snprintf(buf + w, sz - w, "%-16s %14s %4s\n", "thread", "cycles", "pct");

    uint64_t idle = 0, sum = 0;
    for (int i = 0; i < acct_n; i++) {
        uint64_t util = thread_util(acct[i].tcb, i == 0 ? &idle : 0);
        uint64_t d = util - acct[i].last;
        acct[i].last = util;
        sum += d;
        unsigned pct = total ? (unsigned)((d * 100u) / total) : 0;
        w += snprintf(buf + w, sz - w, "%-16s %14llu %3u%%\n",
                      acct[i].name, (unsigned long long)d, pct);
    }

    uint64_t idle_d = idle - acct_last_idle;
    acct_last_idle = idle;
    unsigned ipct = total ? (unsigned)((idle_d * 100u) / total) : 0;
    w += snprintf(buf + w, sz - w, "%-16s %14llu %3u%%\n",
                  "(idle)", (unsigned long long)idle_d, ipct);

    uint64_t accounted = idle_d + sum;
    uint64_t un = (total > accounted) ? (total - accounted) : 0;
    unsigned upct = total ? (unsigned)((un * 100u) / total) : 0;
    w += snprintf(buf + w, sz - w, "%-16s %14llu %3u%%\n",
                  "(unaccounted)", (unsigned long long)un, upct);
    return w;
}

#else  /* !CONFIG_BENCHMARK_TRACK_UTILISATION */

void aios_acct_register(const char *name, seL4_CPtr tcb) { (void)name; (void)tcb; }
void aios_acct_init(void) {}
int  cpuacct_render(char *buf, int sz) {
    return snprintf(buf, sz,
        "cpuacct: unavailable (build with KernelBenchmarks=track_utilisation)\n");
}

#endif
