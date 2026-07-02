/*
 * pal_linux_common.c -- the shared Linux host-driver + ptrace INJECTOR core. NOT a standalone
 * translation unit: it is #included by exactly one trap FRONT-END (pal_linux.c or pal_seccomp.c),
 * which first defines PAL_RESUME() + PAL_TRACE_OPTS. THE ONLY files that know about Linux are these.
 *
 * The split mirrors the design: this file is "the Linux host driver" -- the host-gateway I/O
 * (open/read/write/stat/the *at family/termios/clock/getdents + M4.2 confinement) and the ptrace
 * INJECTORS (mmap/exec/fork/exit + the signal-frame register dance), all of which are identical no
 * matter HOW a guest syscall is trapped. The trap MECHANISM -- spawn + pal_guest_next + how a guest
 * is resumed "to the next trap" -- is what varies, so it lives in the front-end:
 *   pal_linux.c    PTRACE_SYSCALL (SYSEMU-style: every syscall stops; the original backend)
 *   pal_seccomp.c  seccomp SECCOMP_RET_TRACE (a BPF filter traps only the guest's syscalls; the
 *                  guest runs via PTRACE_CONT between them)
 * The one knob the injectors need is PAL_RESUME(pid) = "resume `pid` to run until its next trap"
 * (PTRACE_SYSCALL vs PTRACE_CONT). The internal "step an injected syscall to its exit" calls stay
 * PTRACE_SYSCALL in both. The AIOS kernel above (kernel/aios_kernel.c) is byte-identical either way.
 *
 * aarch64 ABI of the traced guest: syscall number in x8 (overridden via NT_ARM_SYSTEM_CALL), args
 * x0..x5, return value in x0.
 */
#ifndef PAL_RESUME
#error "pal_linux_common.c is an #included core: a front-end must define PAL_RESUME + PAL_TRACE_OPTS first"
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include <termios.h>          /* host tcgetattr/tcsetattr + struct termios (line discipline) */
#include <sys/socket.h>        /* host socket/connect (AIOS networking passes through) */
#include <poll.h>              /* ppoll -- co-wait guest events + socket readiness (park/wake) */
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>        /* __NR_* for the host syscalls we inject */
#include <sys/uio.h>
#include <sys/wait.h>
#include <linux/elf.h>          /* NT_PRSTATUS */
#include <asm/ptrace.h>         /* struct user_pt_regs */


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

/* Decode the AIOS syscall number a trapped svc carries (used by both front-ends' pal_guest_next).
 * Guests trap via the Linux/aarch64 GATEWAY convention (aios_abi.h): x8 = AIOS_GATEWAY (an in-range
 * real syscall, so seccomp -- which ignores out-of-range numbers -- can trap it) and the real AIOS
 * number in x9. A real Linux number left in x8 (NOT the gateway) is a guest escape attempt: it is
 * surfaced as-is (< 0x1000), so the kernel's M4 policy kills the guest. Args stay in x0..x5. */
static uint64_t pal_trapped_nr(const struct user_pt_regs *r) {
    return ((uint64_t)r->regs[8] == AIOS_GATEWAY) ? (uint64_t)r->regs[9] : (uint64_t)r->regs[8];
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
/* Terminal-signal routing (job control). The kernel owns the controlling terminal; the guests are
 * moved OFF the kernel's host process group (setpgid in the spawn child below), so the host pty no
 * longer delivers ^C/^Z to them -- only to the kernel. This handler just RECORDS the caught terminal
 * signal (and lacks SA_RESTART, so a blocking host read the kernel is mid-way through returns EINTR);
 * the kernel reads it via pal_take_term_signal and forwards it to the FOREGROUND process group. A
 * handler for SIGTSTP is also what stops the KERNEL itself from being suspended by ^Z. */
static volatile sig_atomic_t g_term_sig;
static void pal_term_handler(int s) { g_term_sig = s; }
int pal_take_term_signal(void) { int s = (int)g_term_sig; g_term_sig = 0; return s; }

static void pal_fs_init_once(void);              /* M4.2: defined with the confinement layer below */
/* M4.2 confinement state + opener, forward-declared so pal_guest_exec (above the layer) can clamp the
 * exec path to the root. The actual definitions (with initializers) are in the confinement layer. */
static int  g_confined;
static int  g_root_fd;
static long openat2_in_root(int dirfd, const char *path, uint64_t hflags, unsigned int mode);



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
    return PAL_RESUME(who) == 0 ? 0 : -1;   /* resume toward the next syscall */
}

