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

pal_pid_t pal_guest_spawn(const char *path, char *const argv[]) {
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
    if (waitpid(pid, &st, 0) < 0) return PAL_PID_NONE;   /* initial post-execv SIGTRAP stop */
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
        if (pid < 0) return -1;                            /* ECHILD: nothing left to trace */
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
        /* Non-syscall stop: a ptrace event (SIGTRAP + event byte; normally consumed by the inject
         * primitives) or a real signal. Forward real signals; swallow trace traps + group stops. */
        int deliver = (sig == SIGTRAP || sig == SIGSTOP) ? 0 : sig;
        ptrace(PTRACE_SYSCALL, pid, 0, (void *)(long)deliver);
    }
}

int pal_guest_return(pal_pid_t who, uint64_t retval) {
    /* `who` is stopped at a syscall ENTRY. Let the (ENOSYS) syscall run to its EXIT, plant the AIOS
     * result in x0, and resume it toward its next syscall (collected by a later pal_guest_next). */
    if (ptrace(PTRACE_SYSCALL, who, 0, 0) != 0) return -1;
    int st;
    if (waitpid(who, &st, 0) < 0) return -1;
    if (!WIFSTOPPED(st)) return 0;                 /* exited mid-syscall */
    struct user_pt_regs r;
    if (getregs(who, &r) != 0) return -1;
    r.regs[0] = (unsigned long long)retval;
    if (setregs(who, &r) != 0) return -1;
    return ptrace(PTRACE_SYSCALL, who, 0, 0) == 0 ? 0 : -1;   /* resume past the exit */
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
    if (waitpid(who, &st, 0) < 0 || !WIFSTOPPED(st)) return 0;
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
    if (waitpid(who, &st, 0) < 0) return -1;

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
    if (waitpid(parent, &st, 0) < 0) return PAL_PID_NONE;

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
    if (waitpid(cpid, &cst, 0) < 0) return PAL_PID_NONE;       /* the child's initial stop */
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
    if (f & AIOS_O_CREAT)  o |= O_CREAT;
    if (f & AIOS_O_TRUNC)  o |= O_TRUNC;
    if (f & AIOS_O_APPEND) o |= O_APPEND;
    return o;
}

pal_file_t pal_host_open(const char *path, uint64_t aios_flags, uint64_t mode) {
    int fd = open(path, xlate_open_flags(aios_flags), (mode_t)mode);
    return fd < 0 ? PAL_FILE_INVALID : (pal_file_t)fd;
}

long pal_host_read(pal_file_t f, void *buf, size_t len)        { return (long)read((int)f, buf, len); }
long pal_host_write(pal_file_t f, const void *buf, size_t len) { return (long)write((int)f, buf, len); }
int  pal_host_close(pal_file_t f)                              { return close((int)f); }

long long pal_host_lseek(pal_file_t f, long long off, int whence) {
    return (long long)lseek((int)f, (off_t)off, whence);   /* AIOS_SEEK_* == SEEK_* */
}
int pal_host_fstat(pal_file_t f, unsigned long long *size, unsigned int *mode) {
    struct stat st;
    if (fstat((int)f, &st) != 0) return -1;
    if (size) *size = (unsigned long long)st.st_size;
    if (mode) *mode = (unsigned int)st.st_mode;            /* st_mode layout matches AIOS_S_IF* */
    return 0;
}
