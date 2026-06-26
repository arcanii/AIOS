/*
 * pal_linux.c -- the PAL Linux backend. THE ONLY file that knows about Linux.
 *
 * Implements the gVisor-style trap model via ptrace(PTRACE_SYSCALL) for MANY guest processes at
 * once. Each guest is a traced Linux child; the kernel keys everything by pal_pid_t (= the tracee
 * pid). The loop is "wait for the next event from any guest (pal_guest_next, a waitpid(-1)), then
 * service + resume THAT guest". Resume is owned by pal_guest_return / pal_guest_resume; waiting by
 * pal_guest_next -- decoupled, the standard multi-tracee shape.
 *
 * AIOS-numbered syscalls (>= 0x1000) are not real Linux syscalls, so if one runs it just ENOSYSes
 * harmlessly and we plant the real AIOS result in x0 at the exit. PTRACE_SYSCALL (not SYSEMU) is
 * what lets the kernel INJECT a real host syscall (mmap/execve/clone/exit_group) by rewriting the
 * trapped svc in place. The driver never ASSUMES strict entry/exit alternation: it classifies each
 * stop with PTRACE_GET_SYSCALL_INFO and resumes past anything that is not a genuine syscall entry,
 * so the stray exit/event stops that injection leaves behind never desynchronise dispatch.
 *
 * aarch64 ABI of the traced guest: syscall number in x8 (overridden via NT_ARM_SYSTEM_CALL), args
 * x0..x5, return value in x0. A future pal_sel4.c implements this same pal.h contract with no
 * ptrace -- the AIOS kernel above does not change.
 */
#define _GNU_SOURCE
#include "pal.h"
#include "aios_abi.h"           /* AIOS_O_* flag values to translate */

#include <errno.h>
#include <fcntl.h>              /* O_* + open() */
#include <signal.h>            /* SIGCHLD, SIGTRAP, SIGSTOP */
#include <stdint.h>
#include <stdio.h>             /* rename() */
#include <stdlib.h>           /* getenv (AIOS_ROOT -- host-specific config; the PAL is host-aware) */
#include <string.h>
#include <time.h>             /* clock_gettime + CLOCK_* (the host clock source) */
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>        /* __NR_* for the host syscalls we inject */
#include <sys/uio.h>
#include <sys/wait.h>
#include <linux/elf.h>          /* NT_PRSTATUS */
#include <asm/ptrace.h>         /* struct user_pt_regs */

/* Options set on every tracee (init + each forked child): syscall-stops as SIGTRAP|0x80
 * (TRACESYSGOOD), the guest dies if the kernel dies (EXITKILL), clean exec-event stops
 * (TRACEEXEC), and auto-trace + event-stop for new children however they are cloned. */
#define PAL_TRACE_OPTS (PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL | PTRACE_O_TRACEEXEC | \
                        PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE)

/* aarch64: change which syscall the kernel dispatches for the stopped tracee (or -1 to skip). */
#ifndef NT_ARM_SYSTEM_CALL
#define NT_ARM_SYSTEM_CALL 0x404
#endif

static int getregs(pal_pid_t pid, struct user_pt_regs *r) {
    struct iovec io = { r, sizeof *r };
    return ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &io) == 0 ? 0 : -1;
}
static int setregs(pal_pid_t pid, const struct user_pt_regs *r) {
    struct iovec io = { (void *)r, sizeof *r };
    return ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &io) == 0 ? 0 : -1;
}
static int set_syscall_nr(pal_pid_t pid, int nr) {
    struct iovec io = { &nr, sizeof nr };
    return ptrace(PTRACE_SETREGSET, pid, (void *)NT_ARM_SYSTEM_CALL, &io) == 0 ? 0 : -1;
}

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

/* waitpid that retries on EINTR. The kernel catches SIGINT (so ^C interrupts the blocking host read
 * it does on a guest's behalf, see pal_guest_spawn), which would otherwise EINTR these waits-for-a-
 * tracee-event too. The tracee event WILL come, so just retry. */
static pid_t waitpid_r(pid_t pid, int *st, int flags) {
    for (;;) {
        pid_t r = waitpid(pid, st, flags);
        if (r >= 0 || errno != EINTR) return r;
    }
}
/* A no-op SIGINT handler: its only job is to EXIST (so the kernel does not die on ^C) and to lack
 * SA_RESTART (so a blocking host read the kernel is mid-way through on a guest's behalf returns
 * EINTR, freeing the guest to receive its own ^C). The guests get SIGINT via the process group;
 * pal_guest_next reports it to the kernel. */
static void pal_noop_sig(int s) { (void)s; }

static void pal_fs_init_once(void);              /* M4.2: defined with the confinement layer below */

pal_pid_t pal_guest_spawn(const char *path, char *const argv[]) {
    pal_fs_init_once();                          /* M4.2: establish the AIOS root (AIOS_ROOT) once, up front */
    /* The kernel does pipe writes on the guests' behalf; a write to a pipe with no readers must
     * surface as PAL_EPIPE, not a SIGPIPE that kills the kernel. */
    signal(SIGPIPE, SIG_IGN);
    /* Catch SIGINT (^C) so the kernel survives it AND its blocking reads interrupt (no SA_RESTART);
     * the guests receive their own ^C via the process group. */
    struct sigaction sa; sa.sa_handler = pal_noop_sig; sa.sa_flags = 0; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    pid_t pid = fork();
    if (pid < 0) return PAL_PID_NONE;
    if (pid == 0) {
        /* Child becomes the guest: ask to be traced, then exec the AIOS-ABI program. The host
         * sets up the initial stack (argc/argv/envp/auxv); the guest's _start reads argv from it.
         * (A future seL4 PAL builds this stack itself.) */
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        execv(path, argv);
        _exit(127);                              /* exec failed */
    }
    int st;
    if (waitpid_r(pid, &st, 0) < 0) return PAL_PID_NONE;   /* initial post-execv SIGTRAP stop */
    if (!WIFSTOPPED(st)) return PAL_PID_NONE;
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PAL_TRACE_OPTS);
    return (pal_pid_t)pid;                        /* stopped at entry; the kernel resumes it */
}

