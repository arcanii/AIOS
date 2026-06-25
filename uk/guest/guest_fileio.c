/*
 * guest_fileio.c -- an M2 AIOS-ABI program exercising the kernel's VFS.
 *
 * Freestanding (no libc): it OPENs, WRITEs, READs, and CLOSEs through the AIOS ABI only. It
 * writes a file, reads it back, and echoes it to stdout -- proving real file I/O routed through
 * the AIOS kernel (which reaches host storage via the PAL). The program never touches Linux.
 */
#include "aios_abi.h"

static long aios_syscall3(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { aios_syscall3(AIOS_SYS_WRITE, AIOS_FD_STDOUT, (long)s, slen(s)); }

void _start(void) {
    const char *path = "/tmp/aios_m2.txt";
    const char *body = "written by an AIOS-ABI program through the AIOS kernel's VFS\n";

    /* 1. create + write (output-redirection style) */
    long wfd = aios_syscall3(AIOS_SYS_OPEN, (long)path,
                             AIOS_O_WRONLY | AIOS_O_CREAT | AIOS_O_TRUNC, 0644);
    if (wfd < 0) { out("OPEN(write) FAILED\n"); aios_syscall3(AIOS_SYS_EXIT, 1, 0, 0); }
    aios_syscall3(AIOS_SYS_WRITE, wfd, (long)body, slen(body));
    aios_syscall3(AIOS_SYS_CLOSE, wfd, 0, 0);

    /* 2. reopen + read back + echo to stdout (cat style) */
    out("--- the AIOS kernel reads the file back for the program: ---\n");
    long rfd = aios_syscall3(AIOS_SYS_OPEN, (long)path, AIOS_O_RDONLY, 0);
    if (rfd < 0) { out("OPEN(read) FAILED\n"); aios_syscall3(AIOS_SYS_EXIT, 2, 0, 0); }
    char buf[128];
    long n;
    while ((n = aios_syscall3(AIOS_SYS_READ, rfd, (long)buf, sizeof buf)) > 0)
        aios_syscall3(AIOS_SYS_WRITE, AIOS_FD_STDOUT, (long)buf, n);
    aios_syscall3(AIOS_SYS_CLOSE, rfd, 0, 0);

    aios_syscall3(AIOS_SYS_EXIT, 7, 0, 0);   /* distinct M2 exit code */
    for (;;) { }
}
