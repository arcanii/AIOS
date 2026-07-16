/*
 * guest_mmap.c -- a freestanding AIOS-ABI program: the Phase C.1 mmap test. It requests anonymous
 * memory via AIOS_SYS_MMAP (on the seL4 backend the kernel injects vspace_new_pages -- Untyped->Frames
 * mapped into this guest's seL4 VSpace), writes a pattern across two pages of the fresh region, reads
 * it back, and exits 42 iff it round-trips -- proving mmap-backed anonymous memory works end-to-end
 * over the fault-EP trap loop + the resume-past-syscall reply. No libc, no host headers.
 */
#include "aios_abi.h"

/* GATEWAY + x7 trap convention (aios_abi.h + the Phase 0 x7 pin): x8 = AIOS_GATEWAY (so seccomp traps
 * it), the AIOS number in x9 (the Linux PALs) AND x7 (the seL4 fault model). */
static long asys(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = AIOS_GATEWAY;
    register long x9 __asm__("x9") = nr;
    register long x7 __asm__("x7") = nr;   /* seL4 fault-model: nr in x7 (see uk/lib/libaios.c) */
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x9), "r"(x7), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { asys(AIOS_SYS_WRITE, 1, (long)s, (long)slen(s)); }

void _start(void) {
    say("guest_mmap: requesting AIOS_SYS_MMAP (64 KiB) via the fault-EP trap loop ...\n");
    long addr = asys(AIOS_SYS_MMAP, 64 * 1024, 0, 0);   /* len in x0; the kernel returns the vaddr in x0 */
    if (addr == 0) {
        say("guest_mmap: mmap FAILED (x0 == 0)\n");
        asys(AIOS_SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }
    /* Write a 64-bit pattern at two offsets in DIFFERENT pages of the fresh region, then read it back.
     * p[0] is in page 0; p[4096] is 32 KiB in (page 8) -- so a correct round-trip proves the multi-page
     * mapping is real, contiguous, and writable. */
    volatile unsigned long *p = (volatile unsigned long *)(unsigned long)addr;
    p[0]    = 0xA105A105A105A105UL;
    p[4096] = 0x5A105A105A105A10UL;
    int ok = (p[0] == 0xA105A105A105A105UL && p[4096] == 0x5A105A105A105A10UL);
    say(ok ? "guest_mmap: OK -- wrote + read back a pattern across pages of fresh mmap memory\n"
           : "guest_mmap: FAIL -- mmap memory did not round-trip\n");
    asys(AIOS_SYS_EXIT, ok ? 42 : 1, 0, 0);
    for (;;) { }
}
