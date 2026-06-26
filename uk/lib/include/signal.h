/* signal.h -- AIOS shadow header. Signal NUMBERS match Linux (the host-generated dash signames.c
 * bakes in Linux numbers, so these must agree). Dispositions are RECORDED but not yet delivered --
 * there is no async signal path through the PAL yet; enough for dash to install handlers + run -c
 * scripts (nothing fires). A real delivery path is future work. */
#ifndef _SIGNAL_H
#define _SIGNAL_H
#include <sys/types.h>

typedef int          sig_atomic_t;
typedef unsigned long sigset_t;
typedef void (*__sighandler_t)(int);

#define SIG_ERR ((__sighandler_t)-1)
#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGIO    29
#define SIGSYS   31
#define NSIG     65
#define _NSIG    65

/* sigprocmask how */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* sigaction sa_flags */
#define SA_RESTART   0x10000000
#define SA_NOCLDSTOP 0x00000001
#define SA_SIGINFO   0x00000004

struct sigaction {
    __sighandler_t sa_handler;
    sigset_t       sa_mask;
    int            sa_flags;
};

__sighandler_t signal(int sig, __sighandler_t handler);
int  sigaction(int sig, const struct sigaction *act, struct sigaction *old);
int  kill(int pid, int sig);
int  killpg(int pgrp, int sig);
int  raise(int sig);
int  sigemptyset(sigset_t *set);
int  sigfillset(sigset_t *set);
int  sigaddset(sigset_t *set, int sig);
int  sigdelset(sigset_t *set, int sig);
int  sigismember(const sigset_t *set, int sig);
int  sigprocmask(int how, const sigset_t *set, sigset_t *old);
int  sigsuspend(const sigset_t *mask);

#endif /* _SIGNAL_H */