/* Wait for the next event from ANY live guest. Returns 1 (syscall on *who), 0 (*who exited), -1
 * (no live guests). Skips syscall exits + injected artifacts, and re-injects real signals so a
 * crashing guest dies (rather than spinning on a re-faulting instruction). */
int pal_guest_next(pal_pid_t *who, pal_syscall_t *sc, int *exit_code) {
    for (;;) {
        int st;
        pid_t pid = waitpid(-1, &st, 0);
        if (pid < 0) { if (errno == EINTR) continue; return -1; }   /* EINTR (^C): retry; ECHILD: done */
        if (WIFEXITED(st))   { *who = pid; if (exit_code) *exit_code = WEXITSTATUS(st);    return 0; }
        if (WIFSIGNALED(st)) { *who = pid; if (exit_code) *exit_code = 128 + WTERMSIG(st); return 0; }
        if (!WIFSTOPPED(st)) continue;
        int sig = WSTOPSIG(st);
        if (sig == (SIGTRAP | 0x80)) {                     /* a syscall stop */
            if (at_syscall_entry(pid)) {
                struct user_pt_regs r;
                if (getregs(pid, &r) != 0) return -1;
                sc->nr = (uint64_t)r.regs[8];
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

int pal_guest_setret(pal_pid_t who, uint64_t retval) {
    /* `who` is stopped at a syscall ENTRY. ENFORCE THE BOUNDARY (M4): neutralize the trapped syscall
     * number (-1) so the host SKIPS it -- the guest's chosen syscall, AIOS-numbered or a smuggled
     * real-Linux one, NEVER executes on the host. Only the kernel's deliberate injections (mmap/exec/
     * fork/exit, their own paths) ever run a real host syscall. Then run the now-skipped syscall to
     * its EXIT and plant the AIOS result in x0 -- leaving `who` stopped at the exit (NOT resumed). */
    set_syscall_nr(who, -1);
    if (ptrace(PTRACE_SYSCALL, who, 0, 0) != 0) return -1;
    int st;
    if (waitpid_r(who, &st, 0) < 0) return -1;
    if (!WIFSTOPPED(st)) return 0;                 /* exited mid-syscall */
    struct user_pt_regs r;
    if (getregs(who, &r) != 0) return -1;
    r.regs[0] = (unsigned long long)retval;
    return setregs(who, &r) == 0 ? 0 : -1;
}
int pal_guest_return(pal_pid_t who, uint64_t retval) {
    if (pal_guest_setret(who, retval) != 0) return -1;
    return ptrace(PTRACE_SYSCALL, who, 0, 0) == 0 ? 0 : -1;   /* resume toward the next syscall */
}

int pal_guest_resume(pal_pid_t who) {
    return ptrace(PTRACE_SYSCALL, who, 0, 0) == 0 ? 0 : -1;
}

size_t pal_guest_read(pal_pid_t who, uint64_t gaddr, void *dst, size_t len) {
    struct iovec local  = { dst, len };
    struct iovec remote = { (void *)(uintptr_t)gaddr, len };
    ssize_t n = process_vm_readv(who, &local, 1, &remote, 1, 0);
    return n < 0 ? 0 : (size_t)n;
}

size_t pal_guest_write(pal_pid_t who, uint64_t gaddr, const void *src, size_t len) {
    struct iovec local  = { (void *)src, len };
    struct iovec remote = { (void *)(uintptr_t)gaddr, len };
    ssize_t n = process_vm_writev(who, &local, 1, &remote, 1, 0);
    return n < 0 ? 0 : (size_t)n;
}

/* Grow `who`'s address space: rewrite the trapped AIOS_SYS_MMAP svc IN PLACE into a real Linux
 * mmap and run it. Save the guest's regs (mmap clobbers x0..x5 but the guest's wrapper expects all
 * but x0 preserved across the svc) and restore them with x0 = the mapped address. Leaves `who`
 * stopped at the mmap exit; the kernel pal_guest_resume()s it. */
uint64_t pal_guest_mmap(pal_pid_t who, size_t len) {
    struct user_pt_regs saved, r;
    if (getregs(who, &saved) != 0) return 0;
    r = saved;
    r.regs[0] = 0;                       /* addr   = NULL               */
    r.regs[1] = (unsigned long long)len; /* length                      */
    r.regs[2] = 0x1 | 0x2;               /* PROT_READ | PROT_WRITE      */
    r.regs[3] = 0x02 | 0x20;             /* MAP_PRIVATE | MAP_ANONYMOUS */
    r.regs[4] = (unsigned long long)-1;  /* fd     = -1                 */
    r.regs[5] = 0;                       /* offset = 0                  */
    if (setregs(who, &r) != 0) return 0;
    if (set_syscall_nr(who, __NR_mmap) != 0) return 0;

    if (ptrace(PTRACE_SYSCALL, who, 0, 0) != 0) return 0;   /* run mmap -> EXIT */
    int st;
    if (waitpid_r(who, &st, 0) < 0 || !WIFSTOPPED(st)) return 0;
    if (getregs(who, &r) != 0) return 0;
    uint64_t addr = (uint64_t)r.regs[0];
    if ((int64_t)addr < 0 && (int64_t)addr >= -4095) addr = 0;  /* -errno -> failure */

    saved.regs[0] = (unsigned long long)addr;
    if (setregs(who, &saved) != 0) return 0;
    return addr;
}

/* M4.3: confine a guest-issued exec target to the AIOS root (defined with the confinement layer
 * below). Returns 1 (unconfined: use `gpath` unchanged), 0 (confined: *out_addr holds a guest address
 * with the canonical in-root host path to exec instead), or -errno (target not reachable in the root). */
static long pal_confine_exec(pal_pid_t who, uint64_t gpath, const struct user_pt_regs *regs, uint64_t *out_addr);

/* Replace `who`'s image by rewriting the trapped AIOS_SYS_EXEC svc into a Linux execve. M4.3: when the
 * PAL is confined, the path is first resolved INSIDE the AIOS root (a guest can only exec binaries in
 * its root -- the INIT program the operator names on the command line is the trusted entry and is NOT
 * routed here). execve has no normal return on success: PTRACE_O_TRACEEXEC turns it into a
 * PTRACE_EVENT_EXEC stop, after which the new image is live (the kernel resumes it; pal_guest_next then
 * skips the trailing execve exit and lands on the new program's first syscall). On failure execve
 * returns -errno at a normal exit; restore the guest with x0 = -1 (or the confinement -errno). */
int pal_guest_exec(pal_pid_t who, uint64_t gpath, uint64_t gargv, uint64_t genvp) {
    struct user_pt_regs saved, r;
    if (getregs(who, &saved) != 0) return -1;
    uint64_t path_addr = gpath;
    long c = pal_confine_exec(who, gpath, &saved, &path_addr);   /* M4.3: confine the exec target */
    if (c < 0) {                                                 /* target not reachable in the root */
        pal_guest_setret(who, (uint64_t)c);                      /* neutralize the exec svc + plant -errno */
        return -1;                                               /* (so the guest sees ENOENT, not ENOSYS) */
    }
    r = saved;
    r.regs[0] = path_addr;                                       /* confined canon path, or gpath as-is */
    r.regs[1] = gargv;
    r.regs[2] = genvp;
    if (setregs(who, &r) != 0) return -1;
    if (set_syscall_nr(who, __NR_execve) != 0) return -1;

    if (ptrace(PTRACE_SYSCALL, who, 0, 0) != 0) return -1;   /* run execve */
    int st;
    if (waitpid_r(who, &st, 0) < 0) return -1;

    if (WIFSTOPPED(st) && (st >> 8) == (SIGTRAP | (PTRACE_EVENT_EXEC << 8)))
        return 0;                                            /* success: new image live */
    if (WIFSTOPPED(st)) {                                     /* failure: restore + plant -1 */
        saved.regs[0] = (unsigned long long)-1;
        setregs(who, &saved);
        return -1;
    }
    return -1;                                               /* died during execve */
}

/* Fork `parent` (stopped at its AIOS_SYS_FORK svc) by rewriting it into a Linux clone(SIGCHLD):
 * a fork-equivalent (all clone args but the exit-signal are 0, so the aarch64 arg order is moot).
 * PTRACE_O_TRACEFORK makes the child auto-traced + reports a fork/vfork/clone event stop on the
 * parent; the child's pid comes from PTRACE_GETEVENTMSG. We restore BOTH register files to the
 * guest's saved state (clone clobbered x0..x5) with x0 = child pid in the parent, 0 in the child --
 * exactly POSIX fork's two return values. The kernel registers the child and resumes both. Returns
 * the child handle, or PAL_PID_NONE on failure (parent restored with x0 = -1). */
pal_pid_t pal_guest_fork(pal_pid_t parent) {
    struct user_pt_regs saved, r;
    if (getregs(parent, &saved) != 0) return PAL_PID_NONE;
    r = saved;
    r.regs[0] = (unsigned long long)SIGCHLD;   /* clone flags = exit-signal only => fork-like */
    r.regs[1] = 0; r.regs[2] = 0; r.regs[3] = 0; r.regs[4] = 0; r.regs[5] = 0;
    if (setregs(parent, &r) != 0) return PAL_PID_NONE;
    if (set_syscall_nr(parent, __NR_clone) != 0) return PAL_PID_NONE;

    if (ptrace(PTRACE_SYSCALL, parent, 0, 0) != 0) return PAL_PID_NONE;   /* run clone */
    int st;
    if (waitpid_r(parent, &st, 0) < 0) return PAL_PID_NONE;

    int ev = st >> 8;
    int is_new_child = WIFSTOPPED(st) &&
        (ev == (SIGTRAP | (PTRACE_EVENT_FORK  << 8)) ||
         ev == (SIGTRAP | (PTRACE_EVENT_VFORK << 8)) ||
         ev == (SIGTRAP | (PTRACE_EVENT_CLONE << 8)));
    if (!is_new_child) {                                       /* clone failed (-errno at exit) */
        if (WIFSTOPPED(st)) { saved.regs[0] = (unsigned long long)-1; setregs(parent, &saved); }
        return PAL_PID_NONE;
    }

    unsigned long child = 0;
    if (ptrace(PTRACE_GETEVENTMSG, parent, 0, &child) != 0) return PAL_PID_NONE;
    pal_pid_t cpid = (pal_pid_t)child;

    int cst;
    if (waitpid_r(cpid, &cst, 0) < 0) return PAL_PID_NONE;       /* the child's initial stop */
    ptrace(PTRACE_SETOPTIONS, cpid, 0, (void *)PAL_TRACE_OPTS);

    struct user_pt_regs cr = saved; cr.regs[0] = 0;            /* child: fork returns 0 */
    setregs(cpid, &cr);
    struct user_pt_regs pr = saved; pr.regs[0] = (unsigned long long)cpid;  /* parent: child pid */
    setregs(parent, &pr);
    return cpid;
}

/* Terminate `who` with `code` by rewriting its trapped AIOS_SYS_EXIT svc into exit_group(code).
 * The process really exits, freeing its address space; its exit surfaces through pal_guest_next as
 * event 0 (code & 0xff), which drives the kernel's reap / wake-the-parent bookkeeping. */
int pal_guest_exit(pal_pid_t who, int code) {
    struct user_pt_regs r;
    if (getregs(who, &r) != 0) return -1;
    r.regs[0] = (unsigned long long)(unsigned int)code;
    if (setregs(who, &r) != 0) return -1;
    if (set_syscall_nr(who, __NR_exit_group) != 0) return -1;
    return ptrace(PTRACE_SYSCALL, who, 0, 0) == 0 ? 0 : -1;   /* run exit_group; never returns */
}

/* --- host-driver gateway (Linux: a backing object is a host fd) --- */

pal_file_t pal_host_std(int which) { return (pal_file_t)which; }   /* host fds 0/1/2 */

/* AIOS_O_* (host-agnostic) -> Linux O_*. Flag-value translation is host-specific, so it lives
 * here in the PAL, not in the kernel. */
static int xlate_open_flags(uint64_t f) {
    int acc = (int)(f & AIOS_O_ACCMODE);
    int o = (acc == AIOS_O_WRONLY) ? O_WRONLY : (acc == AIOS_O_RDWR) ? O_RDWR : O_RDONLY;
    if (f & AIOS_O_CREAT)     o |= O_CREAT;
    if (f & AIOS_O_TRUNC)     o |= O_TRUNC;
    if (f & AIOS_O_APPEND)    o |= O_APPEND;
    if (f & AIOS_O_CLOEXEC)   o |= O_CLOEXEC;
    if (f & AIOS_O_DIRECTORY) o |= O_DIRECTORY;
    if (f & AIOS_O_EXCL)      o |= O_EXCL;
    if (f & AIOS_O_NONBLOCK)  o |= O_NONBLOCK;
    return o;
}

/* Map a PAL directory handle to a host dirfd: PAL_AT_FDCWD -> the host's AT_FDCWD ("relative to
 * cwd"), otherwise the host fd itself. Keeps the host's AT_FDCWD value out of the kernel. */
static int hostdir(pal_file_t dir) { return dir == PAL_AT_FDCWD ? AT_FDCWD : (int)dir; }

/* host errno -> a negated AIOS error code. The AIOS error numbers (aios_abi.h) are chosen to match
 * Linux, so the Linux PAL's mapping is just `-errno`; this also yields PAL_EWOULDBLOCK (-EAGAIN) and
 * PAL_EPIPE (-EPIPE) for the two the pipe path tests by name. */
static long pal_errno(void) {
    int e = errno;
    return e ? -(long)e : -1;
}

/* ===================== M4.2: filesystem confinement (the other half of the boundary) =====================
 * M4 stopped a guest BYPASSING the kernel; this stops a guest -- even going THROUGH the kernel -- from
 * reaching host paths OUTSIDE an AIOS root. When the PAL is launched with AIOS_ROOT set, every guest file
 * path is resolved INSIDE that root with openat2(RESOLVE_IN_ROOT): absolute paths, ".." traversal, and
 * symlinks (absolute or "..") are all clamped to the root by the host kernel. That is the standard
 * UNPRIVILEGED container path-safety primitive -- no chroot / CAP_SYS_CHROOT, so it runs as plain user
 * `pi` on the Pi. The AIOS kernel above is UNCHANGED (same path strings, same ABI, zero new syscalls);
 * confinement is purely a PAL host-gateway policy, which is exactly where it belongs -- a future seL4 PAL
 * is simply handed an fs cap rooted at the AIOS fs, enforcing the same view with no kernel change.
 *
 * Default (AIOS_ROOT unset) = unconfined: the original open()/stat()/... paths, behaviour unchanged.
 * SCOPE: this confines the DATA boundary (open/stat/unlink/mkdir/rmdir/rename/chdir/getcwd/readlink +
 * the *at family). EXEC stays resolved in the host namespace (the kernel injects execve into the tracee),
 * so confining WHICH binary a guest may launch is a separate, still-open hardening step. cwd is a single
 * PAL-side logical path (as the host cwd was before); per-process cwd is future work. */

#ifndef __NR_openat2
#define __NR_openat2 437
#endif
#ifndef __NR_faccessat2
#define __NR_faccessat2 439
#endif
#ifndef RESOLVE_IN_ROOT
#define RESOLVE_IN_ROOT 0x10ULL   /* openat2: treat dirfd as "/" for this resolution (clamp escapes) */
#endif
#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
struct aios_open_how { uint64_t flags; uint64_t mode; uint64_t resolve; };   /* the openat2(2) arg */

static int  g_confined = 0;        /* set once from AIOS_ROOT at spawn */
static int  g_root_fd  = -1;       /* the AIOS root dir (O_PATH); confined resolution roots here */
static char g_cwd[1024] = "/";     /* the guest's logical cwd WITHIN the root (confined mode) */

/* Establish the confinement root ONCE, from AIOS_ROOT, before any guest runs. Fail CLOSED: if a root
 * was requested but cannot be opened, refuse to start rather than silently expose the whole host. */
static void pal_fs_init_once(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *root = getenv("AIOS_ROOT");
    if (!root || !*root) return;                                  /* unconfined (default) */
    int fd = open(root, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[pal] AIOS_ROOT=%s: cannot open as a directory (%s) -- refusing to start "
                        "(fail closed)\n", root, strerror(errno));
        _exit(71);
    }
    g_root_fd  = fd;
    g_confined = 1;
}

/* openat2 with the resolution clamped to the root anchored at `dirfd`. Returns an fd (>= 0) or -1 with
 * errno set. `mode` is only honoured with O_CREAT (openat2 rejects a nonzero mode otherwise). */
static long openat2_in_root(int dirfd, const char *path, uint64_t hflags, unsigned int mode) {
    struct aios_open_how how;
    how.flags   = hflags;
    /* openat2 is STRICT: how.mode must be 0 without O_CREAT/O_TMPFILE, and must carry ONLY permission
     * bits (a callers's st_mode with S_IFREG etc. -> EINVAL, where plain open() silently ignores them). */
    how.mode    = (hflags & (uint64_t)O_CREAT) ? (uint64_t)(mode & 07777) : 0;
    how.resolve = RESOLVE_IN_ROOT;
    return syscall(__NR_openat2, dirfd, path, &how, sizeof how);
}

/* Collapse an absolute path TEXTUALLY ("."/empty dropped, ".." popped + clamped at root). Logical only:
 * the real, secure resolution is always redone by openat2(RESOLVE_IN_ROOT); this just keeps getcwd tidy. */
static void path_norm(const char *in, char *out, size_t outsz) {
    size_t olen = 0;                                  /* build "/c1/c2/..."; empty result => root "/" */
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - seg);
        if (clen == 1 && seg[0] == '.') continue;
        if (clen == 2 && seg[0] == '.' && seg[1] == '.') {
            while (olen > 0 && out[olen - 1] != '/') olen--;   /* drop last segment's chars */
            if (olen > 0) olen--;                              /* and its leading '/'        */
            continue;
        }
        if (olen + 1 + clen >= outsz) break;                   /* overflow: best-effort truncate */
        out[olen++] = '/';
        for (size_t i = 0; i < clen; i++) out[olen++] = seg[i];
    }
    if (olen == 0) out[olen++] = '/';
    out[olen] = '\0';
}

/* The logical path to hand openat2 for an AT_FDCWD / plain-path op: absolute as-is, else joined onto the
 * logical cwd. openat2(RESOLVE_IN_ROOT) does the secure resolution of any ".."/symlinks inside it. */
static void eff_path(const char *path, char *out, size_t outsz) {
    if      (path[0] == '/')    snprintf(out, outsz, "%s", path);
    else if (g_cwd[1] == '\0')  snprintf(out, outsz, "/%s", path);          /* cwd == "/" */
    else                        snprintf(out, outsz, "%s/%s", g_cwd, path);
}

/* For a confined op pick (base dir fd, path-under-it): PAL_AT_FDCWD -> (root, cwd-joined path); a real
 * directory backing -> (that fd, the raw path resolved with the fd as its root). */
static int confined_base(pal_file_t dir, const char *path, char *effbuf, size_t effsz, const char **outpath) {
    if (dir == PAL_AT_FDCWD) { eff_path(path, effbuf, effsz); *outpath = effbuf; return g_root_fd; }
    *outpath = path;
    return (int)dir;
}

/* Open the PARENT directory of `fullpath` (resolved confined under `basefd`) and hand back the trailing
 * leaf, for ops that must act on a name WITHOUT walking past it (unlink/rmdir/mkdir/rename/readlink). The
 * parent walk is confined by openat2; the leaf op never follows further (unlinkat/readlinkat/mkdirat/
 * renameat do not traverse a final symlink). The returned fd is the caller's to close (a dup of `basefd`
 * when there is no slash, i.e. the leaf sits directly in basefd). */
static long open_parent_at(int basefd, const char *fullpath, char *dirbuf, size_t dirsz, const char **leaf) {
    const char *slash = strrchr(fullpath, '/');
    if (!slash) { *leaf = fullpath; return dup(basefd); }           /* leaf relative to basefd */
    if (slash[1] == '\0') { errno = EINVAL; return -1; }            /* trailing slash: no leaf */
    *leaf = slash + 1;
    size_t dl = (size_t)(slash - fullpath);
    const char *dpath;
    if (dl == 0) dpath = "/";                                       /* "/leaf" -> parent is the root */
    else {
        if (dl >= dirsz) { errno = ENAMETOOLONG; return -1; }
        memcpy(dirbuf, fullpath, dl); dirbuf[dl] = '\0'; dpath = dirbuf;
    }
    return openat2_in_root(basefd, dpath, (uint64_t)(O_PATH | O_DIRECTORY | O_CLOEXEC), 0);
}

/* M4.3 -- confine a guest-issued exec to the AIOS root. Resolve the guest's exec target INSIDE the
 * root (openat2 RESOLVE_IN_ROOT, following symlinks within the root), turn the resulting O_PATH handle
 * into a canonical real host path via /proc/self/fd, and stage that path in the guest's own stack
 * scratch (below sp, inside the current stack page) for execve to use. The canonical path is fully
 * resolved and provably under the root, so execve re-resolving it stays in-root. Returns 0 (use
 * *out_addr), 1 (unconfined -> use the guest path as-is), or -errno (not reachable in the root). */
static long pal_confine_exec(pal_pid_t who, uint64_t gpath, const struct user_pt_regs *regs, uint64_t *out_addr) {
    if (!g_confined) return 1;
    char gp[1024];
    size_t n = pal_guest_read(who, gpath, gp, sizeof gp - 1);
    if (n == 0) return -EFAULT;
    gp[n] = '\0';
    char eff[2048]; eff_path(gp, eff, sizeof eff);
    long fd = openat2_in_root(g_root_fd, eff, (uint64_t)(O_PATH | O_CLOEXEC), 0);   /* in-root? */
    if (fd < 0) return -errno;                                       /* ENOENT/.. : denied */
    char proc[64], canon[2048];
    snprintf(proc, sizeof proc, "/proc/self/fd/%ld", fd);
    ssize_t cl = readlink(proc, canon, sizeof canon - 1);            /* canonical in-root host path */
    close((int)fd);
    if (cl <= 0) return -EACCES;
    canon[cl] = '\0';
    /* stage canon in the guest's current stack page, strictly below sp (dead space the active frames
     * do not use; execve copies the path before unmapping, so on failure the bytes are harmless). */
    uint64_t sp = regs->sp;
    uint64_t pagestart = sp & ~(uint64_t)0xFFF;
    uint64_t scratch   = (sp - 512) & ~(uint64_t)0xF;
    if (scratch < pagestart) scratch = pagestart;
    if (sp - scratch < (uint64_t)cl + 1) return -ENAMETOOLONG;
    if (pal_guest_write(who, scratch, canon, (size_t)cl + 1) != (size_t)cl + 1) return -EFAULT;
    *out_addr = scratch;
    return 0;
}

pal_file_t pal_host_open(const char *path, uint64_t aios_flags, uint64_t mode) {
    int hf = xlate_open_flags(aios_flags);
    if (g_confined) {
        char eff[2048]; eff_path(path, eff, sizeof eff);
        long fd = openat2_in_root(g_root_fd, eff, (uint64_t)hf, (unsigned int)mode);
        return fd < 0 ? (pal_file_t)pal_errno() : (pal_file_t)fd;
    }
    int fd = open(path, hf, (mode_t)mode);
    return fd < 0 ? (pal_file_t)pal_errno() : (pal_file_t)fd;   /* -errno on failure */
}

long pal_host_read(pal_file_t f, void *buf, size_t len) {
    long n = (long)read((int)f, buf, len);
    return n < 0 ? pal_errno() : n;
}
long pal_host_write(pal_file_t f, const void *buf, size_t len) {
    long n = (long)write((int)f, buf, len);
    return n < 0 ? pal_errno() : n;
}
int  pal_host_close(pal_file_t f)                              { return close((int)f); }

int pal_host_pipe(pal_file_t *rd, pal_file_t *wr) {
    int fds[2];
    if (pipe2(fds, O_NONBLOCK) != 0) return (int)pal_errno();   /* non-blocking: kernel parks, never wedges */
    *rd = (pal_file_t)fds[0];
    *wr = (pal_file_t)fds[1];
    return 0;
}

long long pal_host_lseek(pal_file_t f, long long off, int whence) {
    off_t r = lseek((int)f, (off_t)off, whence);           /* AIOS_SEEK_* == SEEK_* */
    return r < 0 ? (long long)pal_errno() : (long long)r;
}
/* aios_stat's time fields are timespecs named st_atim/st_mtim/st_ctim -- the same names the host
 * struct stat uses for its timespec members -- so the per-second/nsec copy below is direct. */
static void fill_aios_stat(struct aios_stat *a, const struct stat *s) {
    a->st_dev   = (unsigned long long)s->st_dev;
    a->st_ino   = (unsigned long long)s->st_ino;
    a->st_mode  = (unsigned int)s->st_mode;                /* st_mode layout matches AIOS_S_IF* */
    a->st_nlink = (unsigned int)s->st_nlink;
    a->st_uid   = (unsigned int)s->st_uid;
    a->st_gid   = (unsigned int)s->st_gid;
    a->st_rdev  = (unsigned long long)s->st_rdev;
    a->st_size    = (long long)s->st_size;
    a->st_blksize = (long long)s->st_blksize;
    a->st_blocks  = (long long)s->st_blocks;
    a->st_atim.tv_sec  = (long long)s->st_atim.tv_sec;  a->st_atim.tv_nsec = (long long)s->st_atim.tv_nsec;
    a->st_mtim.tv_sec  = (long long)s->st_mtim.tv_sec;  a->st_mtim.tv_nsec = (long long)s->st_mtim.tv_nsec;
    a->st_ctim.tv_sec  = (long long)s->st_ctim.tv_sec;  a->st_ctim.tv_nsec = (long long)s->st_ctim.tv_nsec;
}
int pal_host_fstat(pal_file_t f, struct aios_stat *out) {
    struct stat st;
    if (fstat((int)f, &st) != 0) return (int)pal_errno();
    fill_aios_stat(out, &st);
    return 0;
}
int pal_host_stat(const char *path, struct aios_stat *out, int follow) {
    struct stat st;
    if (g_confined) {
        char eff[2048]; eff_path(path, eff, sizeof eff);
        long fd = openat2_in_root(g_root_fd, eff, (uint64_t)(O_PATH | O_CLOEXEC | (follow ? 0 : O_NOFOLLOW)), 0);
        if (fd < 0) return (int)pal_errno();                          /* an O_PATH|O_NOFOLLOW handle of */
        int r = fstat((int)fd, &st); int e = errno; close((int)fd);  /* a symlink fstats the link (lstat) */
        if (r != 0) { errno = e; return (int)pal_errno(); }
        fill_aios_stat(out, &st);
        return 0;
    }
    if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) return (int)pal_errno();
    fill_aios_stat(out, &st);
    return 0;
}
int pal_host_unlink(const char *path) {
    if (g_confined) {
        char eff[2048]; eff_path(path, eff, sizeof eff);
        char dirb[2048]; const char *leaf;
        long pfd = open_parent_at(g_root_fd, eff, dirb, sizeof dirb, &leaf);
        if (pfd < 0) return (int)pal_errno();
        int r = unlinkat((int)pfd, leaf, 0); int e = errno; close((int)pfd);
        return r == 0 ? 0 : (errno = e, (int)pal_errno());
    }
    return unlink(path) == 0 ? 0 : (int)pal_errno();
}
int pal_host_mkdir(const char *path, unsigned int m) {
    if (g_confined) {
        char eff[2048]; eff_path(path, eff, sizeof eff);
        char dirb[2048]; const char *leaf;
        long pfd = open_parent_at(g_root_fd, eff, dirb, sizeof dirb, &leaf);
        if (pfd < 0) return (int)pal_errno();
        int r = mkdirat((int)pfd, leaf, (mode_t)m); int e = errno; close((int)pfd);
        return r == 0 ? 0 : (errno = e, (int)pal_errno());
    }
    return mkdir(path, (mode_t)m) == 0 ? 0 : (int)pal_errno();
}
int pal_host_rmdir(const char *path) {
    if (g_confined) {
        char eff[2048]; eff_path(path, eff, sizeof eff);
        char dirb[2048]; const char *leaf;
        long pfd = open_parent_at(g_root_fd, eff, dirb, sizeof dirb, &leaf);
        if (pfd < 0) return (int)pal_errno();
        int r = unlinkat((int)pfd, leaf, AT_REMOVEDIR); int e = errno; close((int)pfd);
        return r == 0 ? 0 : (errno = e, (int)pal_errno());
    }
    return rmdir(path) == 0 ? 0 : (int)pal_errno();
}
int pal_host_rename(const char *o, const char *n) {
    if (g_confined) {
        char effo[2048], effn[2048]; eff_path(o, effo, sizeof effo); eff_path(n, effn, sizeof effn);
        char dbo[2048], dbn[2048]; const char *lo, *ln;
        long pfo = open_parent_at(g_root_fd, effo, dbo, sizeof dbo, &lo);
        if (pfo < 0) return (int)pal_errno();
        long pfn = open_parent_at(g_root_fd, effn, dbn, sizeof dbn, &ln);
        if (pfn < 0) { int e = errno; close((int)pfo); errno = e; return (int)pal_errno(); }
        int r = renameat((int)pfo, lo, (int)pfn, ln); int e = errno;
        close((int)pfo); close((int)pfn);
        return r == 0 ? 0 : (errno = e, (int)pal_errno());
    }
    return rename(o, n) == 0 ? 0 : (int)pal_errno();
}
int pal_host_chdir(const char *path) {
    if (g_confined) {
        char eff[2048]; eff_path(path, eff, sizeof eff);
        long fd = openat2_in_root(g_root_fd, eff, (uint64_t)(O_PATH | O_DIRECTORY | O_CLOEXEC), 0);
        if (fd < 0) return (int)pal_errno();
        close((int)fd);
        char norm[1024]; path_norm(eff, norm, sizeof norm);          /* tidy logical cwd for getcwd */
        snprintf(g_cwd, sizeof g_cwd, "%s", norm);
        return 0;
    }
    return chdir(path) == 0 ? 0 : (int)pal_errno();
}
long pal_host_getcwd(char *buf, size_t size) {
    if (g_confined) {
        size_t n = strlen(g_cwd);
        if (n + 1 > size) { errno = ERANGE; return pal_errno(); }
        memcpy(buf, g_cwd, n + 1);
        return (long)n;
    }
    return getcwd(buf, size) ? (long)strlen(buf) : pal_errno();
}

