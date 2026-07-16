/*
 * guest_pipe.c -- a freestanding AIOS-ABI program: the Phase C.5 pipe test. It proves the in-root-task
 * byte ring drives the kernel's park/wake fixpoint in BOTH directions across a fork:
 *   - the READER parks on an empty pipe (the parent reaches read() before the child writes) and is
 *     woken when the child writes;
 *   - the WRITER parks on a full ring (the child hands write() a 64 KiB buffer but the ring holds
 *     only ~16 KiB) and is woken as the parent drains it;
 *   - EOF arrives when the child closes its write end after the last byte.
 * A rolling pattern (byte k = k*7+3) is verified across the whole 64 KiB stream, so a lost, dropped,
 * reordered, or duplicated byte fails LOUDLY. Exit 42 iff every byte round-trips and the child exits 0.
 * No libc, no host headers.
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

#define TOTAL (64 * 1024)              /* >> the ring: forces the writer to park + be woken repeatedly */
static unsigned char pat(unsigned long k) { return (unsigned char)(k * 7 + 3); }

void _start(void) {
    int fds[2];
    unsigned char rbuf[4096];          /* the parent's receive chunk (on the stack: no big .bss) */
    if (asys(AIOS_SYS_PIPE, (long)fds, 0, 0) != 0) die("guest_pipe: FAIL -- pipe()\n", 2);

    long r = asys(AIOS_SYS_FORK, 0, 0, 0);
    if (r < 0) die("guest_pipe: FAIL -- fork()\n", 3);

    if (r == 0) {                      /* ---- the CHILD: write TOTAL bytes, then EOF ---- */
        asys(AIOS_SYS_CLOSE, fds[0], 0, 0);            /* close the read end we do not use */
        /* mmap the send buffer (rather than a 64 KiB .bss, which lands in an unaligned multi-page
         * PT_LOAD the freestanding loader dislikes) -- also exercises mmap+pipe together. */
        long a = asys(AIOS_SYS_MMAP, TOTAL, 0, 0);
        if (a == 0) { asys(AIOS_SYS_EXIT, 8, 0, 0); for (;;) { } }
        unsigned char *wbuf = (unsigned char *)(unsigned long)a;
        for (unsigned long i = 0; i < TOTAL; i++) wbuf[i] = pat(i);
        unsigned long off = 0;
        while (off < TOTAL) {                          /* one big write parks in the kernel; loop only
                                                        * covers a short return (POSIX write may be partial) */
            long w = asys(AIOS_SYS_WRITE, fds[1], (long)(wbuf + off), (long)(TOTAL - off));
            if (w <= 0) { asys(AIOS_SYS_EXIT, 8, 0, 0); for (;;) { } }   /* the parent will see status!=0 */
            off += (unsigned long)w;
        }
        asys(AIOS_SYS_CLOSE, fds[1], 0, 0);            /* close the write end -> the reader sees EOF */
        asys(AIOS_SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }

    /* ---- the PARENT: read to EOF, verifying every byte ---- */
    asys(AIOS_SYS_CLOSE, fds[1], 0, 0);                /* close the write end we do not use */
    unsigned long got = 0;
    for (;;) {
        long n = asys(AIOS_SYS_READ, fds[0], (long)rbuf, (long)sizeof rbuf);
        if (n == 0) break;                             /* EOF: the child closed the write end */
        if (n < 0) die("guest_pipe: FAIL -- read() error\n", 4);
        for (long i = 0; i < n; i++)
            if (rbuf[i] != pat(got + (unsigned long)i)) die("guest_pipe: FAIL -- byte mismatch\n", 5);
        got += (unsigned long)n;
    }
    asys(AIOS_SYS_CLOSE, fds[0], 0, 0);
    if (got != TOTAL) die("guest_pipe: FAIL -- short stream (lost bytes)\n", 6);

    volatile int status = -1;
    long reaped = asys(AIOS_SYS_WAIT, AIOS_WAIT_ANY, (long)&status, 0);
    if (reaped != r)        die("guest_pipe: FAIL -- wait reaped the wrong pid\n", 7);
    if (status != 0)        die("guest_pipe: FAIL -- child exited nonzero (its write failed)\n", 7);

    say("guest_pipe: OK -- 64 KiB round-tripped through the ring; reader + writer parked and woke\n");
    asys(AIOS_SYS_EXIT, 42, 0, 0);
    for (;;) { }
}
