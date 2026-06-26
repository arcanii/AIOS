/*
 * guest_escape.c -- a RED-TEAM guest that tries to escape the AIOS kernel (M4 boundary proof).
 *
 * A well-behaved AIOS program only ever emits AIOS syscall numbers (>= 0x1000). This one cheats: in
 * between two legitimate AIOS writes it issues a RAW LINUX syscall (aarch64 __NR_write = 64) aimed
 * straight at the host's fd 1 -- an attempt to reach Linux behind the kernel's back.
 *
 * Before M4 the trap model let the trapped instruction run before the kernel planted its result, so
 * the Linux write EXECUTED on the host ("*** LINUX SYSCALL EXECUTED ON HOST ***" appeared). After
 * M4 the boundary is ENFORCED: the host never runs a guest-chosen syscall, and the kernel treats a
 * non-AIOS syscall as an escape attempt and kills the guest -- so neither the Linux line nor the
 * "after" line appears.
 */
#include "aios_abi.h"

#define LINUX_NR_WRITE 64   /* aarch64 __NR_write -- a REAL Linux syscall number, NOT an AIOS one */

static long syscall3(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

void _start(void) {
    const char *before = "guest_escape: [1] legit AIOS write -- before the escape attempt\n";
    syscall3(AIOS_SYS_WRITE, 1, (long)before, (long)slen(before));

    /* THE ESCAPE: a raw Linux write to the host's fd 1, bypassing the AIOS kernel entirely. */
    const char *escape = "guest_escape: [2] *** LINUX SYSCALL EXECUTED ON HOST *** (boundary breached!)\n";
    syscall3(LINUX_NR_WRITE, 1, (long)escape, (long)slen(escape));

    /* Only reached if the kernel did NOT kill us for the escape attempt. */
    const char *after = "guest_escape: [3] legit AIOS write -- after the escape attempt\n";
    syscall3(AIOS_SYS_WRITE, 1, (long)after, (long)slen(after));

    syscall3(AIOS_SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