/* --- the *at family (relative to a host dirfd, or AT_FDCWD) --- */
pal_file_t pal_host_openat(pal_file_t dir, const char *path, uint64_t aios_flags, uint64_t mode) {
    int hf = xlate_open_flags(aios_flags);
    if (g_confined) {
        char eff[2048]; const char *full; int base = confined_base(dir, path, eff, sizeof eff, &full);
        long fd = openat2_in_root(base, full, (uint64_t)hf, (unsigned int)mode);
        return fd < 0 ? (pal_file_t)pal_errno() : (pal_file_t)fd;
    }
    int fd = openat(hostdir(dir), path, hf, (mode_t)mode);
    return fd < 0 ? (pal_file_t)pal_errno() : (pal_file_t)fd;
}
int pal_host_fstatat(pal_file_t dir, const char *path, struct aios_stat *out, int follow) {
    struct stat st;
    if (g_confined) {
        char eff[2048]; const char *full; int base = confined_base(dir, path, eff, sizeof eff, &full);
        long fd = openat2_in_root(base, full, (uint64_t)(O_PATH | O_CLOEXEC | (follow ? 0 : O_NOFOLLOW)), 0);
        if (fd < 0) return (int)pal_errno();
        int r = fstat((int)fd, &st); int e = errno; close((int)fd);
        if (r != 0) { errno = e; return (int)pal_errno(); }
        fill_aios_stat(out, &st);
        return 0;
    }
    if (fstatat(hostdir(dir), path, &st, follow ? 0 : AT_SYMLINK_NOFOLLOW) != 0) return (int)pal_errno();
    fill_aios_stat(out, &st);
    return 0;
}
int pal_host_unlinkat(pal_file_t dir, const char *path, int removedir) {
    if (g_confined) {
        char eff[2048]; const char *full; int base = confined_base(dir, path, eff, sizeof eff, &full);
        char dirb[2048]; const char *leaf;
        long pfd = open_parent_at(base, full, dirb, sizeof dirb, &leaf);
        if (pfd < 0) return (int)pal_errno();
        int r = unlinkat((int)pfd, leaf, removedir ? AT_REMOVEDIR : 0); int e = errno; close((int)pfd);
        return r == 0 ? 0 : (errno = e, (int)pal_errno());
    }
    return unlinkat(hostdir(dir), path, removedir ? AT_REMOVEDIR : 0) == 0 ? 0 : (int)pal_errno();
}
int pal_host_faccessat(pal_file_t dir, const char *path, int amode) {
    if (g_confined) {
        char eff[2048]; const char *full; int base = confined_base(dir, path, eff, sizeof eff, &full);
        long fd = openat2_in_root(base, full, (uint64_t)(O_PATH | O_CLOEXEC), 0);   /* symlinks followed within root */
        if (fd < 0) return (int)pal_errno();
        long r = syscall(__NR_faccessat2, (int)fd, "", (long)amode, (long)AT_EMPTY_PATH);
        int e = errno; close((int)fd);
        if (r == 0) return 0;
        if (e == ENOSYS) return 0;        /* faccessat2 unavailable: the in-root path exists -> allow */
        errno = e; return (int)pal_errno();
    }
    return faccessat(hostdir(dir), path, amode, 0) == 0 ? 0 : (int)pal_errno();   /* AIOS_?_OK == ?_OK */
}
long pal_host_readlink(const char *path, char *buf, size_t bufsize) {
    if (g_confined) {
        char eff[2048]; eff_path(path, eff, sizeof eff);
        char dirb[2048]; const char *leaf;
        long pfd = open_parent_at(g_root_fd, eff, dirb, sizeof dirb, &leaf);
        if (pfd < 0) return pal_errno();
        ssize_t n = readlinkat((int)pfd, leaf, buf, bufsize); int e = errno; close((int)pfd);
        return n < 0 ? (errno = e, pal_errno()) : (long)n;
    }
    ssize_t n = readlink(path, buf, bufsize);
    return n < 0 ? pal_errno() : (long)n;
}
int pal_host_isatty(pal_file_t f) { return isatty((int)f) ? 1 : 0; }

