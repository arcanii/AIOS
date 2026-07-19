/*
 * guest_tarfs.c -- a freestanding AIOS-ABI program: the Phase C.4 tarfs test. It exercises the
 * read-only tarfs end-to-end: open/read/fstat/lseek/close on a known file (/etc/hostname, "aios\n",
 * via a deliberately messy path to prove the PAL normalizes), the error paths (-ENOENT on a missing
 * file, a refused write-intent open), stat on a directory and a file -- and then the C.4 headline:
 * fork a child that execs /bin/echo BY ITS REAL PATH, a REAL VENDORED SBASE BINARY out of
 * aiosroot.tar (built against libaios with the Phase 0 x7 pin -- the first unmodified vendored
 * userland binary to run on seL4). The parent reaps it and demands exit status 0. Exit 42 iff
 * every check passes. No libc, no host headers.
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
static void die(const char *s, int code) { say(s); asys(AIOS_SYS_EXIT, code, 0, 0); for (;;) { } }
static int memeq(const char *a, const char *b, unsigned n) { for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

void __tarfs_main(long argc, char **argv) {
    (void)argc; (void)argv;
    char buf[32];
    struct aios_stat st;

    /* 1. a known file, through a deliberately messy path (the PAL must normalize) */
    long fd = asys(AIOS_SYS_OPEN, (long)"//etc/./hostname", AIOS_O_RDONLY, 0);
    if (fd < 0)                          die("guest_tarfs: FAIL -- open /etc/hostname\n", 2);
    long n = asys(AIOS_SYS_READ, fd, (long)buf, sizeof buf);
    if (n != 5 || !memeq(buf, "aios\n", 5)) die("guest_tarfs: FAIL -- read /etc/hostname\n", 2);
    if (asys(AIOS_SYS_FSTAT, fd, (long)&st, 0) != 0 ||
        st.st_size != 5 || (st.st_mode & AIOS_S_IFMT) != AIOS_S_IFREG)
                                         die("guest_tarfs: FAIL -- fstat /etc/hostname\n", 2);
    if (asys(AIOS_SYS_LSEEK, fd, 1, AIOS_SEEK_SET) != 1)
                                         die("guest_tarfs: FAIL -- lseek\n", 2);
    n = asys(AIOS_SYS_READ, fd, (long)buf, sizeof buf);
    if (n != 4 || !memeq(buf, "ios\n", 4)) die("guest_tarfs: FAIL -- read after lseek\n", 2);
    if (asys(AIOS_SYS_CLOSE, fd, 0, 0) != 0) die("guest_tarfs: FAIL -- close\n", 2);

    /* 2. the error paths: a missing file still ENOENTs. (A write-open USED to be refused here; since
     * D.3 the fs is a writable RAM tree seeded from the tar, so it now succeeds -- guest_wfs covers
     * the write semantics. Opening without writing must not disturb the seeded content.) */
    if (asys(AIOS_SYS_OPEN, (long)"/etc/nope", AIOS_O_RDONLY, 0) >= 0)
                                         die("guest_tarfs: FAIL -- open of a missing file succeeded\n", 3);
    { long wfd = asys(AIOS_SYS_OPEN, (long)"/etc/motd", AIOS_O_WRONLY, 0);
      if (wfd < 0)                       die("guest_tarfs: FAIL -- write-open on the writable fs refused\n", 3);
      asys(AIOS_SYS_CLOSE, wfd, 0, 0); }

    /* 3. stat: a directory and a file -- plus ".." paths (the normalizer's pop + root-clamp
     * branches; the kernel does NOT path_norm what it hands the PAL) */
    if (asys(AIOS_SYS_STAT, (long)"/bin", (long)&st, 0) != 0 ||
        (st.st_mode & AIOS_S_IFMT) != AIOS_S_IFDIR)
                                         die("guest_tarfs: FAIL -- stat /bin (dir)\n", 4);
    if (asys(AIOS_SYS_STAT, (long)"/etc/motd", (long)&st, 0) != 0 || st.st_size <= 0)
                                         die("guest_tarfs: FAIL -- stat /etc/motd\n", 4);
    if (asys(AIOS_SYS_STAT, (long)"/bin/../etc/hostname", (long)&st, 0) != 0 || st.st_size != 5)
                                         die("guest_tarfs: FAIL -- '..' path did not resolve\n", 4);
    if (asys(AIOS_SYS_STAT, (long)"/../etc/hostname", (long)&st, 0) != 0 || st.st_size != 5)
                                         die("guest_tarfs: FAIL -- '..' was not clamped at the root\n", 4);
    say("guest_tarfs: file ops OK (open/read/fstat/lseek/close + errors + stat)\n");

    /* 4. the headline: exec a REAL vendored sbase binary out of the tar, by its real path */
    long r = asys(AIOS_SYS_FORK, 0, 0, 0);
    if (r < 0)                           die("guest_tarfs: FAIL -- fork\n", 5);
    if (r == 0) {                        /* the child becomes /bin/echo */
        char *xargv[] = { (char *)"echo",
                          (char *)"tarfs-exec: a vendored sbase binary runs on seL4", 0 };
        asys(AIOS_SYS_EXEC, (long)"/bin/echo", (long)xargv, 0);
        die("guest_tarfs: child FAIL -- exec /bin/echo returned\n", 9);
    }
    volatile int status = -1;
    long reaped = asys(AIOS_SYS_WAIT, AIOS_WAIT_ANY, (long)&status, 0);
    if (reaped != r)                     die("guest_tarfs: FAIL -- wait reaped the wrong pid\n", 6);
    if (status != 0)                     die("guest_tarfs: FAIL -- /bin/echo exited nonzero\n", 7);

    say("guest_tarfs: parent OK -- /bin/echo (from aiosroot.tar) ran + exited 0\n");
    asys(AIOS_SYS_EXIT, 42, 0, 0);
    for (;;) { }
}

/* the AIOS SysV entry: [sp]=argc, sp+8=&argv[0] (the same contract libaios's _start uses) */
__asm__(
    ".global _start\n"
    "_start:\n"
    "  ldr x0, [sp]\n"
    "  add x1, sp, #8\n"
    "  bl  __tarfs_main\n"
    "1: b 1b\n"
);
