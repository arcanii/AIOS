/* seccomp_probe.c -- bring-up experiments for the seccomp (SECCOMP_RET_TRACE) PAL backend.
 *
 * Q1 (numbering): does a seccomp filter deliver PTRACE_EVENT_SECCOMP for an OUT-OF-RANGE
 *     (AIOS-style, >= 0x1000) syscall number? Answer (arm64): NO -- only in-range numbers trap.
 *     That is why AIOS guests must trap via an IN-RANGE "gateway" syscall number (x8 = gateway,
 *     real AIOS number in x9).
 * Q2 (service path): from a PTRACE_EVENT_SECCOMP stop on the gateway, can the tracer SKIP the
 *     syscall (x8 = -1), get a syscall-EXIT stop, plant x0, and have the child observe it? This is
 *     the seccomp analogue of pal_guest_setret; if it works the whole backend is viable.
 *
 * NOT part of the build; compiled ad hoc in the container. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <linux/elf.h>
#include <asm/ptrace.h>

#ifndef PTRACE_EVENT_SECCOMP
#define PTRACE_EVENT_SECCOMP 7
#endif

static long do_svc(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static void install_filter(void) {
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),
    };
    struct sock_fprog prog = { 4, f };
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog);
}

/* The gateway must be a REAL, implemented arm64 syscall (the unassigned 294..423 gap does NOT trap,
 * like an out-of-range number). gettid (178) is universal, stable, side-effect-free, and never issued
 * by an AIOS guest. The PAL always neutralizes it (x8 = -1) so it never actually runs as gettid. */
#define GATEWAY 178   /* __NR_gettid on arm64 */

int main(void) {
    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        install_filter();
        /* the SERVICE mechanic: the parent skips this gateway syscall + plants x0 = 0x42 */
        long r = do_svc(GATEWAY, 0xbeef, 0, 0);
        _exit(r == 0x42 ? 55 : (r == -38 ? 38 : 56));   /* 55 = setret worked, 38 = ran as ENOSYS */
    }
    int st;
    waitpid(pid, &st, 0);                      /* SIGSTOP */
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)(PTRACE_O_TRACESECCOMP | PTRACE_O_EXITKILL));
    ptrace(PTRACE_CONT, pid, 0, 0);
    for (;;) {
        if (waitpid(pid, &st, 0) < 0) { printf("waitpid err\n"); break; }
        if (WIFEXITED(st))   { printf("child exited %d  (55=setret-OK, 38=ran-ENOSYS, 56=other)\n", WEXITSTATUS(st)); break; }
        if (WIFSIGNALED(st)) { printf("child killed sig %d\n", WTERMSIG(st)); break; }
        if (!WIFSTOPPED(st)) continue;
        int ev = st >> 8;
        if (ev == (SIGTRAP | (PTRACE_EVENT_SECCOMP << 8))) {
            struct user_pt_regs r; struct iovec io = { &r, sizeof r };
            ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &io);
            unsigned long nr = (unsigned long)r.regs[8];
            printf("SECCOMP event: nr=0x%lx\n", nr);
            if (nr == GATEWAY) {
                /* skip + plant x0 (the pal_guest_setret mechanic, from a seccomp stop) */
                int neg1 = -1; struct iovec sio = { &neg1, sizeof neg1 };
                ptrace(PTRACE_SETREGSET, pid, (void *)0x404 /*NT_ARM_SYSTEM_CALL*/, &sio);
                ptrace(PTRACE_SYSCALL, pid, 0, 0);
                int st2; waitpid(pid, &st2, 0);
                printf("  after skip+SYSCALL: st=0x%x WIFSTOPPED=%d sig=%d ev=%d\n",
                       st2, WIFSTOPPED(st2), WIFSTOPPED(st2)?WSTOPSIG(st2):-1, st2>>8);
                if (WIFSTOPPED(st2)) {
                    struct user_pt_regs r2; struct iovec io2 = { &r2, sizeof r2 };
                    ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &io2);
                    r2.regs[0] = 0x42;
                    ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &io2);
                }
                ptrace(PTRACE_CONT, pid, 0, 0);
            } else {
                ptrace(PTRACE_CONT, pid, 0, 0);
            }
        } else {
            int sig = WSTOPSIG(st);
            printf("other stop sig=%d ev=%d\n", sig, ev);
            ptrace(PTRACE_CONT, pid, 0, sig == SIGTRAP ? 0 : sig);
        }
    }
    return 0;
}
