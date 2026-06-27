/*
 * guest_cat.c -- a real `cat` as an AIOS-ABI program (M3: loader + argv).
 *
 * Freestanding (no libc): its _start reads argc/argv straight off the initial stack the loader
 * set up, then it OPENs the file named in argv[1] and streams it to stdout via the AIOS ABI --
 * composing M2's VFS with M3's argument passing. `aios-uk guest_cat /etc/hostname` cats a real
 * file through the AIOS kernel. The program never touches Linux.
 */
#include "aios_abi.h"

/* GATEWAY trap convention (aios_abi.h): x8 = AIOS_GATEWAY (so seccomp traps it), AIOS number in x9. */
static long asys(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = AIOS_GATEWAY;
    register long x9 __asm__("x9") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x9), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { asys(AIOS_SYS_WRITE, AIOS_FD_STDOUT, (long)s, slen(s)); }
static void aexit(long code) { asys(AIOS_SYS_EXIT, code, 0, 0); for (;;) { } }

/* C entry: argc/argv extracted from the stack by _start below. */
void cat_main(long argc, char **argv) {
    if (argc < 2) { out("usage: cat <file>\n"); aexit(2); }

    long fd = asys(AIOS_SYS_OPEN, (long)argv[1], AIOS_O_RDONLY, 0);
    if (fd < 0) { out("cat: cannot open "); out(argv[1]); out("\n"); aexit(1); }

    char buf[512];
    long n;
    while ((n = asys(AIOS_SYS_READ, fd, (long)buf, sizeof buf)) > 0)
        asys(AIOS_SYS_WRITE, AIOS_FD_STDOUT, (long)buf, n);
    asys(AIOS_SYS_CLOSE, fd, 0, 0);
    aexit(n < 0 ? 1 : 0);
}

/* Process entry. At _start the stack top holds: [argc][argv0][argv1]...[NULL][envp...]. Hand argc
 * and &argv[0] to cat_main; it EXITs via the AIOS ABI, so the loop is just a safety net. */
__asm__(
    ".global _start\n"
    "_start:\n"
    "  ldr x0, [sp]\n"        /* argc            */
    "  add x1, sp, #8\n"      /* argv (&argv[0]) */
    "  bl  cat_main\n"
    "1: b 1b\n"
);
