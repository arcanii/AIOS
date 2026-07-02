/*
 * pal_linux.c -- the PAL Linux backend, SYSEMU-style trap FRONT-END.
 *
 * The gVisor-style trap model via ptrace(PTRACE_SYSCALL) for MANY guest processes at once. Each
 * guest is a traced Linux child; the kernel keys everything by pal_pid_t (= the tracee pid). The
 * loop is "wait for the next event from any guest (pal_guest_next, a waitpid(-1)), then service +
 * resume THAT guest".
 *
 * This file is now ONLY the trap mechanism: spawn (fork+TRACEME+exec), pal_guest_next (classify each
 * ptrace stop), and how a guest is resumed "to its next trap" (PTRACE_SYSCALL: it stops at every
 * syscall). Everything else -- the host-gateway I/O, the M4.2 confinement, and the ptrace injectors
 * (mmap/exec/fork/exit + the signal-frame dance) -- is the SHARED Linux host-driver core in
 * pal_linux_common.c, included below, parameterized by PAL_RESUME + PAL_TRACE_OPTS. The seccomp
 * front-end (pal_seccomp.c) reuses that exact core with a different trap mechanism; the AIOS kernel
 * above does not change either way (see pal_linux_common.c's header).
 *
 * AIOS-numbered syscalls (>= 0x1000) are not real Linux syscalls, so if one runs it just ENOSYSes
 * harmlessly and we plant the real AIOS result in x0 at the exit. PTRACE_SYSCALL (not SYSEMU) is
 * what lets the kernel INJECT a real host syscall (mmap/execve/clone/exit_group) by rewriting the
 * trapped svc in place. The driver never ASSUMES strict entry/exit alternation: it classifies each
 * stop with PTRACE_GET_SYSCALL_INFO and resumes past anything that is not a genuine syscall entry,
 * so the stray exit/event stops that injection leaves behind never desynchronise dispatch.
 */
#define _GNU_SOURCE

/* This backend resumes a guest "to its next trap" by PTRACE_SYSCALL -- it stops at EVERY syscall. */
#define PAL_RESUME(pid) ptrace(PTRACE_SYSCALL, (pid), 0, 0)

/* Options set on every tracee (init + each forked child): syscall-stops as SIGTRAP|0x80
 * (TRACESYSGOOD), the guest dies if the kernel dies (EXITKILL), clean exec-event stops
 * (TRACEEXEC), and auto-trace + event-stop for new children however they are cloned. */
#define PAL_TRACE_OPTS (PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL | PTRACE_O_TRACEEXEC | \
                        PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE)

#include "pal_linux_common.c"   /* the shared Linux host-driver + ptrace injector core */

/* Is `pid` stopped at a syscall ENTRY (vs an EXIT, or no syscall)? Linux >= 5.3 answers directly
 * via PTRACE_GET_SYSCALL_INFO, so the driver never assumes alternation. */
#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420e
#endif
#ifndef PTRACE_SYSCALL_INFO_ENTRY
#define PTRACE_SYSCALL_INFO_ENTRY 1
#endif
static int at_syscall_entry(pal_pid_t pid) {
    unsigned char info[128];                          /* op is the first byte of the struct */
    info[0] = 0;
    ptrace(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof info, info);
    return info[0] == PTRACE_SYSCALL_INFO_ENTRY;
}

