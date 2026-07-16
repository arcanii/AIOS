/*
 * guest_exec.c -- a freestanding AIOS-ABI program: the Phase C.3 exec test (the CALLER side). It
 * first verifies its OWN initial stack (the spawn path now hand-stages the AIOS SysV block too),
 * then execs a MISSING image -- which must FAIL cleanly with this image still running (the
 * double-buffered swap's whole point) -- and finally execs guest_execd with argv+envp, which must
 * NOT return. guest_execd checks what it received and exits 42. No libc, no host headers.
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
static void die(const char *s, int code) { say(s); asys(AIOS_SYS_EXIT, code, 0, 0); for (;;) { } }

void __exec_main(long argc, char **argv) {
    /* the spawn path staged this stack: argv[0] is the path the kernel launched ("/sbin/init") */
    if (argc < 1 || !argv[0] || argv[0][0] != '/')
        die("guest_exec: FAIL -- the spawn-staged stack is malformed\n", 2);
    say("guest_exec: spawn-staged argc/argv OK; exec'ing a MISSING image (must fail cleanly) ...\n");

    char *bad_argv[] = { (char *)"nonexistent", 0 };
    long r = asys(AIOS_SYS_EXEC, (long)"/bin/nonexistent", (long)bad_argv, 0);
    if (r >= 0) die("guest_exec: FAIL -- exec of a missing image claimed success\n", 3);
    say("guest_exec: missing-image exec failed cleanly, old image still running; exec'ing guest_execd ...\n");

    char *xargv[] = { (char *)"guest_execd", (char *)"payload42", 0 };
    char *xenvp[] = { (char *)"AIOS=1", 0 };
    asys(AIOS_SYS_EXEC, (long)"/bin/guest_execd", (long)xargv, (long)xenvp);
    die("guest_exec: FAIL -- exec of guest_execd RETURNED\n", 4);
}

/* the AIOS SysV entry: [sp]=argc, sp+8=&argv[0] (the same contract libaios's _start uses) */
__asm__(
    ".global _start\n"
    "_start:\n"
    "  ldr x0, [sp]\n"
    "  add x1, sp, #8\n"
    "  bl  __exec_main\n"
    "1: b 1b\n"
);
