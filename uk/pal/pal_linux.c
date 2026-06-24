/*
 * pal_linux.c -- the PAL Linux backend. THE ONLY file that knows about Linux.
 *
 * Implements the gVisor-style trap model via ptrace(PTRACE_SYSCALL): each guest syscall stops
 * here at its entry; AIOS-numbered syscalls aren't real Linux syscalls so they ENOSYS harmlessly
 * and we plant the real AIOS result in x0 at the exit. PTRACE_SYSCALL (not SYSEMU) is what lets
 * the kernel INJECT a real host syscall (mmap) by rewriting the syscall number in place. So the
 * guest only ever talks to the AIOS kernel -- Linux supplies the trap mechanism, the memory, and
 * (via the host gateway) the actual hardware/services.
 *
 * aarch64 ABI of the traced guest: syscall number in x8, args x0..x5, return value in x0
 * (struct user_pt_regs.regs[]). A future pal_sel4.c implements the same pal.h contract with no
 * ptrace -- the AIOS kernel above this file does not change.
 */
#define _GNU_SOURCE
#include "pal.h"
#include "aios_abi.h"           /* AIOS_O_* flag values to translate */

#include <errno.h>
#include <fcntl.h>              /* O_* + open() */
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <linux/elf.h>          /* NT_PRSTATUS */
#include <asm/ptrace.h>         /* struct user_pt_regs */

static pid_t            g_guest = -1;
static struct user_pt_regs g_regs;   /* GP register file of the guest at its last trap */

static int getregs(struct user_pt_regs *r) {
    struct iovec io = { r, sizeof *r };
    return ptrace(PTRACE_GETREGSET, g_guest, (void *)NT_PRSTATUS, &io) == 0 ? 0 : -1;
}
static int setregs(const struct user_pt_regs *r) {
    struct iovec io = { (void *)r, sizeof *r };
    return ptrace(PTRACE_SETREGSET, g_guest, (void *)NT_PRSTATUS, &io) == 0 ? 0 : -1;
}
static int read_regs(void)  { return getregs(&g_regs); }
static int write_regs(void) { return setregs(&g_regs); }

int pal_spawn_guest(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child becomes the guest: ask to be traced, then exec the AIOS-ABI program. The host
         * sets up the initial stack (argc/argv/envp/auxv); the guest's _start reads argv from it.
         * (A future seL4 PAL builds this stack itself -- here the host loader does it for us.) */
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        execv(path, argv);
        _exit(127);                              /* exec failed */
    }
    int st;
    if (waitpid(pid, &st, 0) < 0) return -1;     /* initial post-execv SIGTRAP stop */
    if (!WIFSTOPPED(st)) return -1;
    /* TRACESYSGOOD: syscall-stops arrive as SIGTRAP|0x80 (distinguishable from real SIGTRAPs).
     * EXITKILL: if the AIOS kernel dies, the guest dies with it (no orphaned tracee). */
    ptrace(PTRACE_SETOPTIONS, pid, 0,
           (void *)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL));
    g_guest = pid;
    return 0;
}

/* aarch64: write a new syscall number for the kernel to dispatch (or -1 to skip). */
#ifndef NT_ARM_SYSTEM_CALL
#define NT_ARM_SYSTEM_CALL 0x404
#endif
static int set_syscall_nr(int nr) {
    struct iovec io = { &nr, sizeof nr };
    return ptrace(PTRACE_SETREGSET, g_guest, (void *)NT_ARM_SYSTEM_CALL, &io) == 0 ? 0 : -1;
}

/* Driver model: PTRACE_SYSCALL (not SYSEMU), strict entry/exit alternation. trap_next lands on a
 * syscall ENTRY; pal_guest_return / pal_guest_mmap consume the matching EXIT. AIOS syscall numbers
 * (>= 0x1000) are not real Linux syscalls, so if one runs it just returns -ENOSYS (harmless) and we
 * overwrite x0 with the real AIOS result at the exit. SYSCALL (vs SYSEMU) is what lets the kernel
 * INJECT a real syscall (mmap) by rewriting the number in place. */
