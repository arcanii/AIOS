#ifndef AIOS_CPUACCT_H
#define AIOS_CPUACCT_H
/*
 * cpuacct.h -- per-thread CPU-cycle accounting (/proc/cpuacct).
 *
 * Built on the seL4 scheduler's BENCHMARK_TRACK_UTILISATION accounting: the
 * kernel records cycles + schedule counts per TCB on every context switch
 * (KernelBenchmarks=track_utilisation, settings-rpi4.cmake / settings.cmake).
 * The root registers each long-lived thread (name -> TCB cptr) at spawn;
 * /proc/cpuacct queries seL4_BenchmarkGetThreadUtilisation per TCB.
 */
#include <sel4/sel4.h>

/* Register a long-lived thread for /proc/cpuacct. Called at spawn with the
 * thread's TCB cptr (sel4utils_thread_t.tcb.cptr, or seL4_CapInitThreadTCB for
 * the root). A NULL/0 cptr or table-full is ignored. The name string must be
 * long-lived (a literal). */
void aios_acct_register(const char *name, seL4_CPtr tcb);

/* Enable kernel utilisation accounting (seL4_BenchmarkResetLog). Call once at
 * boot, after the servers are registered. */
void aios_acct_init(void);

/* Render the CPU-accounting table into buf. Shows per-thread CPU cycles consumed
 * since the LAST read (deltas -- the CCNT is 32-bit so only short windows are
 * accurate), the idle thread, and an "unaccounted" line (PMCCNTR total minus the
 * registered sum) that catches any unregistered thread. Returns bytes written. */
int cpuacct_render(char *buf, int sz);

#endif /* AIOS_CPUACCT_H */
