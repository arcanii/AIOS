/*
 * guest_fork.c -- a freestanding AIOS-ABI program: the Phase C.2 fork test. The parent mmaps a page
 * and stamps a pattern, then forks. The CHILD must see the parent's pre-fork state (the data global,
 * a stack local, the mmap'd pattern -- proving the eager VSpace copy carried all three regions), then
 * MUTATES its copies and exits 7. The PARENT wait()s (proving the parked-parent wake path: its saved
 * reply cap must survive the child's intervening faults), checks the reaped pid + status, and then
 * verifies its OWN copies are UNCHANGED (isolation: the child wrote to different frames). Exit 42 iff
 * every check passes. No libc, no host headers.
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

#define PAT_PARENT 0xA11CE5EDBEEF0001UL
#define PAT_CHILD  0xC0DEC0DE0000FFFFUL

static volatile long g_data = 111;             /* .data -- the writable-ELF-segment copy check. The
                                                * parent re-stamps it to a SENTINEL != the ELF
                                                * initializer before forking: the child's fresh ELF
                                                * preload already holds 111, so only a real copy of
                                                * the parent's MUTATED page can show the sentinel
                                                * (review catch: 111 alone proves nothing). */
#define DATA_SENTINEL 0xD00DL

void _start(void) {
    volatile long lstack = 77;                 /* the stack copy check (volatile: keep it in memory) */

    long ppid = asys(AIOS_SYS_GETPID, 0, 0, 0);
    long addr = asys(AIOS_SYS_MMAP, 4096, 0, 0);
    if (addr == 0) die("guest_fork: FAIL -- pre-fork mmap returned 0\n", 2);
    volatile unsigned long *m = (volatile unsigned long *)(unsigned long)addr;
    m[0] = PAT_PARENT;
    g_data = DATA_SENTINEL;                    /* mutate .data pre-fork so the copy is provable */
    say("guest_fork: parent mmap'd a page + stamped it; forking ...\n");

    long r = asys(AIOS_SYS_FORK, 0, 0, 0);
    if (r < 0) die("guest_fork: FAIL -- fork returned an error\n", 3);

    if (r == 0) {                              /* ---- the CHILD ---- */
        int ok = 1;
        if (g_data != DATA_SENTINEL) { say("guest_fork: child FAIL -- data global not copied\n"); ok = 0; }
        if (lstack != 77)           { say("guest_fork: child FAIL -- stack local not copied\n");  ok = 0; }
        if (m[0] != PAT_PARENT)     { say("guest_fork: child FAIL -- mmap pattern not copied\n"); ok = 0; }
        long cpid = asys(AIOS_SYS_GETPID, 0, 0, 0);
        if (cpid == ppid || cpid <= 0) { say("guest_fork: child FAIL -- bad getpid\n");           ok = 0; }
        g_data = 222; lstack = 88; m[0] = PAT_CHILD;   /* mutate OUR copies; the parent must not see this */
        say(ok ? "guest_fork: child OK -- saw the pre-fork state; mutated its copies; exiting 7\n"
               : "guest_fork: child saw a broken copy; exiting 1\n");
        asys(AIOS_SYS_EXIT, ok ? 7 : 1, 0, 0);
        for (;;) { }
    }

    /* ---- the PARENT ---- (straight to wait: keeps the console interleaving deterministic) */
    volatile int status = -1;
    long reaped = asys(AIOS_SYS_WAIT, AIOS_WAIT_ANY, (long)&status, 0);
    if (reaped != r)            die("guest_fork: FAIL -- wait reaped the wrong pid\n", 4);
    if (status != (7 << 8))     die("guest_fork: FAIL -- wrong child exit status\n", 5);
    if (g_data != DATA_SENTINEL) die("guest_fork: FAIL -- child mutation leaked into the parent's data\n", 6);
    if (lstack != 77)           die("guest_fork: FAIL -- child mutation leaked into the parent's stack\n", 6);
    if (m[0] != PAT_PARENT)     die("guest_fork: FAIL -- child mutation leaked into the parent's mmap\n", 6);
    say("guest_fork: parent OK -- reaped the child (status 7), all three regions isolated\n");
    asys(AIOS_SYS_EXIT, 42, 0, 0);
    for (;;) { }
}
