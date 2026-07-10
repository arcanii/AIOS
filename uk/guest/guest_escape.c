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

/* A LEGIT AIOS syscall: the Linux/aarch64 GATEWAY convention (aios_abi.h) -- x8 = AIOS_GATEWAY, the
 * real AIOS number in x9. This is exactly what libaios + every well-behaved guest emits. */
static long aios_syscall3(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = AIOS_GATEWAY;
    register long x9 __asm__("x9") = nr;
    register long x7 __asm__("x7") = nr;   /* seL4 fault-model: nr in x7 (see uk/lib/libaios.c) */
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x9), "r"(x7), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}
/* THE ESCAPE: a RAW Linux syscall -- the real number goes straight in x8 (NOT via the gateway), the
 * way a guest would try to call the host directly. Under either Linux PAL, x8 != AIOS_GATEWAY => the
 * kernel sees a sub-0x1000 number = an escape attempt and kills the guest (M4). Under seccomp this
 * also traps because write(64) is a real in-range syscall.
 * x7 also carries `nr` -- NOT as an AIOS number but as a DETERMINISTIC escape: seL4/aarch64 reads the
 * syscall number from x7, so a controlled x7 = 64 makes this a reliable UnknownSyscall fault (64 is
 * positive, so never a valid seL4 syscall number -> it always faults, never invokes a real seL4 call)
 * carrying a value < 0x1000, which the seL4 fault handler classifies as an escape (never mis-decodes
 * as an AIOS syscall). Without pinning x7 the escape would be codegen-dependent on seL4 -- exactly the
 * hazard the x7 pin exists to remove -- so the ESCAPE gets a controlled x7 too, just a non-AIOS one. */
static long raw_linux_syscall3(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = nr;
    register long x7 __asm__("x7") = nr;   /* seL4 fault-model: a controlled sub-0x1000 escape number */
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x7), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

void _start(void) {
    const char *before = "guest_escape: [1] legit AIOS write -- before the escape attempt\n";
    aios_syscall3(AIOS_SYS_WRITE, 1, (long)before, (long)slen(before));

    /* THE ESCAPE: a raw Linux write to the host's fd 1, bypassing the AIOS gateway entirely. */
    const char *escape = "guest_escape: [2] *** LINUX SYSCALL EXECUTED ON HOST *** (boundary breached!)\n";
    raw_linux_syscall3(LINUX_NR_WRITE, 1, (long)escape, (long)slen(escape));

    /* Only reached if the kernel did NOT kill us for the escape attempt. */
    const char *after = "guest_escape: [3] legit AIOS write -- after the escape attempt\n";
    aios_syscall3(AIOS_SYS_WRITE, 1, (long)after, (long)slen(after));

    aios_syscall3(AIOS_SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
