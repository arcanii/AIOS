/*
 * login_pty.c -- a HOST test driver for the AIOS system layer (init + login). Runs `<argv[1...]>`
 * (= ./aios-uk <root>/sbin/init, with AIOS_ROOT set by the caller) on a pty: the AIOS init starts the
 * console login; we log in (username `aios`, then the password with terminal ECHO off), reach the
 * user's shell, run a command, then `exit` (logout) -- and confirm init RESPAWNS the login (a second
 * prompt). PASS iff the session ran our command AND the session runs AS the user (whoami == aios) AND a
 * second login prompt appeared. Compile with -lutil (forkpty); aios-uk needs CAP_SYS_PTRACE.
 *
 * EXPECT-STYLE (robust under load): rather than write input on fixed sleeps and drain once at the end
 * (which raced the shell -- the `who=aios` output, produced by a `$(whoami)` command substitution, could
 * arrive after logout or after the final drain ended), the driver continuously accumulates the pty
 * output and WAITS for each expected token before proceeding: it waits for `login:` before sending the
 * username, `Password:` before the password, and -- the key fix -- the `who=aios` OUTPUT (which appears
 * only in the command's output, never in the typed `echo who=$(whoami)` line) before sending `exit`. So
 * logout can no longer race the whoami output. Deadlines are generous but only elapse on real failure
 * (on success each wait returns the instant its token appears).
 */
#define _GNU_SOURCE
#include <pty.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <poll.h>

static char b[32768];       /* all pty output, accumulated across the whole session */
static int  t = 0;
static int  mfd;

static long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int count(const char *hay, const char *needle) {
    int n = 0; const char *p = hay;
    while ((p = strstr(p, needle))) { n++; p += strlen(needle); }
    return n;
}
/* Drain pty output into b until `needle` has appeared at least `mincount` times, or `budget_ms`
 * elapses. Returns 1 iff the target count was reached. Accumulates continuously so nothing is lost. */
static int wait_for(const char *needle, int mincount, int budget_ms) {
    long deadline = now_ms() + budget_ms;
    for (;;) {
        if (count(b, needle) >= mincount) return 1;
        long rem = deadline - now_ms();
        if (rem <= 0) return count(b, needle) >= mincount;
        struct pollfd pf = { mfd, POLLIN, 0 };
        int pr = poll(&pf, 1, (int)(rem < 200 ? rem : 200));
        if (pr > 0) {
            ssize_t n = read(mfd, b + t, sizeof b - 1 - t);
            if (n <= 0) return count(b, needle) >= mincount;   /* EOF/err */
            t += n; b[t] = '\0';
            if (t >= (int)sizeof b - 1) return count(b, needle) >= mincount;
        }
    }
}
static void send(const char *s) { (void)!write(mfd, s, strlen(s)); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <cmd...>\n", argv[0]); return 2; }
    pid_t pid = forkpty(&mfd, 0, 0, 0);
    if (pid < 0) { perror("forkpty"); return 2; }
    if (pid == 0) { execvp(argv[1], &argv[1]); _exit(127); }

    /* Deadlines are CEILINGS -- each wait returns the instant its token appears, so a healthy session
     * finishes in a second or two; the generous budgets only ever elapse on a REAL failure and keep a
     * slow-but-valid step (aios-uk under heavy load does fork+exec+ptrace per command) from being cut
     * off. Their sum (~47s) stays under the gate's per-test `timeout` (60s in gate.sh), so a squeezed
     * late step cannot masquerade as a failure. */
    wait_for("login:", 1, 12000);    send("aios\n");      /* username at the login prompt              */
    wait_for("Password:", 1, 8000);  send("aios\n");      /* password (read with ECHO off)             */
    send("echo LOGIN-MARKER\n");                          /* the session runs a command                */
    send("echo who=$(whoami)\n");                         /* inc 2: the session runs AS the user       */
    wait_for("who=aios", 1, 15000);  send("exit\n");      /* WAIT for the whoami output, THEN log out  */
    wait_for("login:", 2, 12000);                         /* logout -> init respawns a fresh login     */

    if (getenv("LP_DEBUG")) fprintf(stderr, "==== PTY OUTPUT (%d bytes) ====\n%s\n==== END ====\n", t, b);
    int ran     = strstr(b, "LOGIN-MARKER") != NULL;   /* the authenticated session ran our command */
    int asuser  = strstr(b, "who=aios") != NULL;       /* inc 2: the session runs AS the user (whoami) */
    int respawn = count(b, "login:") >= 2;             /* logout -> init started a fresh login */
    int ok = ran && asuser && respawn;
    printf("    logged in + ran a command: %s ; session is the user (whoami=aios): %s ; logout respawned the login: %s\n",
           ran ? "yes" : "NO", asuser ? "yes" : "NO", respawn ? "yes" : "NO");
    printf("  login_pty: %s\n", ok ? "PASS" : "FAIL");
    close(mfd);
    waitpid(pid, 0, 0);
    return ok ? 0 : 1;
}
