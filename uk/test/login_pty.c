/*
 * login_pty.c -- a HOST test driver for the AIOS system layer (init + login). Runs `<argv[1...]>`
 * (= ./aios-uk <root>/sbin/init, with AIOS_ROOT set by the caller) on a pty: the AIOS init starts the
 * console login; we log in (username `aios`, then the password with terminal ECHO off), reach the
 * user's shell, run a command, then `exit` (logout) -- and confirm init RESPAWNS the login (a second
 * prompt). PASS iff the session ran our command AND a second login prompt appeared (the
 * login -> shell -> logout -> login loop). Compile with -lutil (forkpty); aios-uk needs CAP_SYS_PTRACE.
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

static void ms(int m) { struct timespec t = { m / 1000, (m % 1000) * 1000000L }; nanosleep(&t, 0); }

static int count(const char *hay, const char *needle) {
    int n = 0; const char *p = hay;
    while ((p = strstr(p, needle))) { n++; p += strlen(needle); }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <cmd...>\n", argv[0]); return 2; }
    int mfd;
    pid_t pid = forkpty(&mfd, 0, 0, 0);
    if (pid < 0) { perror("forkpty"); return 2; }
    if (pid == 0) { execvp(argv[1], &argv[1]); _exit(127); }

    ms(900);                                     /* init -> login prompt */
    write(mfd, "aios\n", 5);          ms(500);   /* username */
    write(mfd, "aios\n", 5);          ms(800);   /* password (read with ECHO off) -> the user's shell */
    write(mfd, "echo LOGIN-MARKER\n", 18); ms(600);  /* the session runs a command */
    write(mfd, "echo who=$(whoami)\n", 19); ms(700); /* inc 2: the session runs AS the user (whoami) */
    write(mfd, "exit\n", 5);          ms(900);   /* logout -> init respawns login (a 2nd prompt) */

    char b[16384]; int t = 0; ssize_t n;
    struct pollfd pf = { mfd, POLLIN, 0 };
    while (t < (int)sizeof b - 1 && poll(&pf, 1, 1500) > 0) {
        n = read(mfd, b + t, sizeof b - 1 - t);
        if (n <= 0) break;
        t += n;
    }
    b[t] = '\0';

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
