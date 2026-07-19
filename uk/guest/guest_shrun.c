/*
 * guest_shrun.c -- a freestanding AIOS-ABI launcher: the Phase D.2 headline. It forks + execs the
 * REAL vendored dash (/bin/sh) from aiosroot.tar with `-c <cmd>` and PATH=/bin, so dash parses the
 * command, forks, resolves + execs REAL sbase tools by PATH, wires pipes (C.5) + redirections (dup2),
 * and reaps them -- an entire shell pipeline running on seL4. It runs a handful of pipelines (their
 * output lands on the console), requiring each shell to exit 0, and exits 42 iff all succeed.
 * No libc, no host headers.
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
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { asys(AIOS_SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void die(const char *s, int code) { say(s); asys(AIOS_SYS_EXIT, code, 0, 0); for (;;) { } }

/* Run "/bin/sh -c cmd" with PATH=/bin; return the child's wait status, or -1. */
static long run(const char *cmd) {
    long r = asys(AIOS_SYS_FORK, 0, 0, 0);
    if (r < 0) return -1;
    if (r == 0) {
        char *argv[] = { (char *)"sh", (char *)"-c", (char *)cmd, 0 };
        char *envp[] = { (char *)"PATH=/bin", (char *)"HOME=/", 0 };
        asys(AIOS_SYS_EXEC, (long)"/bin/sh", (long)argv, (long)envp);
        asys(AIOS_SYS_EXIT, 127, 0, 0);          /* exec failed */
        for (;;) { }
    }
    volatile int status = -1;
    long reaped = asys(AIOS_SYS_WAIT, AIOS_WAIT_ANY, (long)&status, 0);
    if (reaped != r) return -1;
    return (long)((status >> 8) & 0xff);          /* WEXITSTATUS */
}

/* Each command is SELF-VERIFYING: dash captures the pipeline output and `[ ]`-asserts it, so a
 * broken non-final pipeline stage (seq/ls) makes the shell exit nonzero -- the automated exit-42
 * gate fails, not just the human reading the serial log (the review's catch). The result is also
 * echoed so the console still shows real output. sbase wc prints %zu (no padding), so the plain
 * comparison is exact. The last command is a 3-stage / 2-pipe pipeline (exercises PIPE_MAX + park/
 * wake across three guests). */
static const char *cmds[] = {
    "echo hello from real dash on seL4",
    "r=$(cat /etc/hostname); echo \"cat /etc/hostname = $r\"; [ \"$r\" = aios ]",
    "r=$(seq 1 5 | wc -l); echo \"seq 1 5 | wc -l = $r\"; [ \"$r\" = 5 ]",
    "r=$(ls /bin | wc -l); echo \"ls /bin | wc -l = $r\"; [ \"$r\" = 31 ]",
    "r=$(grep -c root /etc/passwd); echo \"grep -c root /etc/passwd = $r\"; [ \"$r\" = 1 ]",
    "r=$(seq 1 3 | tr 0-9 a-j | tr -d '\\n'); echo \"seq|tr|tr = $r\"; [ \"$r\" = bcd ]",
    /* D.3: the fs is writable, so shell REDIRECTION to a file works -- write it, read it back
     * through a second process, append, and check the whole thing round-trips. */
    "echo written-by-dash > /tmp/sh1; r=$(cat /tmp/sh1); echo \"redirect = $r\"; [ \"$r\" = written-by-dash ]",
    "echo one > /tmp/sh2; echo two >> /tmp/sh2; r=$(wc -l < /tmp/sh2); echo \"append lines = $r\"; [ \"$r\" = 2 ]",
    "mkdir /tmp/shd && echo deep > /tmp/shd/f && r=$(cat /tmp/shd/f) && echo \"mkdir+write = $r\" && [ \"$r\" = deep ]",
};

void _start(void) {
    say("guest_shrun: launching the REAL vendored dash + sbase from aiosroot.tar ...\n");
    int n = (int)(sizeof cmds / sizeof cmds[0]);
    for (int i = 0; i < n; i++) {
        say("\n$ "); say(cmds[i]); say("\n");
        long st = run(cmds[i]);
        if (st != 0) {
            say("guest_shrun: FAIL -- the shell exited nonzero for the command above\n");
            asys(AIOS_SYS_EXIT, 1, 0, 0);
            for (;;) { }
        }
    }
    say("\nguest_shrun: OK -- real dash ran real sbase pipelines on seL4 (all exited 0)\n");
    asys(AIOS_SYS_EXIT, 42, 0, 0);
    for (;;) { }
}