int pal_guest_resume(pal_pid_t who) {
    return PAL_RESUME(who) == 0 ? 0 : -1;
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

/* Stage a kernel string `s` into guest `who`'s own stack scratch (below sp, inside the current stack
 * page -- dead space the active frames do not use; execve copies the path before unmapping). Returns
 * the guest address, or 0 on failure. */
static uint64_t stage_str(pal_pid_t who, uint64_t sp, const char *s) {
    size_t len = 0; while (s[len]) len++;
    uint64_t pagestart = sp & ~(uint64_t)0xFFF;
    uint64_t scratch   = (sp - 512) & ~(uint64_t)0xF;
    if (scratch < pagestart) scratch = pagestart;
    if (sp - scratch < (uint64_t)len + 1) return 0;
    if (pal_guest_write(who, scratch, s, len + 1) != len + 1) return 0;
    return scratch;
}

/* Replace `who`'s image by rewriting the trapped AIOS_SYS_EXEC svc into a Linux execve of `abspath`
 * (a kernel string the kernel resolved against the process's cwd; staged into `who`). M4.3: when the
 * PAL is confined, abspath is clamped to the AIOS root + canonicalized via /proc/self/fd, so a guest
 * can only launch in-root binaries (the operator's INIT spawn goes through pal_guest_spawn, not here).
 * execve has no normal return on success: PTRACE_O_TRACEEXEC turns it into a PTRACE_EVENT_EXEC stop,
 * after which the new image is live (the kernel resumes it; pal_guest_next then skips the trailing
 * execve exit and lands on the new program's first syscall). On failure the guest gets -errno. */
int pal_guest_exec(pal_pid_t who, const char *abspath, uint64_t gargv, uint64_t genvp) {
    struct user_pt_regs saved, r;
    if (getregs(who, &saved) != 0) return -1;

    const char *hostpath = abspath;
    char canon[2048];
    if (g_confined) {                                            /* clamp + canonicalize to the root */
        long fd = openat2_in_root(g_root_fd, abspath, (uint64_t)(O_PATH | O_CLOEXEC), 0);
        if (fd < 0) { pal_guest_setret(who, (uint64_t)(long)-errno); return -1; }   /* not in root */
        char proc[64]; snprintf(proc, sizeof proc, "/proc/self/fd/%ld", fd);
        ssize_t cl = readlink(proc, canon, sizeof canon - 1);
        close((int)fd);
        if (cl <= 0) { pal_guest_setret(who, (uint64_t)(long)-EACCES); return -1; }
        canon[cl] = '\0';
        hostpath = canon;
    }

    uint64_t addr = stage_str(who, saved.sp, hostpath);          /* path into the guest's stack scratch */
    if (addr == 0) { pal_guest_setret(who, (uint64_t)(long)-ENAMETOOLONG); return -1; }

    r = saved;
    r.regs[0] = addr;
    r.regs[1] = gargv;
    r.regs[2] = genvp;
    if (setregs(who, &r) != 0) return -1;
    if (set_syscall_nr(who, __NR_execve) != 0) return -1;

    if (ptrace(PTRACE_SYSCALL, who, 0, 0) != 0) return -1;   /* run execve */
    int st;
    if (waitpid_r(who, &st, 0) < 0) return -1;

    if (WIFSTOPPED(st) && (st >> 8) == (SIGTRAP | (PTRACE_EVENT_EXEC << 8)))
        return 0;                                            /* success: new image live */
    if (WIFSTOPPED(st)) {                                     /* execve failed: restore + plant -1 */
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

/* Resolve `path` (under basefd) INSIDE the root to a CANONICAL real host path in `out`, following the
 * final symlink unless `nofollow`. openat2(RESOLVE_IN_ROOT) guarantees the result is under the root,
 * so a plain host metadata op on `out` cannot be redirected out of the root by a final symlink the
 * guest planted (the M4.3 exec trick, reused for chmod/chown/utimensat). 0, or -1 with errno set. */
static long confined_canon(int basefd, const char *path, int nofollow, char *out, size_t outsz) {
    long fd = openat2_in_root(basefd, path, (uint64_t)(O_PATH | O_CLOEXEC | (nofollow ? O_NOFOLLOW : 0)), 0);
    if (fd < 0) return -1;                               /* errno set by openat2 */
    char proc[64];
    snprintf(proc, sizeof proc, "/proc/self/fd/%ld", fd);
    ssize_t n = readlink(proc, out, outsz - 1);
    int e = errno; close((int)fd);
    if (n < 0) { errno = e; return -1; }
    out[n] = '\0';
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
/* chdir is now VERIFY-ONLY: the per-process cwd lives in the kernel, so the PAL just confirms `path`
 * (an absolute path the kernel built) is a reachable directory -- it mutates no PAL state (no host
 * chdir, so the tracer's real cwd never moves, which keeps a relative AIOS_ROOT anchored). */
int pal_host_chdir(const char *path) {
    long fd = g_confined
        ? openat2_in_root(g_root_fd, path, (uint64_t)(O_PATH | O_DIRECTORY | O_CLOEXEC), 0)
        : (long)open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return (int)pal_errno();
    close((int)fd);
    return 0;
}
/* getcwd is only used to SEED init's cwd now (the kernel owns the per-process cwd): the tracer's real
 * dir unconfined, or "/" confined. */
long pal_host_getcwd(char *buf, size_t size) {
    if (g_confined) {
        if (size < 2) { errno = ERANGE; return pal_errno(); }
        buf[0] = '/'; buf[1] = '\0';
        return 1;
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

/* --- networking (host-passthrough). AIOS domain/type/protocol + sockaddr layout match the host's, so
 * these forward straight through; AIOS errno == host errno, so -errno is the negated AIOS code. --- */
/* Every AIOS socket is made NON-BLOCKING at the host, so a serviced read/write/connect/accept that
 * would block returns EAGAIN/EINPROGRESS instead of stalling the single-threaded kernel; the kernel
 * PARKS the guest (which still sees ordinary blocking semantics) and the trap front-end wakes it via
 * the readiness co-wait. EAGAIN == -PAL_EWOULDBLOCK already (errno 11), so pal_host_read/write need no
 * change; only connect's EINPROGRESS is remapped, below. */
static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}
pal_file_t pal_host_socket(int domain, int type, int protocol) {
    int fd = socket(domain, type, protocol);
    if (fd < 0) return (pal_file_t)(-errno);
    set_nonblock(fd);
    return (pal_file_t)fd;
}
int pal_host_connect(pal_file_t f, const void *addr, unsigned int addrlen) {
    if (connect((int)f, (const struct sockaddr *)addr, (socklen_t)addrlen) == 0) return 0;
    if (errno == EINPROGRESS || errno == EALREADY) return PAL_EWOULDBLOCK;   /* in flight -> park on writable */
    return -errno;
}
int pal_host_bind(pal_file_t f, const void *addr, unsigned int addrlen) {
    return bind((int)f, (const struct sockaddr *)addr, (socklen_t)addrlen) == 0 ? 0 : -errno;
}
int pal_host_listen(pal_file_t f, int backlog) {
    return listen((int)f, backlog) == 0 ? 0 : -errno;
}
/* accept: fill `addr` (up to *addrlen bytes) with the peer address, set *addrlen to the ACTUAL peer
 * length, and return a new (non-blocking) host socket -- or PAL_EWOULDBLOCK (no pending connection) /
 * -errno. A NULL addr/addrlen means "don't want the peer". */
pal_file_t pal_host_accept(pal_file_t f, void *addr, unsigned int *addrlen) {
    socklen_t sl = addrlen ? (socklen_t)*addrlen : 0;
    int c = accept((int)f, (struct sockaddr *)addr, addr ? &sl : NULL);
    if (c < 0) return (pal_file_t)(errno == EAGAIN || errno == EWOULDBLOCK ? PAL_EWOULDBLOCK : -errno);
    set_nonblock(c);                                  /* the accepted socket parks on read too */
    if (addrlen) *addrlen = (unsigned int)sl;
    return (pal_file_t)c;
}
int pal_host_setsockopt(pal_file_t f, int level, int optname, const void *optval, unsigned int optlen) {
    return setsockopt((int)f, level, optname, optval, (socklen_t)optlen) == 0 ? 0 : -errno;
}
int pal_host_getsockname(pal_file_t f, void *addr, unsigned int *addrlen) {
    socklen_t sl = addrlen ? (socklen_t)*addrlen : 0;
    if (getsockname((int)f, (struct sockaddr *)addr, &sl) != 0) return -errno;
    if (addrlen) *addrlen = (unsigned int)sl;
    return 0;
}
int pal_host_sock_error(pal_file_t f) {                /* getsockopt SO_ERROR -- non-blocking connect completion */
    int err = 0; socklen_t sl = sizeof err;
    if (getsockopt((int)f, SOL_SOCKET, SO_ERROR, &err, &sl) != 0) return -errno;
    return err == 0 ? 0 : -err;
}
int pal_host_sock_writable(pal_file_t f) {             /* has a non-blocking connect finished (POLLOUT/ERR)? */
    struct pollfd pfd = { .fd = (int)f, .events = POLLOUT, .revents = 0 };
    int r = poll(&pfd, 1, 0);
    if (r < 0) return -errno;
    return (r > 0 && (pfd.revents & (POLLOUT | POLLERR | POLLHUP))) ? 1 : 0;
}

/* --- socket-readiness co-wait (park/wake) ---
 * The kernel publishes the sockets its parked guests wait on (reset + add each loop); pal_net_wait_ready
 * blocks in ppoll on that set with SIGCHLD momentarily unblocked, so a guest event (which stops a tracee
 * and raises SIGCHLD to the tracer) OR a ready socket wakes it. It returns 1 iff a socket is ready; on a
 * SIGCHLD/terminal-signal EINTR (or the safety timeout) it returns 0 so the caller collects the guest
 * event with a non-blocking waitpid. SIGCHLD is BLOCKED outside ppoll (installed once, below) so no
 * child-event wakeup is lost in the window between the caller's waitpid and this ppoll. */
#define PAL_NET_MAX_WATCH 64
static struct { int fd; short events; } g_net_watch[PAL_NET_MAX_WATCH];
static int g_net_nwatch;
static int g_net_timeout_ms = -1;                     /* the kernel's earliest park deadline, or -1 = none */
void pal_net_watch_reset(void) { g_net_nwatch = 0; g_net_timeout_ms = -1; }
void pal_net_watch_add(pal_file_t f, int want_write) {
    if (g_net_nwatch >= PAL_NET_MAX_WATCH) return;
    g_net_watch[g_net_nwatch].fd = (int)f;
    g_net_watch[g_net_nwatch].events = want_write ? POLLOUT : POLLIN;
    g_net_nwatch++;
}
void pal_net_watch_timeout(int ms) { g_net_timeout_ms = ms; }   /* ms until the kernel's earliest deadline */
int pal_net_have_watches(void) { return g_net_nwatch > 0; }
static void pal_net_chld_noop(int s) { (void)s; }
static void pal_net_init_signals(void) {              /* idempotent: SIGCHLD deliverable + blocked (ppoll unblocks it) */
    struct sigaction sa; sa.sa_handler = pal_net_chld_noop; sa.sa_flags = 0; sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGCHLD); sigprocmask(SIG_BLOCK, &m, NULL);
}
int pal_net_wait_ready(void) {
    if (g_net_nwatch == 0) return 0;
    struct pollfd pfds[PAL_NET_MAX_WATCH];
    for (int i = 0; i < g_net_nwatch; i++) {
        pfds[i].fd = g_net_watch[i].fd; pfds[i].events = g_net_watch[i].events; pfds[i].revents = 0;
    }
    sigset_t mask; sigprocmask(SIG_SETMASK, NULL, &mask); sigdelset(&mask, SIGCHLD);  /* deliver SIGCHLD during the wait */
    /* wake by the kernel's earliest park deadline (SO_RCVTIMEO) if sooner than the 1s safety net */
    int budget = (g_net_timeout_ms >= 0 && g_net_timeout_ms < 1000) ? g_net_timeout_ms : 1000;
    struct timespec to = { budget / 1000, (long)(budget % 1000) * 1000000L };
    int r = ppoll(pfds, (nfds_t)g_net_nwatch, &to, &mask);
    /* >=0 (a socket is ready OR the wait elapsed) surfaces to the kernel (code 4) so it retries parked
     * ops AND expires any timed-out reads; <0 (EINTR: SIGCHLD/^C) -> the caller collects the guest event */
    return r >= 0 ? 1 : 0;
}

/* AIOS termios <-> host termios. The shadow <termios.h> flag/c_cc values match the host's, so this is
 * a field copy (a future seL4 PAL would remap each flag). c_cc is copied up to the smaller NCCS. */
static void host_to_aios_termios(const struct termios *h, struct aios_termios *a) {
    a->c_iflag = h->c_iflag; a->c_oflag = h->c_oflag; a->c_cflag = h->c_cflag; a->c_lflag = h->c_lflag;
    a->c_line  = h->c_line;
    for (unsigned i = 0; i < AIOS_NCCS && i < NCCS; i++) a->c_cc[i] = h->c_cc[i];
    a->c_ispeed = cfgetispeed(h); a->c_ospeed = cfgetospeed(h);
}
static void aios_to_host_termios(const struct aios_termios *a, struct termios *h) {
    h->c_iflag = a->c_iflag; h->c_oflag = a->c_oflag; h->c_cflag = a->c_cflag; h->c_lflag = a->c_lflag;
    h->c_line  = a->c_line;
    for (unsigned i = 0; i < AIOS_NCCS && i < NCCS; i++) h->c_cc[i] = a->c_cc[i];
    cfsetispeed(h, a->c_ispeed); cfsetospeed(h, a->c_ospeed);
}
int pal_host_tcgetattr(pal_file_t f, struct aios_termios *out) {
    struct termios h;
    if (tcgetattr((int)f, &h) != 0) return (int)pal_errno();
    host_to_aios_termios(&h, out);
    return 0;
}
int pal_host_tcsetattr(pal_file_t f, int actions, const struct aios_termios *in) {
    struct termios h;
    if (tcgetattr((int)f, &h) != 0) return (int)pal_errno();   /* start from current -> preserve unmapped fields */
    aios_to_host_termios(in, &h);
    if (tcsetattr((int)f, actions, &h) != 0) return (int)pal_errno();
    return 0;
}

int pal_host_clock_gettime(int clk_id, struct aios_timespec *out) {
    clockid_t c = (clk_id == AIOS_CLOCK_MONOTONIC) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    struct timespec ts;
    if (clock_gettime(c, &ts) != 0) return (int)pal_errno();
    out->tv_sec  = (long long)ts.tv_sec;
    out->tv_nsec = (long long)ts.tv_nsec;
    return 0;
}

/* --- file-metadata *at family. Confined single-target ops go through confined_canon (so a final
 * symlink cannot redirect the change to a host file); create ops confine the parent dir. --- */
#ifndef AT_SYMLINK_FOLLOW
#define AT_SYMLINK_FOLLOW 0x400
#endif
int pal_host_fchmodat(pal_file_t dir, const char *path, unsigned int mode, int nofollow) {
    if (g_confined) {
        char eff[2048]; const char *full; int base = confined_base(dir, path, eff, sizeof eff, &full);
        char canon[2048];
        if (confined_canon(base, full, nofollow, canon, sizeof canon) != 0) return (int)pal_errno();
        return fchmodat(AT_FDCWD, canon, (mode_t)mode, 0) == 0 ? 0 : (int)pal_errno();
    }
    return fchmodat(hostdir(dir), path, (mode_t)mode, 0) == 0 ? 0 : (int)pal_errno();
}
int pal_host_fchownat(pal_file_t dir, const char *path, unsigned int owner, unsigned int group, int nofollow) {
    if (g_confined) {
        char eff[2048]; const char *full; int base = confined_base(dir, path, eff, sizeof eff, &full);
        char canon[2048];
        if (confined_canon(base, full, nofollow, canon, sizeof canon) != 0) return (int)pal_errno();
        return fchownat(AT_FDCWD, canon, (uid_t)owner, (gid_t)group, nofollow ? AT_SYMLINK_NOFOLLOW : 0) == 0
                   ? 0 : (int)pal_errno();
    }
    return fchownat(hostdir(dir), path, (uid_t)owner, (gid_t)group, nofollow ? AT_SYMLINK_NOFOLLOW : 0) == 0
               ? 0 : (int)pal_errno();
}
int pal_host_symlinkat(const char *target, pal_file_t newdir, const char *linkpath) {
    if (g_confined) {
        char eff[2048]; const char *full; int base = confined_base(newdir, linkpath, eff, sizeof eff, &full);
        char dirb[2048]; const char *leaf;
        long pfd = open_parent_at(base, full, dirb, sizeof dirb, &leaf);   /* link created in a confined dir */
        if (pfd < 0) return (int)pal_errno();
        int r = symlinkat(target, (int)pfd, leaf); int e = errno; close((int)pfd);
        return r == 0 ? 0 : (errno = e, (int)pal_errno());                 /* target stored verbatim (resolved confined later) */
    }
    return symlinkat(target, hostdir(newdir), linkpath) == 0 ? 0 : (int)pal_errno();
}
int pal_host_linkat(pal_file_t olddir, const char *oldpath, pal_file_t newdir, const char *newpath, int follow) {
    if (g_confined) {
        char effo[2048]; const char *fullo; int baseo = confined_base(olddir, oldpath, effo, sizeof effo, &fullo);
        char canono[2048];
        if (confined_canon(baseo, fullo, follow ? 0 : 1, canono, sizeof canono) != 0) return (int)pal_errno();
        char effn[2048]; const char *fulln; int basen = confined_base(newdir, newpath, effn, sizeof effn, &fulln);
        char dirb[2048]; const char *leaf;
        long pfd = open_parent_at(basen, fulln, dirb, sizeof dirb, &leaf);
        if (pfd < 0) return (int)pal_errno();
        int r = linkat(AT_FDCWD, canono, (int)pfd, leaf, 0); int e = errno; close((int)pfd);
        return r == 0 ? 0 : (errno = e, (int)pal_errno());
    }
    return linkat(hostdir(olddir), oldpath, hostdir(newdir), newpath, follow ? AT_SYMLINK_FOLLOW : 0) == 0
               ? 0 : (int)pal_errno();
}
int pal_host_utimensat(pal_file_t dir, const char *path, const struct aios_timespec *times, int nofollow) {
    struct timespec ts[2], *tp = NULL;
    if (times) {
        ts[0].tv_sec = (time_t)times[0].tv_sec; ts[0].tv_nsec = (long)times[0].tv_nsec;
        ts[1].tv_sec = (time_t)times[1].tv_sec; ts[1].tv_nsec = (long)times[1].tv_nsec;
        tp = ts;
    }
    if (g_confined) {
        char eff[2048]; const char *full; int base = confined_base(dir, path, eff, sizeof eff, &full);
        char canon[2048];
        if (confined_canon(base, full, nofollow, canon, sizeof canon) != 0) return (int)pal_errno();
        return utimensat(AT_FDCWD, canon, tp, 0) == 0 ? 0 : (int)pal_errno();
    }
    return utimensat(hostdir(dir), path, tp, nofollow ? AT_SYMLINK_NOFOLLOW : 0) == 0 ? 0 : (int)pal_errno();
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
    return PAL_RESUME(who) == 0 ? 0 : -1;   /* resume into the handler */
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
    return PAL_RESUME(who) == 0 ? 0 : -1;   /* resume -> re-executes the deferred syscall */
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