pal_pid_t pal_guest_spawn(const char *path, char *const argv[]) {
    pal_fs_init_once();                          /* M4.2: establish the AIOS root (AIOS_ROOT) once, up front */
    pal_net_init_once();                         /* inc 5: establish the network allow-list (AIOS_NET_ALLOW) */
    umask(0);                                    /* the kernel owns a per-process umask now; do not let the
                                                  * host re-mask the modes the kernel already masked */
    /* The kernel does pipe writes on the guests' behalf; a write to a pipe with no readers must
     * surface as PAL_EPIPE, not a SIGPIPE that kills the kernel. */
    signal(SIGPIPE, SIG_IGN);
    /* Catch the terminal signals (^C -> SIGINT, ^Z -> SIGTSTP) so the kernel SURVIVES them (a SIGTSTP
     * handler stops the kernel from being suspended), its blocking reads interrupt (no SA_RESTART),
     * and it can FORWARD them to the foreground process group. The guests are off the kernel's host
     * pgrp (below), so the host pty signals only the kernel. */
    struct sigaction sa; sa.sa_handler = pal_term_handler; sa.sa_flags = 0; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    pal_net_init_signals();                      /* SIGCHLD plumbing for the socket-readiness co-wait */
    pid_t pid = fork();
    if (pid < 0) return PAL_PID_NONE;
    if (pid == 0) {
        /* Child becomes the guest: ask to be traced, leave the kernel's host process group (so the
         * host pty no longer delivers terminal signals to guests -- the kernel routes them), then
         * exec the AIOS-ABI program. The host sets up the initial stack (argc/argv/envp/auxv); the
         * guest's _start reads argv from it. (A future seL4 PAL builds this stack itself.) */
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        setpgid(0, 0);                           /* own host process group -- off the kernel's, off the pty fg */
        execv(path, argv);
        _exit(127);                              /* exec failed */
    }
    int st;
    if (waitpid_r(pid, &st, 0) < 0) return PAL_PID_NONE;   /* initial post-execv SIGTRAP stop */
    if (!WIFSTOPPED(st)) return PAL_PID_NONE;
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PAL_TRACE_OPTS);
    return (pal_pid_t)pid;                        /* stopped at entry; the kernel resumes it */
}

/* Wait for the next event from ANY live guest. Returns 1 (syscall on *who), 0 (*who exited), 2 (async
 * signal `*exit_code` on *who), 3 (a TERMINAL signal `*exit_code` was caught -- the kernel forwards it
 * to the foreground group; *who unused), -1 (no live guests). Skips syscall exits + injected artifacts,
 * and re-injects real signals so a crashing guest dies (rather than spinning on a re-faulting instr). */
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
            if (pid < 0) { if (errno == EINTR) continue; return -1; }   /* EINTR (^C): loop -> the g_term_sig check returns 3 */
        }
        if (WIFEXITED(st))   { *who = pid; if (exit_code) *exit_code = WEXITSTATUS(st);    return 0; }
        if (WIFSIGNALED(st)) { *who = pid; if (exit_code) *exit_code = 128 + WTERMSIG(st); return 0; }
        if (!WIFSTOPPED(st)) continue;
        int sig = WSTOPSIG(st);
        if (sig == (SIGTRAP | 0x80)) {                     /* a syscall stop */
            if (at_syscall_entry(pid)) {
                struct user_pt_regs r;
                if (getregs(pid, &r) != 0) return -1;
                sc->nr = pal_trapped_nr(&r);               /* GATEWAY decode: x8==gateway ? x9 : x8 (escape) */
                for (int i = 0; i < 6; i++) sc->arg[i] = (uint64_t)r.regs[i];
                *who = pid;
                return 1;                                  /* syscall ENTRY */
            }
            ptrace(PTRACE_SYSCALL, pid, 0, 0);             /* exit / artifact -> step past it */
            continue;
        }
        /* Non-syscall stop: a ptrace event (SIGTRAP + event byte; consumed by the inject primitives)
         * or a real async signal. Swallow trace traps + group stops; REPORT a real signal to the
         * kernel, which owns the policy (run the guest's handler, ignore, or terminate). */
        if (sig == SIGTRAP || sig == SIGSTOP) { ptrace(PTRACE_SYSCALL, pid, 0, 0); continue; }
        *who = pid;
        if (exit_code) *exit_code = sig;
        return 2;                                          /* async signal `sig` on *who */
    }
}
