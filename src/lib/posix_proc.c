/*
 * posix_proc.c -- AIOS POSIX process/identity/signal syscall handlers
 * exit, exit_group, getpid, getppid, getuid, geteuid, getgid, getegid,
 * clone, execve, wait4, rt_sigaction, rt_sigprocmask, kill, tgkill,
 * rt_sigpending
 */
#include "posix_internal.h"

long aios_sys_exit(va_list ap) {
    int status = va_arg(ap, int);
    (void)status;
    /* Trigger VM fault — exec_thread Recv's on fault ep */
    volatile int *null = (volatile int *)0;
    *null = 0;
    __builtin_unreachable();
    return 0;
}

long aios_sys_exit_group(va_list ap) {
    int status = va_arg(ap, int);
    /* Send exit code to pipe_server before dying */
    if (pipe_ep) {
        seL4_SetMR(0, (seL4_Word)status);
        seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_EXIT, 0, 0, 1));
    }
    /* Fault to trigger reaper (NULL deref) */
    volatile int *null_ptr = (volatile int *)0;
    *null_ptr = 0;
    __builtin_unreachable();
    return 0;
}


long aios_sys_getpid(va_list ap) {
    (void)ap;
    if (aios_pid <= 1 && pipe_ep) {
        seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
            seL4_MessageInfo_new(PIPE_GETPID, 0, 0, 0));
        long pid = (long)seL4_GetMR(0);
        if (pid > 0) aios_pid = (int)pid;
    }
    return (long)aios_pid;
}

long aios_sys_getppid(va_list ap) {
    (void)ap;
    return 0;
}

long aios_sys_getuid(va_list ap) { (void)ap; return (long)aios_uid; }
long aios_sys_geteuid(va_list ap) { (void)ap; return (long)aios_uid; }
long aios_sys_getgid(va_list ap) { (void)ap; return (long)aios_gid; }
long aios_sys_getegid(va_list ap) { (void)ap; return (long)aios_gid; }

long aios_sys_wait4(va_list ap) {
    int pid = va_arg(ap, int);
    int *wstatus = va_arg(ap, int *);
    /* int options = va_arg(ap, int); — ignored for now */
    /* struct rusage *rusage = va_arg(ap, struct rusage *); — ignored */

    if (!pipe_ep) return -38;  /* ENOSYS */

    /* Send PIPE_WAIT to pipe_server */
    seL4_SetMR(0, (seL4_Word)pid);
    seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
        seL4_MessageInfo_new(PIPE_WAIT, 0, 0, 1));

    long child_pid = (long)seL4_GetMR(0);
    int exit_status = (int)seL4_GetMR(1);

    if (wstatus && child_pid > 0) {
        /* Encode exit status in wait format: (status & 0xff) << 8 */
        *wstatus = (exit_status & 0xff) << 8;
    }

    return child_pid;
}