int pal_host_clock_gettime(int clk_id, struct aios_timespec *out) {
    clockid_t c = (clk_id == AIOS_CLOCK_MONOTONIC) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    struct timespec ts;
    if (clock_gettime(c, &ts) != 0) return (int)pal_errno();
    out->tv_sec  = (long long)ts.tv_sec;
    out->tv_nsec = (long long)ts.tv_nsec;
    return 0;
}

/* --- signal delivery (register manipulation -- host-specific, so it lives here) ---
 * `who` is stopped at a syscall EXIT (its result already planted by pal_guest_setret). Make it RUN
 * its handler: save the post-syscall regs as-is (sigreturn restores them -> the guest resumes right
 * after the interrupted syscall), then jump into the handler with x0=signum, lr=tramp. */
int pal_guest_deliver(pal_pid_t who, uint64_t handler, uint64_t signum, uint64_t tramp, void *savebuf) {
    struct user_pt_regs cur;
    if (getregs(who, &cur) != 0) return -1;
    memcpy(savebuf, &cur, sizeof cur);
    struct user_pt_regs r = cur;
    r.regs[0]  = signum;                              /* handler(int signum) */
    r.regs[30] = tramp;                               /* x30 = lr = the sigreturn trampoline */
    r.pc       = handler;                             /* jump into the handler */
    if (setregs(who, &r) != 0) return -1;
    return ptrace(PTRACE_SYSCALL, who, 0, 0) == 0 ? 0 : -1;   /* resume into the handler */
}
/* The guest is stopped at the SIGRETURN syscall entry (the trampoline called it). Neutralize that,
 * run it to exit, then restore the saved pre-signal regs so the guest re-executes its deferred call. */
