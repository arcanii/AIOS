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
 * feed the DVFS governor a work-load signal (aios_acct_busy_permille): the
 * fraction of core-0 cycles spent on real work = the PMCCNTR total minus the
 * idle-filler background threads (the root no-WFI spin + the periodic pollers).
 * That sidesteps the core-0 idle thread reading 0 -- which is expected, not a
 * bug: the root spins, so the kernel idle thread on core 0 never runs.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sel4/sel4.h>
#include "aios/cpuacct.h"

#ifdef CONFIG_BENCHMARK_TRACK_UTILISATION
#include <sel4/benchmark_utilisation_types.h>

#define ACCT_MAX 24
static struct acct_ent {
    const char *name;
    seL4_CPtr   tcb;
    uint64_t    last;       /* previous cumulative utilisation -- human render */
    uint64_t    last_gov;   /* previous cumulative utilisation -- governor sample */
    int         background; /* idle-filler thread, excluded from the governor load */
} acct[ACCT_MAX];
static int      acct_n = 0;
static int      acct_started = 0;
static int      acct_primed  = 0;
static uint64_t acct_last_idle = 0;
static uint32_t acct_last_ccnt = 0;
static uint64_t acct_last_ct   = 0;
/* the governor keeps its own CCNT/utilisation baselines so its ~1s sampling and
 * the human /proc/cpuacct read do not consume each other intervals. */
static int      gov_primed     = 0;
static uint32_t gov_last_ccnt  = 0;

/* The idle-filler / periodic threads that run regardless of user workload: the
 * root no-WFI spin (the stall cure) plus the slow background pollers. The DVFS
 * governor subtracts these so its load metric tracks real work (pipe / fs / exec
 * / net plus any unregistered forked user procs), not the idle spin. */
static int name_is_background(const char *n) {
    return (!strcmp(n, "root") ||
            !strcmp(n, "serverstats") || !strcmp(n, "flush"));
}

