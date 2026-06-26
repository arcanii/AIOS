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
#include <string.h>
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

pal_pid_t pal_guest_spawn(const char *path, char *const argv[]) {
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

/* Replace `who`'s image by rewriting the trapped AIOS_SYS_EXEC svc into a Linux execve, using the
 * guest's own path/argv/envp pointers. execve has no normal return on success: PTRACE_O_TRACEEXEC
 * turns it into a PTRACE_EVENT_EXEC stop, after which the new image is live (the kernel resumes it;
 * pal_guest_next then skips the trailing execve exit and lands on the new program's first syscall).
 * On failure execve returns -errno at a normal exit; restore the guest with x0 = -1. */
int pal_guest_exec(pal_pid_t who, uint64_t gpath, uint64_t gargv, uint64_t genvp) {
    struct user_pt_regs saved, r;
    if (getregs(who, &saved) != 0) return -1;
    r = saved;
    r.regs[0] = gpath;
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

pal_file_t pal_host_open(const char *path, uint64_t aios_flags, uint64_t mode) {
    int fd = open(path, xlate_open_flags(aios_flags), (mode_t)mode);
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
    if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) return (int)pal_errno();
    fill_aios_stat(out, &st);
    return 0;
}
int  pal_host_unlink(const char *path)               { return unlink(path) == 0 ? 0 : (int)pal_errno(); }
int  pal_host_mkdir (const char *path, unsigned int m){ return mkdir(path, (mode_t)m) == 0 ? 0 : (int)pal_errno(); }
int  pal_host_rmdir (const char *path)               { return rmdir(path) == 0 ? 0 : (int)pal_errno(); }
int  pal_host_rename(const char *o, const char *n)   { return rename(o, n) == 0 ? 0 : (int)pal_errno(); }
int  pal_host_chdir (const char *path)               { return chdir(path) == 0 ? 0 : (int)pal_errno(); }
long pal_host_getcwd(char *buf, size_t size)         { return getcwd(buf, size) ? (long)strlen(buf) : pal_errno(); }

/* --- the *at family (relative to a host dirfd, or AT_FDCWD) --- */
pal_file_t pal_host_openat(pal_file_t dir, const char *path, uint64_t aios_flags, uint64_t mode) {
    int fd = openat(hostdir(dir), path, xlate_open_flags(aios_flags), (mode_t)mode);
    return fd < 0 ? (pal_file_t)pal_errno() : (pal_file_t)fd;
}
int pal_host_fstatat(pal_file_t dir, const char *path, struct aios_stat *out, int follow) {
    struct stat st;
    if (fstatat(hostdir(dir), path, &st, follow ? 0 : AT_SYMLINK_NOFOLLOW) != 0) return (int)pal_errno();
    fill_aios_stat(out, &st);
    return 0;
}
int pal_host_unlinkat(pal_file_t dir, const char *path, int removedir) {
    return unlinkat(hostdir(dir), path, removedir ? AT_REMOVEDIR : 0) == 0 ? 0 : (int)pal_errno();
}
int pal_host_faccessat(pal_file_t dir, const char *path, int amode) {
    return faccessat(hostdir(dir), path, amode, 0) == 0 ? 0 : (int)pal_errno();   /* AIOS_?_OK == ?_OK */
}
long pal_host_readlink(const char *path, char *buf, size_t bufsize) {
    ssize_t n = readlink(path, buf, bufsize);
    return n < 0 ? pal_errno() : (long)n;
}
int pal_host_isatty(pal_file_t f) { return isatty((int)f) ? 1 : 0; }

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
