/*
 * guest_wfs.c -- a freestanding AIOS-ABI program: the Phase D.3 WRITABLE-fs test. The fs is now a
 * node tree seeded from aiosroot.tar whose file bytes start out pointing AT the tar image and are
 * copied up on the first write, so this checks both halves:
 *   - the SEEDED (read) side still works byte-exactly (/etc/hostname == "aios\n");
 *   - CREATE + write + read-back + append + O_TRUNC + lseek-overwrite in a new file;
 *   - COPY-UP: writing over a SEEDED file changes it, and the change is visible on re-open, while
 *     a DIFFERENT seeded file is untouched (no shared-buffer aliasing);
 *   - mkdir/rmdir (incl. ENOTEMPTY), unlink (incl. EISDIR/ENOENT), rename, and that a fresh file
 *     SHOWS UP in its directory's getdents listing.
 * Exit 42 iff every check passes. No libc, no host headers.
 */
#include "aios_abi.h"

static long asys(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = AIOS_GATEWAY;
    register long x9 __asm__("x9") = nr;
    register long x7 __asm__("x7") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x9), "r"(x7), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}
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
static int memeq(const char *a, const char *b, unsigned n) { for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* open + read the whole file into buf; returns the byte count, or the negative errno from open. */
static long slurp(const char *path, char *buf, long cap) {
    long fd = asys(AIOS_SYS_OPEN, (long)path, AIOS_O_RDONLY, 0);
    if (fd < 0) return fd;
    long n = asys(AIOS_SYS_READ, fd, (long)buf, cap);
    asys(AIOS_SYS_CLOSE, fd, 0, 0);
    return n;
}

void _start(void) {
    char buf[128];
    struct aios_stat st;

    /* 1. the SEEDED side still reads byte-exactly */
    long n = slurp("/etc/hostname", buf, sizeof buf);
    if (n != 5 || !memeq(buf, "aios\n", 5)) die("guest_wfs: FAIL -- seeded /etc/hostname\n", 2);

    /* 2. CREATE a new file, write, read back */
    long fd = asys(AIOS_SYS_OPEN, (long)"/tmp/w1", AIOS_O_WRONLY | AIOS_O_CREAT, 0644);
    if (fd < 0)                                        die("guest_wfs: FAIL -- create /tmp/w1\n", 3);
    if (asys(AIOS_SYS_WRITE, fd, (long)"hello", 5) != 5) die("guest_wfs: FAIL -- write /tmp/w1\n", 3);
    asys(AIOS_SYS_CLOSE, fd, 0, 0);
    n = slurp("/tmp/w1", buf, sizeof buf);
    if (n != 5 || !memeq(buf, "hello", 5))             die("guest_wfs: FAIL -- read back /tmp/w1\n", 3);
    if (asys(AIOS_SYS_STAT, (long)"/tmp/w1", (long)&st, 0) != 0 || st.st_size != 5 ||
        (st.st_mode & AIOS_S_IFMT) != AIOS_S_IFREG)   die("guest_wfs: FAIL -- stat /tmp/w1\n", 3);

    /* 3. APPEND, then O_TRUNC, then a positioned overwrite */
    fd = asys(AIOS_SYS_OPEN, (long)"/tmp/w1", AIOS_O_WRONLY | AIOS_O_APPEND, 0);
    if (fd < 0 || asys(AIOS_SYS_WRITE, fd, (long)"world", 5) != 5)
                                                       die("guest_wfs: FAIL -- append /tmp/w1\n", 4);
    asys(AIOS_SYS_CLOSE, fd, 0, 0);
    n = slurp("/tmp/w1", buf, sizeof buf);
    if (n != 10 || !memeq(buf, "helloworld", 10))      die("guest_wfs: FAIL -- append content\n", 4);

    fd = asys(AIOS_SYS_OPEN, (long)"/tmp/w1", AIOS_O_WRONLY | AIOS_O_TRUNC, 0);
    if (fd < 0)                                        die("guest_wfs: FAIL -- open O_TRUNC\n", 4);
    if (asys(AIOS_SYS_WRITE, fd, (long)"abcdef", 6) != 6) die("guest_wfs: FAIL -- write after trunc\n", 4);
    if (asys(AIOS_SYS_LSEEK, fd, 2, AIOS_SEEK_SET) != 2)  die("guest_wfs: FAIL -- lseek in a write fd\n", 4);
    if (asys(AIOS_SYS_WRITE, fd, (long)"XY", 2) != 2)     die("guest_wfs: FAIL -- positioned write\n", 4);
    asys(AIOS_SYS_CLOSE, fd, 0, 0);
    n = slurp("/tmp/w1", buf, sizeof buf);
    if (n != 6 || !memeq(buf, "abXYef", 6))            die("guest_wfs: FAIL -- trunc+overwrite content\n", 4);
    say("guest_wfs: create/write/append/trunc/seek-write OK\n");

    /* 4. COPY-UP: overwrite a SEEDED file; a sibling seeded file must be untouched */
    fd = asys(AIOS_SYS_OPEN, (long)"/etc/hostname", AIOS_O_WRONLY | AIOS_O_TRUNC, 0);
    if (fd < 0)                                        die("guest_wfs: FAIL -- open seeded file for write\n", 5);
    if (asys(AIOS_SYS_WRITE, fd, (long)"sel4\n", 5) != 5) die("guest_wfs: FAIL -- write seeded file\n", 5);
    asys(AIOS_SYS_CLOSE, fd, 0, 0);
    n = slurp("/etc/hostname", buf, sizeof buf);
    if (n != 5 || !memeq(buf, "sel4\n", 5))            die("guest_wfs: FAIL -- copy-up did not stick\n", 5);
    n = slurp("/etc/motd", buf, sizeof buf);           /* a DIFFERENT seeded file must be intact */
    if (n <= 0 || !memeq(buf, "Welcome", 7))          die("guest_wfs: FAIL -- copy-up corrupted a sibling\n", 5);
    say("guest_wfs: copy-up OK (a seeded file is writable; its siblings are untouched)\n");

    /* 5. mkdir / rmdir / unlink / rename + the error paths */
    if (asys(AIOS_SYS_MKDIR, (long)"/tmp/d", 0755, 0) != 0) die("guest_wfs: FAIL -- mkdir /tmp/d\n", 6);
    if (asys(AIOS_SYS_MKDIR, (long)"/tmp/d", 0755, 0) != -AIOS_EEXIST)
                                                       die("guest_wfs: FAIL -- mkdir twice != EEXIST\n", 6);
    if (asys(AIOS_SYS_STAT, (long)"/tmp/d", (long)&st, 0) != 0 ||
        (st.st_mode & AIOS_S_IFMT) != AIOS_S_IFDIR)   die("guest_wfs: FAIL -- stat /tmp/d\n", 6);
    fd = asys(AIOS_SYS_OPEN, (long)"/tmp/d/inner", AIOS_O_WRONLY | AIOS_O_CREAT, 0644);
    if (fd < 0)                                        die("guest_wfs: FAIL -- create in a new dir\n", 6);
    asys(AIOS_SYS_CLOSE, fd, 0, 0);
    if (asys(AIOS_SYS_RMDIR, (long)"/tmp/d", 0, 0) != -AIOS_ENOTEMPTY)
                                                       die("guest_wfs: FAIL -- rmdir non-empty != ENOTEMPTY\n", 6);
    if (asys(AIOS_SYS_UNLINK, (long)"/tmp/d", 0, 0) != -AIOS_EISDIR)
                                                       die("guest_wfs: FAIL -- unlink a dir != EISDIR\n", 6);
    if (asys(AIOS_SYS_UNLINK, (long)"/tmp/d/inner", 0, 0) != 0)
                                                       die("guest_wfs: FAIL -- unlink /tmp/d/inner\n", 6);
    if (asys(AIOS_SYS_RMDIR, (long)"/tmp/d", 0, 0) != 0) die("guest_wfs: FAIL -- rmdir /tmp/d\n", 6);
    if (asys(AIOS_SYS_STAT, (long)"/tmp/d", (long)&st, 0) != -AIOS_ENOENT)
                                                       die("guest_wfs: FAIL -- /tmp/d survived rmdir\n", 6);

    if (asys(AIOS_SYS_RENAME, (long)"/tmp/w1", (long)"/tmp/w2", 0) != 0)
                                                       die("guest_wfs: FAIL -- rename\n", 7);
    n = slurp("/tmp/w2", buf, sizeof buf);
    if (n != 6 || !memeq(buf, "abXYef", 6))            die("guest_wfs: FAIL -- renamed content\n", 7);
    if (slurp("/tmp/w1", buf, sizeof buf) != -AIOS_ENOENT)
                                                       die("guest_wfs: FAIL -- the old name survived rename\n", 7);
    say("guest_wfs: mkdir/rmdir/unlink/rename OK (with EEXIST/ENOTEMPTY/EISDIR/ENOENT)\n");

    /* 6. a fresh file appears in its directory's getdents listing */
    long dfd = asys(AIOS_SYS_OPEN, (long)"/tmp", AIOS_O_RDONLY | AIOS_O_DIRECTORY, 0);
    if (dfd < 0)                                       die("guest_wfs: FAIL -- open /tmp\n", 8);
    int found = 0;
    for (;;) {
        long g = asys(AIOS_SYS_GETDENTS, dfd, (long)buf, sizeof buf);
        if (g < 0)  die("guest_wfs: FAIL -- getdents /tmp\n", 8);
        if (g == 0) break;
        long off = 0;
        while (off < g) {
            struct aios_dirent *d = (struct aios_dirent *)(buf + off);
            if (streq(d->d_name, "w2")) found = 1;
            off += d->d_reclen;
        }
    }
    asys(AIOS_SYS_CLOSE, dfd, 0, 0);
    if (!found)                                        die("guest_wfs: FAIL -- w2 missing from /tmp getdents\n", 8);

    /* 7. faccessat now grants W_OK (the fs is writable) */
    if (asys4(AIOS_SYS_FACCESSAT, AIOS_AT_FDCWD, (long)"/tmp/w2", AIOS_W_OK, 0) != 0)
                                                       die("guest_wfs: FAIL -- faccessat W_OK on a writable fs\n", 9);

    /* 8. PARTIAL IN-PLACE WRITE over a SEEDED file -- O_RDWR with NO O_TRUNC (the review's must-fix:
     * the copy-up used to keep only the written prefix while leaving st_size at the old length, so
     * the tail was destroyed AND every later read ran off the end of the heap buffer). /etc/motd is
     * ~192 bytes and starts "Welcome"; overwrite the first 5 and require the TAIL to survive and the
     * size to be unchanged. */
    if (asys(AIOS_SYS_STAT, (long)"/etc/motd", (long)&st, 0) != 0 || st.st_size < 32)
                                                       die("guest_wfs: FAIL -- stat /etc/motd (setup)\n", 10);
    long motd_size = (long)st.st_size;
    char tail_before[32];
    fd = asys(AIOS_SYS_OPEN, (long)"/etc/motd", AIOS_O_RDONLY, 0);
    if (fd < 0 || asys(AIOS_SYS_LSEEK, fd, motd_size - 16, AIOS_SEEK_SET) != motd_size - 16 ||
        asys(AIOS_SYS_READ, fd, (long)tail_before, 16) != 16)
                                                       die("guest_wfs: FAIL -- read /etc/motd tail\n", 10);
    asys(AIOS_SYS_CLOSE, fd, 0, 0);

    fd = asys(AIOS_SYS_OPEN, (long)"/etc/motd", AIOS_O_RDWR, 0);   /* NO O_TRUNC */
    if (fd < 0)                                        die("guest_wfs: FAIL -- open /etc/motd O_RDWR\n", 10);
    if (asys(AIOS_SYS_WRITE, fd, (long)"HELLO", 5) != 5) die("guest_wfs: FAIL -- in-place write\n", 10);
    asys(AIOS_SYS_CLOSE, fd, 0, 0);

    if (asys(AIOS_SYS_STAT, (long)"/etc/motd", (long)&st, 0) != 0 || st.st_size != motd_size)
                                                       die("guest_wfs: FAIL -- in-place write changed the size (tail lost)\n", 10);
    char tail_after[32];
    fd = asys(AIOS_SYS_OPEN, (long)"/etc/motd", AIOS_O_RDONLY, 0);
    if (fd < 0 || asys(AIOS_SYS_LSEEK, fd, motd_size - 16, AIOS_SEEK_SET) != motd_size - 16 ||
        asys(AIOS_SYS_READ, fd, (long)tail_after, 16) != 16)
                                                       die("guest_wfs: FAIL -- re-read /etc/motd tail\n", 10);
    asys(AIOS_SYS_CLOSE, fd, 0, 0);
    if (!memeq(tail_before, tail_after, 16))           die("guest_wfs: FAIL -- in-place write destroyed the tail\n", 10);
    n = slurp("/etc/motd", buf, 5);
    if (n != 5 || !memeq(buf, "HELLO", 5))             die("guest_wfs: FAIL -- in-place write did not land\n", 10);
    say("guest_wfs: partial in-place write OK (prefix updated, tail + size intact)\n");

    /* 9. rename type compatibility (POSIX): dir onto a file -> ENOTDIR, file onto a dir -> EISDIR */
    if (asys(AIOS_SYS_MKDIR, (long)"/tmp/rd", 0755, 0) != 0) die("guest_wfs: FAIL -- mkdir /tmp/rd\n", 11);
    if (asys(AIOS_SYS_RENAME, (long)"/tmp/rd", (long)"/tmp/w2", 0) != -AIOS_ENOTDIR)
                                                       die("guest_wfs: FAIL -- rename dir onto file != ENOTDIR\n", 11);
    if (asys(AIOS_SYS_RENAME, (long)"/tmp/w2", (long)"/tmp/rd", 0) != -AIOS_EISDIR)
                                                       die("guest_wfs: FAIL -- rename file onto dir != EISDIR\n", 11);

    /* 10. getdents stays stable while the directory MUTATES: list /tmp one record at a time and
     * unlink an ALREADY-LISTED entry midway -- no later entry may be skipped (the review's
     * positional-cursor bug). We must still see the sentinel created before enumeration started. */
    fd = asys(AIOS_SYS_OPEN, (long)"/tmp/victim", AIOS_O_WRONLY | AIOS_O_CREAT, 0644);
    if (fd < 0)                                        die("guest_wfs: FAIL -- create /tmp/victim\n", 12);
    asys(AIOS_SYS_CLOSE, fd, 0, 0);
    if (asys(AIOS_SYS_MKDIR, (long)"/tmp/zz", 0755, 0) != 0) die("guest_wfs: FAIL -- mkdir /tmp/zz\n", 12);
    dfd = asys(AIOS_SYS_OPEN, (long)"/tmp", AIOS_O_RDONLY | AIOS_O_DIRECTORY, 0);
    if (dfd < 0)                                       die("guest_wfs: FAIL -- reopen /tmp\n", 12);
    int saw_zz = 0, removed = 0;
    for (;;) {
        char sb[40];                                   /* tiny: ~one record per call */
        long g = asys(AIOS_SYS_GETDENTS, dfd, (long)sb, sizeof sb);
        if (g < 0)  die("guest_wfs: FAIL -- getdents during mutation\n", 12);
        if (g == 0) break;
        long off = 0;
        while (off < g) {
            struct aios_dirent *d = (struct aios_dirent *)(sb + off);
            if (streq(d->d_name, "zz")) saw_zz = 1;
            if (!removed && streq(d->d_name, "victim")) {  /* unlink an entry we ALREADY listed */
                asys(AIOS_SYS_UNLINK, (long)"/tmp/victim", 0, 0);
                removed = 1;
            }
            off += d->d_reclen;
        }
    }
    asys(AIOS_SYS_CLOSE, dfd, 0, 0);
    if (!saw_zz) die("guest_wfs: FAIL -- getdents skipped an entry after a mid-enumeration unlink\n", 12);
    say("guest_wfs: rename type checks + getdents-under-mutation OK\n");

    say("guest_wfs: OK -- the RAM fs seeded from aiosroot.tar is fully writable on seL4\n");
    asys(AIOS_SYS_EXIT, 42, 0, 0);
    for (;;) { }
}
