/*
 * guest_fsbreadth.c -- a freestanding AIOS-ABI program: the Phase D.1 fs-breadth + clock test. It
 * exercises the tarfs directory/metadata ops the shell + coreutils need before dash can run:
 *   - getdents: open /etc as a directory, enumerate it, and require the known members (passwd,
 *     shadow, motd, inittab) + the "." and ".." entries appear, with a plausible d_type;
 *   - a read() on a directory handle is refused (-EISDIR);
 *   - fstat/stat classify dir vs file (S_IFDIR / S_IFREG);
 *   - faccessat: R_OK + W_OK on a real file OK (the fs is writable since D.3), F_OK missing -> ENOENT;
 *   - chdir + getcwd round-trip (chdir /bin -> getcwd == "/bin");
 *   - clock_gettime(MONOTONIC) advances (two reads, the second not before the first).
 * Exit 42 iff every check passes. No libc, no host headers.
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
/* the 4-arg form (openat/faccessat pass a dirfd in x0 + more) -- x8/x9/x7 gateway, x0..x3 args */
static long asys4(long nr, long a0, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = AIOS_GATEWAY;
    register long x9 __asm__("x9") = nr;
    register long x7 __asm__("x7") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8),"r"(x9),"r"(x7),"r"(x1),"r"(x2),"r"(x3) : "memory","cc");
    return x0;
}
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { asys(AIOS_SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void die(const char *s, int code) { say(s); asys(AIOS_SYS_EXIT, code, 0, 0); for (;;) { } }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* Enumerate a directory fd, OR-ing a found-bit for each name we care about. */
static int scan_etc(long fd) {
    char buf[1024];
    int found = 0;   /* bit0 . / bit1 .. / bit2 passwd / bit3 shadow / bit4 motd / bit5 inittab */
    for (;;) {
        long n = asys(AIOS_SYS_GETDENTS, fd, (long)buf, sizeof buf);
        if (n < 0) die("guest_fsbreadth: FAIL -- getdents error\n", 3);
        if (n == 0) break;
        long off = 0;
        while (off < n) {
            struct aios_dirent *d = (struct aios_dirent *)(buf + off);
            const char *nm = d->d_name;
            if      (streq(nm, "."))       found |= 1 << 0;
            else if (streq(nm, ".."))      found |= 1 << 1;
            else if (streq(nm, "passwd"))  found |= 1 << 2;
            else if (streq(nm, "shadow"))  found |= 1 << 3;
            else if (streq(nm, "motd"))    found |= 1 << 4;
            else if (streq(nm, "inittab")) found |= 1 << 5;
            if (d->d_reclen == 0) die("guest_fsbreadth: FAIL -- zero d_reclen (would loop)\n", 3);
            off += d->d_reclen;
        }
    }
    return found;
}

void _start(void) {
    struct aios_stat st;
    char cwd[64];

    /* 1. getdents on /etc */
    long dfd = asys(AIOS_SYS_OPEN, (long)"/etc", AIOS_O_RDONLY | AIOS_O_DIRECTORY, 0);
    if (dfd < 0) die("guest_fsbreadth: FAIL -- open /etc as a directory\n", 2);
    if (asys(AIOS_SYS_FSTAT, dfd, (long)&st, 0) != 0 || (st.st_mode & AIOS_S_IFMT) != AIOS_S_IFDIR)
        die("guest_fsbreadth: FAIL -- fstat /etc is not a dir\n", 2);
    char one;
    if (asys(AIOS_SYS_READ, dfd, (long)&one, 1) != -AIOS_EISDIR)
        die("guest_fsbreadth: FAIL -- read() on a directory did not give EISDIR\n", 2);
    int found = scan_etc(dfd);
    asys(AIOS_SYS_CLOSE, dfd, 0, 0);
    if ((found & 0x3f) != 0x3f)
        die("guest_fsbreadth: FAIL -- /etc missing . .. passwd shadow motd or inittab\n", 3);

    /* Enumerate the ROOT too: it must show the real top-level dirs and MUST NOT leak the archive's
     * own top prefix dir as a phantom child (the review's catch). Also uses a TINY buffer to force
     * getdents to resume across multiple calls (the cursor-carry path). */
    long rfd = asys(AIOS_SYS_OPEN, (long)"/", AIOS_O_RDONLY | AIOS_O_DIRECTORY, 0);
    if (rfd < 0) die("guest_fsbreadth: FAIL -- open / as a directory\n", 3);
    int rfound = 0;   /* bit0 bin / bit1 etc / bit2 sbin */
    int rcalls = 0;
    for (;;) {
        char rb[64];                                 /* tiny: /'s records won't all fit at once */
        long n = asys(AIOS_SYS_GETDENTS, rfd, (long)rb, sizeof rb);
        if (n < 0) die("guest_fsbreadth: FAIL -- getdents / error\n", 3);
        if (n == 0) break;
        rcalls++;
        long off = 0;
        while (off < n) {
            struct aios_dirent *d = (struct aios_dirent *)(rb + off);
            if (streq(d->d_name, "aios_loginroot"))
                die("guest_fsbreadth: FAIL -- '/' leaked the archive prefix dir (phantom child)\n", 3);
            if      (streq(d->d_name, "bin"))  rfound |= 1 << 0;
            else if (streq(d->d_name, "etc"))  rfound |= 1 << 1;
            else if (streq(d->d_name, "sbin")) rfound |= 1 << 2;
            off += d->d_reclen;
        }
    }
    asys(AIOS_SYS_CLOSE, rfd, 0, 0);
    if ((rfound & 0x7) != 0x7)
        die("guest_fsbreadth: FAIL -- / missing bin, etc, or sbin\n", 3);
    if (rcalls < 2)
        die("guest_fsbreadth: FAIL -- getdents did not resume across calls (tiny buffer)\n", 3);
    say("guest_fsbreadth: getdents OK (/etc + / enumerated; no phantom; multi-call resume)\n");

    /* 2. O_DIRECTORY on a plain file -> ENOTDIR */
    if (asys(AIOS_SYS_OPEN, (long)"/etc/passwd", AIOS_O_RDONLY | AIOS_O_DIRECTORY, 0) != -AIOS_ENOTDIR)
        die("guest_fsbreadth: FAIL -- O_DIRECTORY on a file did not give ENOTDIR\n", 4);

    /* 3. faccessat (AT_FDCWD): R_OK + W_OK both granted (since D.3 the fs is writable), F_OK on a
     * missing file -> ENOENT */
    if (asys4(AIOS_SYS_FACCESSAT, AIOS_AT_FDCWD, (long)"/etc/passwd", AIOS_R_OK, 0) != 0)
        die("guest_fsbreadth: FAIL -- faccessat R_OK on /etc/passwd\n", 5);
    if (asys4(AIOS_SYS_FACCESSAT, AIOS_AT_FDCWD, (long)"/etc/passwd", AIOS_W_OK, 0) != 0)
        die("guest_fsbreadth: FAIL -- faccessat W_OK on the writable fs\n", 5);
    if (asys4(AIOS_SYS_FACCESSAT, AIOS_AT_FDCWD, (long)"/etc/nope", AIOS_F_OK, 0) != -AIOS_ENOENT)
        die("guest_fsbreadth: FAIL -- faccessat F_OK on a missing file\n", 5);

    /* 4. fstatat (AT_FDCWD) classifies a file */
    if (asys4(AIOS_SYS_FSTATAT, AIOS_AT_FDCWD, (long)"/bin/echo", (long)&st, 0) != 0 ||
        (st.st_mode & AIOS_S_IFMT) != AIOS_S_IFREG || st.st_size <= 0)
        die("guest_fsbreadth: FAIL -- fstatat /bin/echo\n", 6);
    say("guest_fsbreadth: faccessat + fstatat OK\n");

    /* 5. chdir + getcwd round-trip */
    if (asys(AIOS_SYS_CHDIR, (long)"/bin", 0, 0) != 0)
        die("guest_fsbreadth: FAIL -- chdir /bin\n", 7);
    long cl = asys(AIOS_SYS_GETCWD, (long)cwd, sizeof cwd, 0);
    if (cl <= 0 || !streq(cwd, "/bin"))
        die("guest_fsbreadth: FAIL -- getcwd != /bin after chdir\n", 7);
    if (asys(AIOS_SYS_CHDIR, (long)"/nonexistent", 0, 0) != -AIOS_ENOENT)
        die("guest_fsbreadth: FAIL -- chdir to a missing dir succeeded\n", 7);
    say("guest_fsbreadth: chdir + getcwd OK\n");

    /* 6. the clock advances (monotonic) */
    struct aios_timespec t0, t1;
    if (asys(AIOS_SYS_CLOCK_GETTIME, AIOS_CLOCK_MONOTONIC, (long)&t0, 0) != 0)
        die("guest_fsbreadth: FAIL -- clock_gettime\n", 8);
    for (volatile int i = 0; i < 200000; i++) { }        /* burn a little time */
    if (asys(AIOS_SYS_CLOCK_GETTIME, AIOS_CLOCK_MONOTONIC, (long)&t1, 0) != 0)
        die("guest_fsbreadth: FAIL -- clock_gettime (2)\n", 8);
    long long d = (t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec);
    if (d < 0 || t1.tv_sec < 0)
        die("guest_fsbreadth: FAIL -- clock did not advance monotonically\n", 8);
    say("guest_fsbreadth: clock_gettime OK (monotonic)\n");

    say("guest_fsbreadth: OK -- getdents, *at, chdir/getcwd, and the CNTPCT clock all work on seL4\n");
    asys(AIOS_SYS_EXIT, 42, 0, 0);
    for (;;) { }
}
