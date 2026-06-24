/*
 * pal_linux.c -- the PAL Linux backend. THE ONLY file that knows about Linux.
 *
 * Implements the gVisor-style trap model via ptrace(PTRACE_SYSEMU): each guest syscall stops
 * here at syscall-entry and is NOT executed by Linux. The AIOS kernel services it per the AIOS
 * ABI and we write the guest's return register; the next PTRACE_SYSEMU resumes the guest just
 * past its `svc`. So the guest only ever talks to the AIOS kernel -- Linux supplies the trap
 * mechanism and (via the host gateway) the actual hardware/services.
 *
 * aarch64 ABI of the traced guest: syscall number in x8, args x0..x5, return value in x0
 * (struct user_pt_regs.regs[]). A future pal_sel4.c implements the same pal.h contract with no
 * ptrace -- the AIOS kernel above this file does not change.
 */
#define _GNU_SOURCE
#include "pal.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <linux/elf.h>          /* NT_PRSTATUS */
#include <asm/ptrace.h>         /* struct user_pt_regs */

static pid_t            g_guest = -1;
static struct user_pt_regs g_regs;   /* GP register file of the guest at its last trap */

static int read_regs(void) {
    struct iovec io = { &g_regs, sizeof g_regs };
    return ptrace(PTRACE_GETREGSET, g_guest, (void *)NT_PRSTATUS, &io) == 0 ? 0 : -1;
}
static int write_regs(void) {
    struct iovec io = { &g_regs, sizeof g_regs };
    return ptrace(PTRACE_SETREGSET, g_guest, (void *)NT_PRSTATUS, &io) == 0 ? 0 : -1;
}

int pal_spawn_guest(const char *path) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child becomes the guest: ask to be traced, then exec the AIOS-ABI program. */
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        char *const argv[] = { (char *)path, NULL };
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

int pal_guest_trap_next(pal_syscall_t *out, int *exit_code) {
    for (;;) {
        /* SYSEMU: run to the next syscall-entry but do NOT execute the syscall. */
        if (ptrace(PTRACE_SYSEMU, g_guest, 0, 0) != 0) return -1;
        int st;
        if (waitpid(g_guest, &st, 0) < 0) return -1;

        if (WIFEXITED(st))  { if (exit_code) *exit_code = WEXITSTATUS(st);      return 0; }
        if (WIFSIGNALED(st)) { if (exit_code) *exit_code = 128 + WTERMSIG(st);  return 0; }
        if (!WIFSTOPPED(st)) continue;

        int sig = WSTOPSIG(st);
        if (sig == (SIGTRAP | 0x80)) {           /* a syscall-entry-stop (TRACESYSGOOD) */
            if (read_regs() != 0) return -1;
            out->nr = (uint64_t)g_regs.regs[8];
            for (int i = 0; i < 6; i++) out->arg[i] = (uint64_t)g_regs.regs[i];
            return 1;
        }
        /* Any other stop is a signal-delivery-stop; re-deliver nothing and keep going. */
    }
}

int pal_guest_return(uint64_t retval) {
    g_regs.regs[0] = (unsigned long long)retval;  /* aarch64 return value in x0 */
    return write_regs();
}

size_t pal_guest_read(uint64_t gaddr, void *dst, size_t len) {
    struct iovec local  = { dst, len };
    struct iovec remote = { (void *)(uintptr_t)gaddr, len };
    ssize_t n = process_vm_readv(g_guest, &local, 1, &remote, 1, 0);
    return n < 0 ? 0 : (size_t)n;
}

long pal_host_write(int fd, const void *buf, size_t len) {
    return (long)write(fd, buf, len);
}