int pal_guest_sigreturn(pal_pid_t who, const void *savebuf) {
    set_syscall_nr(who, -1);
    if (ptrace(PTRACE_SYSCALL, who, 0, 0) != 0) return -1;
    int st;
    if (waitpid_r(who, &st, 0) < 0 || !WIFSTOPPED(st)) return -1;
    struct user_pt_regs saved;
    memcpy(&saved, savebuf, sizeof saved);
    if (setregs(who, &saved) != 0) return -1;
    return ptrace(PTRACE_SYSCALL, who, 0, 0) == 0 ? 0 : -1;   /* resume -> re-executes the deferred syscall */
}

/* A directory listing: getdents64 into a host temp, then translate each linux_dirent64 into an
 * EQUAL-SIZE aios_dirent record. The two record layouts are field-for-field identical (d_ino u64,
 * d_off s64, d_reclen u16, d_type u8, then the NUL-terminated name + padding) and d_type already
 * uses the DT_* values, so the translation is value-preserving and a translated batch never
 * outgrows its source -- it always fits in a same-size output buffer. */
struct pal_linux_dirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};
long pal_host_getdents(pal_file_t f, void *buf, size_t len) {
    char tmp[8192];
    size_t cap = len < sizeof tmp ? len : sizeof tmp;
    long n = syscall(SYS_getdents64, (int)f, tmp, cap);
    if (n < 0)  return pal_errno();
    if (n == 0) return 0;                                /* end of directory */
    size_t in = 0, out = 0;
    while (in < (size_t)n) {
        struct pal_linux_dirent64 *ld = (void *)(tmp + in);
        struct aios_dirent        *ad = (void *)((char *)buf + out);
        unsigned short rl  = ld->d_reclen;
        size_t         hdr = (size_t)((char *)ad->d_name - (char *)ad);   /* aios header size (==19) */
        ad->d_ino    = ld->d_ino;
        ad->d_off    = ld->d_off;
        ad->d_reclen = rl;                               /* identical layout -> identical length */
        ad->d_type   = ld->d_type;                       /* DT_* values match AIOS_DT_* */
        memcpy(ad->d_name, ld->d_name, rl - hdr);        /* name + trailing pad (headers same size) */
        in  += rl;
        out += rl;
    }
    return (long)out;
}
