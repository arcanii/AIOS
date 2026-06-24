/*
 * guest_hello.c -- a freestanding AIOS-ABI program (the M1 "hello world").
 *
 * No libc, no host headers: it makes AIOS syscalls directly (svc #0 with the AIOS syscall number
 * in x8) so it talks to the AIOS kernel and NEVER to Linux. Built -nostdlib -static; its _start
 * is the entry point. This binary is the proof that an AIOS-ABI program runs under the AIOS
 * userspace kernel -- nothing here is Linux-specific except the aarch64 syscall calling sequence
 * (which the AIOS ABI shares for now).
 */
#include "aios_abi.h"

/* Make one AIOS syscall: nr in x8, args in x0..x2, return in x0. svc #0 traps to the AIOS kernel
 * (via the host PAL). The host kernel never sees this as one of its own syscalls. */
static long aios_syscall3(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2)
                     : "memory", "cc");
    return x0;
}

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

void _start(void) {
    const char *msg =
        "hello from an AIOS-ABI program -- serviced by the AIOS userspace kernel, not Linux\n";
    aios_syscall3(AIOS_SYS_WRITE, 1 /* fd */, (long)msg, (long)slen(msg));
    aios_syscall3(AIOS_SYS_EXIT, 42, 0, 0);
    for (;;) { }   /* unreachable: EXIT does not return */
}