void aios_acct_register(const char *name, seL4_CPtr tcb) {
    if (!tcb || acct_n >= ACCT_MAX) return;
    acct[acct_n].name = name;
    acct[acct_n].tcb  = tcb;
    acct[acct_n].last = 0;
    acct[acct_n].last_gov   = 0;
    acct[acct_n].background = name_is_background(name);
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
/* generic timer: fixed-rate (clock-independent), 64-bit -- used to measure the
 * wall-clock length of a window so a wrapped CCNT total can be detected. */
static inline uint64_t rd_cntpct(void) {
    uint64_t v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v;
}
static inline uint64_t rd_cntfrq(void) {
    uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}

static uint64_t thread_util(seL4_CPtr tcb, uint64_t *idle_out) {
    seL4_BenchmarkGetThreadUtilisation(tcb);
    if (idle_out) *idle_out = (uint64_t)seL4_GetMR(BENCHMARK_IDLE_LOCALCPU_UTILISATION);
    return (uint64_t)seL4_GetMR(BENCHMARK_TCB_UTILISATION);
}

int aios_acct_busy_permille(uint64_t *total_out, uint64_t *work_out) {
    if (!acct_started) return -1;
    uint32_t ccnt = rd_ccnt();
    if (!gov_primed) {
        gov_last_ccnt = ccnt;
        for (int i = 0; i < acct_n; i++)
            acct[i].last_gov = thread_util(acct[i].tcb, 0);
        gov_primed = 1;
        return -1;   /* first call only primes the baseline -- no interval yet */
    }
    uint32_t total = ccnt - gov_last_ccnt;   /* modular 32-bit; valid for < ~7s */
    gov_last_ccnt = ccnt;

    /* Sum the WORK servers (everything but the background) POSITIVELY rather than
     * total-minus-background. seL4 track_utilisation books a TCB cycles only when
     * it is switched OUT, so the no-WFI root/tlbi idle spinners (which seldom
     * deschedule) under-report -- their cycles show in the PMCCNTR total but not
     * in their TCB, so total-minus-background would count the idle spin as work
     * and never read idle (HW-confirmed: bmin stuck ~440 over a disconnected
     * idle). The work servers are event-driven (block on seL4_Recv), so they ARE
     * booked accurately and read ~0 when idle and high under load. A forked user
     * proc (tcc) is unregistered, but its syscall traffic lights up pipe/fs/exec,
     * so a real compile still registers as work. */
    uint64_t work = 0;
    for (int i = 0; i < acct_n; i++) {
        uint64_t util = thread_util(acct[i].tcb, 0);
        uint64_t d = util - acct[i].last_gov;
        acct[i].last_gov = util;
        if (!acct[i].background) work += d;
    }
    if (work > total) work = total;          /* window skew clamp */
    if (total_out) *total_out = total;
    if (work_out)  *work_out = work;
    if (total == 0) return 0;
    return (int)((work * 1000u) / total);
}

int cpuacct_render(char *buf, int sz) {
    if (!acct_started)
        return snprintf(buf, sz, "cpuacct: not started\n");

    uint32_t ccnt = rd_ccnt();
    uint64_t ct   = rd_cntpct();

    /* First read just primes the baselines -- a since-boot delta is meaningless. */
    if (!acct_primed) {
        acct_last_ccnt = ccnt;
        acct_last_ct   = ct;
        for (int i = 0; i < acct_n; i++)
            acct[i].last = thread_util(acct[i].tcb, i == 0 ? &acct_last_idle : 0);
        acct_primed = 1;
        return snprintf(buf, sz,
            "cpuacct: baseline primed (%d threads) -- read again for CPU cycles "
            "over the interval\n", acct_n);
    }

    uint32_t total = ccnt - acct_last_ccnt;   /* modular 32-bit */
    acct_last_ccnt = ccnt;
    uint64_t frq = rd_cntfrq(); if (!frq) frq = 54000000u;
    uint64_t wall_ms = ((ct - acct_last_ct) * 1000u) / frq;
    acct_last_ct = ct;
    /* PMCCNTR is 32-bit and wraps in ~7.16s at the 600MHz ceiling; past that the
     * total (so the pct column and the unaccounted line) is unreliable. The
     * per-thread cycle counts are 64-bit accumulators and stay accurate, so fall
     * the pct base back to the accounted sum and suppress unaccounted. */
    int total_ok = (wall_ms < 7000u);

    /* Gather all deltas first so the pct base can fall back when CCNT has wrapped. */
    uint64_t deltas[ACCT_MAX];
    uint64_t idle = 0, sum = 0;
    for (int i = 0; i < acct_n; i++) {
        uint64_t util = thread_util(acct[i].tcb, i == 0 ? &idle : 0);
        deltas[i] = util - acct[i].last;
        acct[i].last = util;
        sum += deltas[i];
    }
    uint64_t idle_d = idle - acct_last_idle;
    acct_last_idle = idle;

    uint64_t base = total_ok ? (uint64_t)total : (sum + idle_d);
    if (base == 0) base = 1;

    int w = 0;
    w += snprintf(buf + w, sz - w,
        "cpuacct: core-0 CPU cycles over %llums.  total=%u%s\n",
        (unsigned long long)wall_ms, total,
        total_ok ? "" : " (CCNT wrapped: pct vs accounted sum)");
    w += snprintf(buf + w, sz - w, "%-16s %14s %4s\n", "thread", "cycles", "pct");

    for (int i = 0; i < acct_n; i++) {
        unsigned pct = (unsigned)((deltas[i] * 100u) / base);
        w += snprintf(buf + w, sz - w, "%-16s %14llu %3u%%\n",
                      acct[i].name, (unsigned long long)deltas[i], pct);
    }

    /* core-0 idle is ~always 0: the root spins (no-WFI stall cure) so the kernel
     * idle thread on core 0 never runs. Shown for completeness. */
    unsigned ipct = (unsigned)((idle_d * 100u) / base);
    w += snprintf(buf + w, sz - w, "%-16s %14llu %3u%%\n",
                  "(idle)", (unsigned long long)idle_d, ipct);

    if (total_ok) {
        uint64_t accounted = idle_d + sum;
        uint64_t un = (total > accounted) ? (total - accounted) : 0;
        unsigned upct = (unsigned)((un * 100u) / base);
        w += snprintf(buf + w, sz - w, "%-16s %14llu %3u%%\n",
                      "(unaccounted)", (unsigned long long)un, upct);
    } else {
        w += snprintf(buf + w, sz - w, "%-16s %14s\n", "(unaccounted)", "n/a");
    }
    return w;
}

#else  /* !CONFIG_BENCHMARK_TRACK_UTILISATION */

void aios_acct_register(const char *name, seL4_CPtr tcb) { (void)name; (void)tcb; }
void aios_acct_init(void) {}
int  aios_acct_busy_permille(uint64_t *total_out, uint64_t *bg_out) {
    (void)total_out; (void)bg_out; return -1;
}
int  cpuacct_render(char *buf, int sz) {
    return snprintf(buf, sz,
        "cpuacct: unavailable (build with KernelBenchmarks=track_utilisation)\n");
}

#endif
