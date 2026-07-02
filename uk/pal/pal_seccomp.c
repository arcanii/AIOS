/*
 * pal_seccomp.c -- a SECOND PAL Linux backend, seccomp SECCOMP_RET_TRACE trap FRONT-END.
 *
 * This is the portability proof: the AIOS userspace kernel (kernel/aios_kernel.c) runs UNCHANGED
 * over a DIFFERENT host trap mechanism, with only the PAL swapped (`make PAL=seccomp`). Where
 * pal_linux.c traps via PTRACE_SYSCALL (every syscall stops, SYSEMU-style), this backend installs a
 * seccomp BPF filter that classifies the guest's syscalls and traps ONLY them, via
 * SECCOMP_RET_TRACE -> PTRACE_EVENT_SECCOMP; the guest runs via PTRACE_CONT in between. That is the
 * defining difference -- the syscall-interception HOT PATH is BPF-filtered seccomp, not blanket
 * ptrace.
 *
 * The two backends share the entire Linux host-driver + ptrace INJECTOR core (pal_linux_common.c):
 * Linux offers NO userspace-only primitive to inject memory/processes or rewrite another process's
 * registers, so mmap/exec/fork/exit + the signal-frame dance are necessarily ptrace either way --
 * that is a property of the host, not of the seam. seccomp changes how an EMULATED syscall (read/
 * write/open/stat/... -- the bulk) is intercepted. The injectors run at a PTRACE_EVENT_SECCOMP stop
 * (syscall entry, after the seccomp check), so rewriting the syscall number there dispatches the
 * real host syscall WITHOUT re-running the filter -- exactly the SYSEMU inject, reached differently.
 *
 * The one knob the shared injectors need is PAL_RESUME(pid) = "resume to the next trap"; here that
 * is PTRACE_CONT (the guest stops only at its own seccomp-filtered syscalls). PAL_TRACE_OPTS adds
 * PTRACE_O_TRACESECCOMP so SECCOMP_RET_TRACE surfaces as a PTRACE_EVENT_SECCOMP stop.
 */
#define _GNU_SOURCE

/* This backend resumes a guest "to its next trap" by PTRACE_CONT -- it stops only at a seccomp-
 * filtered syscall (a PTRACE_EVENT_SECCOMP stop), not at every syscall. */
#define PAL_RESUME(pid) ptrace(PTRACE_CONT, (pid), 0, 0)

/* Like pal_linux's options PLUS PTRACE_O_TRACESECCOMP: a SECCOMP_RET_TRACE action is delivered to
 * the tracer as a PTRACE_EVENT_SECCOMP stop only when this is set (else the filtered syscall would
 * simply run). TRACESYSGOOD still tags the syscall-EXIT stops the injectors step to internally. All
 * the PTRACE_O_* names come from <sys/ptrace.h> (pulled in by the common core below) as ENUM
 * constants -- do NOT #define a fallback for them here: that would fire (an enum is not a macro) and
 * mangle the enum declaration. PAL_TRACE_OPTS is only EXPANDED after that header, so the enums are in
 * scope at every use site. */
#define PAL_TRACE_OPTS (PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL | PTRACE_O_TRACEEXEC | \
                        PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE | \
                        PTRACE_O_TRACESECCOMP)

#include "pal_linux_common.c"   /* the SHARED Linux host-driver + ptrace injector core */

#include <stddef.h>             /* offsetof (seccomp_data field offsets) */
#include <sys/prctl.h>          /* PR_SET_NO_NEW_PRIVS */
#include <linux/audit.h>        /* AUDIT_ARCH_AARCH64 */
#include <linux/filter.h>       /* struct sock_filter / sock_fprog + BPF_* */
#include <linux/seccomp.h>      /* SECCOMP_* + struct seccomp_data */