long aios_sys_execve(va_list ap) {
    const char *pathname = va_arg(ap, const char *);
    char *const *argv_arr = va_arg(ap, char *const *);
    (void)va_arg(ap, char *const *); /* envp -- unused */
    if (!pipe_ep || !pathname) return -38;

    /* Pack into buffer: path\0argv[0]\0argv[1]\0...\0\0
     * Double-null terminates the argument list. */
    char buf[900];
    int pos = 0;

    /* Resolve path */
    if (pathname[0] == '/') {
        while (*pathname && pos < 898) buf[pos++] = *pathname++;
    } else {
        const char *pfx = "/bin/";
        while (*pfx && pos < 898) buf[pos++] = *pfx++;
        while (*pathname && pos < 898) buf[pos++] = *pathname++;
    }
    buf[pos++] = 0; /* null after path */

    /* Pack argv strings */
    if (argv_arr) {
        for (int ai = 0; argv_arr[ai] && pos < 898; ai++) {
            const char *a = argv_arr[ai];
            while (*a && pos < 898) buf[pos++] = *a++;
            buf[pos++] = 0;
        }
    }
    buf[pos++] = 0; /* double-null terminator */

    /* Pack CWD after argv double-null so pipe_server can extract it */
    {
        const char *cwdp = aios_cwd;
        while (*cwdp && pos < 898) buf[pos++] = *cwdp++;
        buf[pos++] = 0;
    }

    /* Pack pipe metadata in MR0, string buffer in MR1..MRn
     * MR0 bits [31:16] = stdout_pipe_id + 1 (0 = no redirect)
     * MR0 bits [15:0]  = stdin_pipe_id + 1  (0 = no redirect) */
    seL4_Word pipe_meta = 0;
    if (stdout_pipe_id >= 0)
        pipe_meta |= ((seL4_Word)(stdout_pipe_id + 1)) << 16;
    if (stdin_pipe_id >= 0)
        pipe_meta |= ((seL4_Word)(stdin_pipe_id + 1)) & 0xFFFF;
    seL4_SetMR(0, pipe_meta);

    int nmrs = (pos + 7) / 8;
    if (nmrs > 109) nmrs = 109; /* leave headroom: MR0=meta + 109 data MRs */
    for (int m = 0; m < nmrs; m++) {
        seL4_Word w = 0;
        for (int b = 0; b < 8; b++) {
            int idx = m * 8 + b;
            if (idx < pos) w |= ((seL4_Word)(uint8_t)buf[idx]) << (b * 8);
        }
        seL4_SetMR(m + 1, w); /* offset by 1: MR0 is metadata */
    }
    seL4_MessageInfo_t reply = seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_EXEC, 0, 0, nmrs + 1));
    return (long)seL4_GetMR(0);
}

long aios_sys_clone(va_list ap) {
    /* On AArch64, clone(flags, stack, ...) -- basic fork has flags=SIGCHLD, stack=0 */
    (void)ap;
    if (!pipe_ep) return -38; /* -ENOSYS */

    /* Send PIPE_FORK to pipe_server (badged, so server knows who we are) */
    seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
        seL4_MessageInfo_new(PIPE_FORK, 0, 0, 0));
    long result = (long)seL4_GetMR(0);
    if (result == 0) {
        /* Child -- reset cached PID so getpid() re-queries */
        aios_pid = 0;
        /* Note: pipe refs are NOT incremented here. PIPE_EXEC already
         * increments refs for the specific pipes the child uses after
         * exec. Auto-incrementing all inherited FDs would double-count
         * because children do not close unused pipe ends before exec. */
    }
    return result;
}

long aios_sys_rt_sigaction(va_list ap) {
    int sig = va_arg(ap, int);
    const aios_k_sigaction_t *act = va_arg(ap, const aios_k_sigaction_t *);
    aios_k_sigaction_t *oldact = va_arg(ap, aios_k_sigaction_t *);
    (void)va_arg(ap, long); /* sigsetsize */

    if (sig < 1 || sig >= AIOS_NSIG) return -22; /* EINVAL */
    if (SIG_UNCATCHABLE(sig)) return -22;

    if (!sigstate.initialized) aios_sigstate_init(&sigstate);

    if (oldact) *oldact = sigstate.actions[sig];
    if (act) sigstate.actions[sig] = *act;
    return 0;
}

long aios_sys_rt_sigprocmask(va_list ap) {
    int how = va_arg(ap, int);
    const uint64_t *set = va_arg(ap, const uint64_t *);
    uint64_t *oldset = va_arg(ap, uint64_t *);
    (void)va_arg(ap, long); /* sigsetsize */

    if (!sigstate.initialized) aios_sigstate_init(&sigstate);

    if (oldset) *oldset = sigstate.blocked;
    if (set) {
        switch (how) {
        case 0: /* SIG_BLOCK */
            sigstate.blocked |= *set;
            break;
        case 1: /* SIG_UNBLOCK */
            sigstate.blocked &= ~(*set);
            break;
        case 2: /* SIG_SETMASK */
            sigstate.blocked = *set;
            break;
        default:
            return -22; /* EINVAL */
        }
        /* SIGKILL(9) and SIGSTOP(19) cannot be blocked */
        sigstate.blocked &= ~(SIG_BIT(9) | SIG_BIT(19));
    }
    return 0;
}