int pal_guest_trap_next(pal_syscall_t *out, int *exit_code) {
    for (;;) {
        if (ptrace(PTRACE_SYSCALL, g_guest, 0, 0) != 0) return -1;   /* -> next ENTRY */
        int st;
        if (waitpid(g_guest, &st, 0) < 0) return -1;
        if (WIFEXITED(st))   { if (exit_code) *exit_code = WEXITSTATUS(st);     return 0; }
        if (WIFSIGNALED(st)) { if (exit_code) *exit_code = 128 + WTERMSIG(st);  return 0; }
        if (!WIFSTOPPED(st)) continue;
        if (WSTOPSIG(st) != (SIGTRAP | 0x80)) continue;   /* signal-delivery stop -- ignore */
        if (read_regs() != 0) return -1;
        out->nr = (uint64_t)g_regs.regs[8];
        for (int i = 0; i < 6; i++) out->arg[i] = (uint64_t)g_regs.regs[i];
        return 1;                                          /* syscall ENTRY */
    }
}

int pal_guest_return(uint64_t retval) {
    /* At the ENTRY stop: let the (harmless ENOSYS) syscall run to its EXIT, then plant the AIOS
     * result in x0 so the guest sees it. */
    if (ptrace(PTRACE_SYSCALL, g_guest, 0, 0) != 0) return -1;
    int st;
    if (waitpid(g_guest, &st, 0) < 0) return -1;
    if (!WIFSTOPPED(st)) return 0;                 /* guest exited mid-syscall */
    if (read_regs() != 0) return -1;
    g_regs.regs[0] = (unsigned long long)retval;
    return write_regs();
}

size_t pal_guest_read(uint64_t gaddr, void *dst, size_t len) {
    struct iovec local  = { dst, len };
    struct iovec remote = { (void *)(uintptr_t)gaddr, len };
    ssize_t n = process_vm_readv(g_guest, &local, 1, &remote, 1, 0);
    return n < 0 ? 0 : (size_t)n;
}

size_t pal_guest_write(uint64_t gaddr, const void *src, size_t len) {
    struct iovec local  = { (void *)src, len };
    struct iovec remote = { (void *)(uintptr_t)gaddr, len };
    ssize_t n = process_vm_writev(g_guest, &local, 1, &remote, 1, 0);
    return n < 0 ? 0 : (size_t)n;
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

/* Grow the guest's address space by rewriting the guest's AIOS_SYS_MMAP `svc` IN PLACE into a real
 * Linux mmap and letting it run -- so the guest's own syscall maps the memory and x0 receives the
 * address (no save/restore, no SYSEMU/SYSCALL mixing). Called at the AIOS_SYS_MMAP ENTRY stop;
 * consumes the matching EXIT (the kernel does NOT also call pal_guest_return for mmap). aarch64:
 * the dispatched number is set via NT_ARM_SYSTEM_CALL, args via the GP regs, result in x0.
 * Returns the mapped address (also now in the guest's x0), or 0 on failure. */
uint64_t pal_guest_mmap(size_t len) {
    /* Save the guest's registers: the rewritten mmap clobbers x0..x5, but the guest's syscall
     * wrapper expects all but x0 preserved across the `svc`. We restore them (with x0 = result). */
    struct user_pt_regs saved = g_regs;
    g_regs.regs[0] = 0;                       /* addr   = NULL               */
    g_regs.regs[1] = (unsigned long long)len; /* length                      */
    g_regs.regs[2] = 0x1 | 0x2;               /* PROT_READ | PROT_WRITE      */
    g_regs.regs[3] = 0x02 | 0x20;             /* MAP_PRIVATE | MAP_ANONYMOUS */
    g_regs.regs[4] = (unsigned long long)-1;  /* fd     = -1                 */
    g_regs.regs[5] = 0;                       /* offset = 0                  */
    if (write_regs() != 0) return 0;
    if (set_syscall_nr(222) != 0) return 0;   /* dispatch __NR_mmap (aarch64) */

    if (ptrace(PTRACE_SYSCALL, g_guest, 0, 0) != 0) return 0;   /* run mmap -> EXIT */
    int st;
    if (waitpid(g_guest, &st, 0) < 0 || !WIFSTOPPED(st)) return 0;
    if (read_regs() != 0) return 0;
    uint64_t addr = (uint64_t)g_regs.regs[0];
    if ((int64_t)addr < 0 && (int64_t)addr >= -4095) addr = 0;  /* -errno -> failure */

    /* Restore the guest's registers, with x0 = the mapped address (pc/sp are unchanged across the
     * syscall, so the saved set is correct post-svc). */
    saved.regs[0] = (unsigned long long)addr;
    g_regs = saved;
    if (write_regs() != 0) return 0;
    return addr;
}