#ifndef PTRACE_EVENT_SECCOMP
#define PTRACE_EVENT_SECCOMP 7
#endif
#ifndef SECCOMP_SET_MODE_FILTER
#define SECCOMP_SET_MODE_FILTER 1
#endif
#ifndef SECCOMP_RET_TRACE
#define SECCOMP_RET_TRACE 0x7ff00000U
#endif
#ifndef SECCOMP_RET_KILL_PROCESS
#  ifdef SECCOMP_RET_KILL_THREAD
#    define SECCOMP_RET_KILL_PROCESS SECCOMP_RET_KILL_THREAD
#  else
#    define SECCOMP_RET_KILL_PROCESS 0x00000000U
#  endif
#endif

/* Install the seccomp filter in the (to-be-)guest: TRACE every syscall to the tracer. The guest
 * never emits a real Linux syscall in normal operation -- libaios issues only AIOS svc numbers
 * (>= 0x1000) -- so trapping everything routes each AIOS syscall to the kernel for service AND makes
 * the M4 escape policy explicit: a real-Linux syscall the guest tries to smuggle (e.g. guest_escape's
 * raw write) also traps, the kernel sees nr < 0x1000 and kills it. The kernel's OWN injected syscalls
 * (mmap/clone/execve/exit_group) are written at the PTRACE_EVENT_SECCOMP stop, AFTER the filter ran,
 * so they dispatch without re-filtering. NO_NEW_PRIVS lets an unprivileged process install a filter
 * (so this runs as plain user `pi` on the Pi, like the rest of the unprivileged confinement). */
static void install_seccomp_filter(void) {
    struct sock_filter filt[] = {
        BPF_STMT(BPF_LD  | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),   /* wrong arch -> kill (defensive) */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),          /* everything else -> trap to the tracer */
    };
    struct sock_fprog prog = { .len = (unsigned short)(sizeof filt / sizeof filt[0]), .filter = filt };
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog);
}

pal_pid_t pal_guest_spawn(const char *path, char *const argv[]) {
    pal_fs_init_once();                          /* M4.2: establish the AIOS root (AIOS_ROOT) once, up front */
    umask(0);                                    /* the kernel owns the per-process umask */
    signal(SIGPIPE, SIG_IGN);                    /* a guest broken-pipe write must surface as PAL_EPIPE */
    /* Catch the terminal signals so the kernel survives ^C/^Z and forwards them to the foreground
     * group (the guests are moved off the kernel's host pgrp below). Identical to the SYSEMU backend. */
    struct sigaction sa; sa.sa_handler = pal_term_handler; sa.sa_flags = 0; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    pal_net_init_signals();                      /* SIGCHLD plumbing for the socket-readiness co-wait */
    pid_t pid = fork();
    if (pid < 0) return PAL_PID_NONE;
    if (pid == 0) {
        /* Child becomes the guest. Ask to be traced, leave the kernel's host process group, then
         * STOP so the tracer can set PTRACE_O_TRACESECCOMP BEFORE the filter starts trapping (else
         * the first filtered syscall would just run). After the tracer continues us, install the
         * filter and exec -- the execve is the first filtered syscall (handled by the tracer below). */
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        setpgid(0, 0);                           /* own host process group -- off the kernel's, off the pty fg */
        raise(SIGSTOP);                          /* handshake: let the tracer set options before the filter bites */
        install_seccomp_filter();                /* NNP + SECCOMP_RET_TRACE everything */
        execv(path, argv);
        _exit(127);                              /* exec failed */
    }
    int st;
    if (waitpid_r(pid, &st, 0) < 0) return PAL_PID_NONE;   /* the SIGSTOP group-stop */
    if (!WIFSTOPPED(st)) return PAL_PID_NONE;
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PAL_TRACE_OPTS);   /* incl TRACESECCOMP, before the filter runs */
    ptrace(PTRACE_CONT, pid, 0, 0);              /* let the child install the filter + execve */
    /* Run THROUGH the load: the execve traps via the now-active filter (a PTRACE_EVENT_SECCOMP stop)
     * -- let it proceed -- then PTRACE_O_TRACEEXEC turns the successful exec into PTRACE_EVENT_EXEC,
     * where the new image is live and stopped at its entry. (Any other benign stop in this window is
     * just continued.) */
    for (;;) {
        if (waitpid_r(pid, &st, 0) < 0) return PAL_PID_NONE;
        if (WIFEXITED(st) || WIFSIGNALED(st)) return PAL_PID_NONE;   /* execve failed / died */
        if (WIFSTOPPED(st) && (st >> 8) == (SIGTRAP | (PTRACE_EVENT_EXEC << 8))) break;
        ptrace(PTRACE_CONT, pid, 0, 0);          /* the execve seccomp-trap (or a stray stop) -> proceed */
    }
    return (pal_pid_t)pid;                        /* stopped at the post-exec landing; the kernel resumes it */
}

