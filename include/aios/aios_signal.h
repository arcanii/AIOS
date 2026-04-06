#ifndef AIOS_SIGNAL_H
#define AIOS_SIGNAL_H

/*
 * AIOS Signal Infrastructure
 *
 * Provides POSIX signal semantics on seL4.
 * Signal delivery is cooperative: pending signals are checked
 * on syscall return paths in the POSIX shim.
 *
 * Phase 1 (v0.4.53): sigaction/kill/sigprocmask syscalls,
 *   server-side dispatch for SIGKILL/SIGTERM/SIGINT/SIGCHLD.
 * Phase 2 (future): stack-based handler invocation via
 *   TCB register manipulation + signal trampoline.
 */

#include <stdint.h>

/* Number of signals we track (1..31) */
#define AIOS_NSIG 32

/* Bit manipulation for signal masks */
#define SIG_BIT(sig) (1U << ((sig) - 1))

/* Uncatchable / unstoppable signals */
#define SIG_UNCATCHABLE(sig) ((sig) == 9 || (sig) == 19)

/* Standard POSIX signal numbers (AArch64 Linux) */
#define AIOS_SIGHUP     1
#define AIOS_SIGINT     2
#define AIOS_SIGQUIT    3
#define AIOS_SIGILL     4
#define AIOS_SIGTRAP    5
#define AIOS_SIGABRT    6
#define AIOS_SIGBUS     7
#define AIOS_SIGFPE     8
#define AIOS_SIGKILL    9
#define AIOS_SIGUSR1   10
#define AIOS_SIGSEGV   11
#define AIOS_SIGUSR2   12
#define AIOS_SIGPIPE   13
#define AIOS_SIGALRM   14
#define AIOS_SIGTERM   15
#define AIOS_SIGCHLD   17
#define AIOS_SIGCONT   18
#define AIOS_SIGSTOP   19
#define AIOS_SIGTSTP   20
#define AIOS_SIGTTIN   21
#define AIOS_SIGTTOU   22

/*
 * Kernel-format sigaction structure.
 * This matches what musl passes to __NR_rt_sigaction on AArch64.
 * musl translates userspace struct sigaction to this layout
 * before invoking the syscall.
 */
typedef struct {
    void     (*sa_handler)(int);
    unsigned long sa_flags;
    void     (*sa_restorer)(void);
    unsigned long sa_mask;   /* 64-bit signal mask */
} aios_k_sigaction_t;

/*
 * Per-process signal state.
 * Stored in aios_posix.c (process-local data).
 * Each forked/execed process gets a fresh copy.
 */
typedef struct {
    aios_k_sigaction_t actions[AIOS_NSIG];
    uint64_t blocked;        /* blocked signal mask (sigprocmask) */
    uint32_t pending;        /* pending signals bitmask */
    int      initialized;
} aios_sigstate_t;

/* Initialize to POSIX defaults (SIG_DFL for all) */
static inline void aios_sigstate_init(aios_sigstate_t *ss) {
    for (int i = 0; i < AIOS_NSIG; i++) {
        ss->actions[i].sa_handler = (void (*)(int))0; /* SIG_DFL */
        ss->actions[i].sa_flags = 0;
        ss->actions[i].sa_restorer = (void (*)(void))0;
        ss->actions[i].sa_mask = 0;
    }
    ss->blocked = 0;
    ss->pending = 0;
    ss->initialized = 1;
}

#endif /* AIOS_SIGNAL_H */