long aios_sys_kill(va_list ap) {
    int pid = va_arg(ap, int);
    int sig = va_arg(ap, int);

    if (sig < 0 || sig >= AIOS_NSIG) return -22; /* EINVAL */
    if (!pipe_ep) return -38; /* ENOSYS */

    seL4_SetMR(0, (seL4_Word)pid);
    seL4_SetMR(1, (seL4_Word)sig);
    seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
        seL4_MessageInfo_new(PIPE_SIGNAL, 0, 0, 2));
    (void)reply;
    long kill_result = (long)seL4_GetMR(0);
    aios_sig_check();
    return kill_result;
}

long aios_sys_tgkill(va_list ap) {
    (void)va_arg(ap, int); /* tgid -- ignored for now */
    int tid = va_arg(ap, int);
    int sig = va_arg(ap, int);

    if (sig < 0 || sig >= AIOS_NSIG) return -22;
    if (!pipe_ep) return -38;

    seL4_SetMR(0, (seL4_Word)tid);
    seL4_SetMR(1, (seL4_Word)sig);
    seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
        seL4_MessageInfo_new(PIPE_SIGNAL, 0, 0, 2));
    (void)reply;
    long kill_result = (long)seL4_GetMR(0);
    aios_sig_check();
    return kill_result;
}

long aios_sys_rt_sigpending(va_list ap) {
    uint64_t *set = va_arg(ap, uint64_t *);
    (void)va_arg(ap, long); /* sigsetsize */

    if (!sigstate.initialized) aios_sigstate_init(&sigstate);
    if (set) *set = (uint64_t)sigstate.pending;
    return 0;
}

/* v0.4.62: setuid -- set real user ID */
long aios_sys_setuid(va_list ap) {
    unsigned int uid = va_arg(ap, unsigned int);
    /* root can set any uid; non-root can only set own uid */
    if (aios_uid == 0 || uid == aios_uid) {
        aios_uid = uid;
        return 0;
    }
    return -EPERM;
}

/* v0.4.62: setgid -- set real group ID */
long aios_sys_setgid(va_list ap) {
    unsigned int gid = va_arg(ap, unsigned int);
    if (aios_uid == 0 || gid == aios_gid) {
        aios_gid = gid;
        return 0;
    }
    return -EPERM;
}

/* v0.4.62: setsid -- create new session, return session ID */
long aios_sys_setsid(va_list ap) {
    (void)ap;
    return (long)(aios_pid > 0 ? aios_pid : 1);
}

/* v0.4.62: getpgid -- get process group ID (flat model: pgid == pid) */
long aios_sys_getpgid(va_list ap) {
    int pid = va_arg(ap, int);
    (void)pid;
    return (long)(aios_pid > 0 ? aios_pid : 1);
}

/* v0.4.62: rt_sigreturn -- return from signal handler
 * AIOS dispatches signals synchronously via aios_sig_check(),
 * so the handler runs inline and returns normally.
 * rt_sigreturn is a no-op here; musl may call it. */
long aios_sys_rt_sigreturn(va_list ap) {
    (void)ap;
    return 0;
}

/* v0.4.62: sigaltstack -- alternate signal stack
 * Signals in AIOS are polled, no kernel stack switch needed.
 * We store the parameters for POSIX compliance. */
static void    *_sigalt_sp    = (void *)0;
static size_t   _sigalt_size  = 0;
static int      _sigalt_flags = 2; /* SS_DISABLE */

long aios_sys_sigaltstack(va_list ap) {
    /* ABI: stack_t { void *ss_sp; int ss_flags; size_t ss_size; } */
    typedef struct { void *sp; int flags; size_t size; } aios_stack_t;
    const aios_stack_t *ss  = va_arg(ap, const aios_stack_t *);
    aios_stack_t       *oss = va_arg(ap, aios_stack_t *);

    if (oss) {
        oss->sp    = _sigalt_sp;
        oss->flags = _sigalt_flags;
        oss->size  = _sigalt_size;
    }
    if (ss) {
        _sigalt_sp    = ss->sp;
        _sigalt_flags = ss->flags;
        _sigalt_size  = ss->size;
    }
    return 0;
}

/* v0.4.64: setpgid -- set process group ID
 * Flat process model: always succeeds, pgid == pid. */
long aios_sys_setpgid(va_list ap) {
    int pid = va_arg(ap, int);
    int pgid = va_arg(ap, int);
    (void)pid; (void)pgid;
    return 0;
}