/* Wait for the next event from ANY live guest. Same contract + return codes as the SYSEMU backend
 * (1 syscall / 0 exit / 2 async signal / 3 terminal signal / -1 none), but a guest syscall arrives
 * as a PTRACE_EVENT_SECCOMP stop (always at syscall entry) rather than a SIGTRAP|0x80 syscall stop.
 * Because the guest runs via PTRACE_CONT, it never stops at a syscall EXIT here -- those happen only
 * inside the injectors' own PTRACE_SYSCALL steps, consumed locally -- so there is no exit artifact to
 * skip; only seccomp events, exits, fork/exec event stops (consumed by the injectors), and signals. */
int pal_guest_next(pal_pid_t *who, pal_syscall_t *sc, int *exit_code) {
    for (;;) {
        if (g_term_sig) { *who = PAL_PID_NONE; if (exit_code) *exit_code = pal_take_term_signal(); return 3; }
        int st; pid_t pid;
        if (pal_net_have_watches()) {                  /* a guest is parked on a socket: co-wait events + readiness */
            pid = waitpid(-1, &st, WNOHANG);           /* collect a pending guest event without blocking... */
            if (pid == 0) {                            /* ...none pending -> block until a socket is ready or a guest stops */
                if (pal_net_wait_ready()) { *who = PAL_PID_NONE; return 4; }
                continue;                              /* woke on SIGCHLD/^C/timeout -> re-check term + guest events */
            }
            if (pid < 0) { if (errno == EINTR) continue; return -1; }   /* ECHILD (no guests) / error */
        } else {
            pid = waitpid(-1, &st, 0);
            if (pid < 0) { if (errno == EINTR) continue; return -1; }   /* EINTR (^C): loop -> g_term_sig returns 3 */
        }
        if (WIFEXITED(st))   { *who = pid; if (exit_code) *exit_code = WEXITSTATUS(st);    return 0; }
        if (WIFSIGNALED(st)) { *who = pid; if (exit_code) *exit_code = 128 + WTERMSIG(st); return 0; }
        if (!WIFSTOPPED(st)) continue;
        int sig = WSTOPSIG(st);
        if ((st >> 8) == (SIGTRAP | (PTRACE_EVENT_SECCOMP << 8))) {  /* a guest syscall trapped via seccomp */
            struct user_pt_regs r;
            if (getregs(pid, &r) != 0) return -1;
            sc->nr = pal_trapped_nr(&r);               /* GATEWAY decode: x8==gateway ? x9 : x8 (escape) */
            for (int i = 0; i < 6; i++) sc->arg[i] = (uint64_t)r.regs[i];
            *who = pid;
            return 1;                                  /* syscall ENTRY (seccomp event) */
        }
        /* Other ptrace EVENT stops (exec/fork/vfork/clone + a new child's initial stop) are consumed
         * inside the inject primitives; a SIGTRAP / group-stop that surfaces here is a stray -- swallow
         * it (PTRACE_CONT). A real async signal is reported to the kernel (handler / ignore / terminate). */
        if (sig == SIGTRAP || sig == SIGSTOP) { ptrace(PTRACE_CONT, pid, 0, 0); continue; }
        *who = pid;
        if (exit_code) *exit_code = sig;
        return 2;                                          /* async signal `sig` on *who */
    }
}
