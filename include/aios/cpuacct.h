#ifndef AIOS_CPUACCT_H
#define AIOS_CPUACCT_H
/*
 * cpuacct.h -- per-thread CPU-cycle accounting (/proc/cpuacct).
 *
 * Built on the seL4 scheduler BENCHMARK_TRACK_UTILISATION accounting: the
 * kernel records cycles + schedule counts per TCB on every context switch
 * (KernelBenchmarks=track_utilisation, settings-rpi4.cmake / settings.cmake).
 * The root registers each long-lived thread (name -> TCB cptr) at spawn;
 * /proc/cpuacct queries seL4_BenchmarkGetThreadUtilisation per TCB.
 */
#include <sel4/sel4.h>

/* Register a long-lived thread for /proc/cpuacct. Called at spawn with the
 * the thread TCB cptr (sel4utils_thread_t.tcb.cptr, or seL4_CapInitThreadTCB for
 * the root). A NULL/0 cptr or table-full is ignored. The name string must be
 * long-lived (a literal). */
void aios_acct_register(const char *name, seL4_CPtr tcb);

/* Enable kernel utilisation accounting (seL4_BenchmarkResetLog). Call once at
 * boot, after the servers are registered. */
void aios_acct_init(void);

/* DVFS governor load sample (src/cpu_gov.c). Returns the permille (0..1000) of
 * core-0 cycles spent on real work since the previous call: the SUM of the
 * event-driven work servers (pipe / fs / exec / net / ... -- everything but the
 * background spinners/pollers root / tlbi_probe / serverstats / flush), over the
 * PMCCNTR total. Summed positively, NOT total-minus-background: track_utilisation
 * books cycles at switch-out, so the no-WFI idle spinners under-report and a
 * total-minus-background metric reads the idle spin as work (HW-confirmed). The
 * work servers block on seL4_Recv, so they are booked accurately and read ~0 at
 * idle. A forked user proc (tcc) is unregistered but its syscall traffic lights
 * up pipe/fs/exec, so a real compile still registers. Keeps its own baselines,
 * independent of cpuacct_render. The CCNT total is a modular 32-bit delta valid
 * under ~7s, so the governor samples ~1/s. Returns -1 if accounting is
 * unavailable or on the first priming call; fills total_out / work_out (cycles)
 * for diagnostics when non-NULL. */
int aios_acct_busy_permille(uint64_t *total_out, uint64_t *work_out);

/* Render the CPU-accounting table into buf. Shows per-thread CPU cycles consumed
 * since the LAST read (deltas; the per-thread counts are 64-bit and accurate),
 * the idle thread (~always 0 on core 0 -- the root spins so the kernel idle
 * thread never runs there), and an "unaccounted" line (PMCCNTR total minus the
 * registered sum) that catches any unregistered thread. The CCNT total is 32-bit
 * and wraps in ~7s; past that the pct base falls back to the accounted sum and
 * the unaccounted line reads n/a. Returns bytes written. */
int cpuacct_render(char *buf, int sz);

#endif /* AIOS_CPUACCT_H */
