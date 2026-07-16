/*
 * guest_execd.c -- a freestanding AIOS-ABI program: the Phase C.3 exec test (the EXEC'D side). It is
 * never spawned directly: guest_exec execs it with argv {"guest_execd","payload42"} and envp
 * {"AIOS=1"}. It verifies the loader staged exactly that block per the AIOS SysV contract
 * ([sp]=argc, argv, NULL, envp, NULL, strings -- what libaios's _start and environ expect) and
 * exits 42 iff everything matches. No libc, no host headers.
 */
#include "aios_abi.h"

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
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

void __execd_main(long argc, char **argv) {
    char **envp = argv + argc + 1;             /* the libaios environ convention */
    int ok = 1;
    if (argc != 2)                             { say("guest_execd: FAIL -- argc != 2\n");        ok = 0; }
    else {
        if (!streq(argv[0], "guest_execd"))    { say("guest_execd: FAIL -- bad argv[0]\n");      ok = 0; }
        if (!streq(argv[1], "payload42"))      { say("guest_execd: FAIL -- bad argv[1]\n");      ok = 0; }
        if (argv[2] != 0)                      { say("guest_execd: FAIL -- argv not NULL-terminated\n"); ok = 0; }
        if (!envp[0] || !streq(envp[0], "AIOS=1")) { say("guest_execd: FAIL -- bad envp[0]\n");  ok = 0; }
        else if (envp[1] != 0)                 { say("guest_execd: FAIL -- envp not NULL-terminated\n"); ok = 0; }
    }
    say(ok ? "guest_execd: OK -- the exec'd image received argc/argv/envp per the AIOS SysV contract\n"
           : "guest_execd: the staged stack is wrong\n");
    asys(AIOS_SYS_EXIT, ok ? 42 : 1, 0, 0);
    for (;;) { }
}

/* the AIOS SysV entry: [sp]=argc, sp+8=&argv[0] (the same contract libaios's _start uses) */
__asm__(
    ".global _start\n"
    "_start:\n"
    "  ldr x0, [sp]\n"
    "  add x1, sp, #8\n"
    "  bl  __execd_main\n"
    "1: b 1b\n"
);
