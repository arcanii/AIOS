/*
 * ctrlc_pty.c -- a HOST test driver (not an AIOS guest) that proves interactive dash + ^C work.
 *
 * It runs `<argv[1...]>` (= ./aios-uk ./dash) on a pseudo-terminal so dash sees a real tty
 * (isatty(0) is true -> interactive mode, a prompt), types a command, sends ^C (0x03) while dash is
 * at the prompt, then types another command and exits. PASS iff dash printed BOTH command outputs --
 * i.e. it SURVIVED ^C (its SIGINT handler ran, the line was aborted, the prompt returned) instead of
 * dying. Compile on the host with -lutil (forkpty); needs CAP_SYS_PTRACE for aios-uk.
 */
#define _GNU_SOURCE
#include <pty.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

static void ms(int m) { struct timespec t = { m / 1000, (m % 1000) * 1000000L }; nanosleep(&t, 0); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <cmd...>\n", argv[0]); return 2; }
    int mfd;
    pid_t pid = forkpty(&mfd, 0, 0, 0);
    if (pid < 0) { perror("forkpty"); return 2; }
    if (pid == 0) { execvp(argv[1], &argv[1]); _exit(127); }

    ms(700);                                   /* let dash reach its prompt */
    write(mfd, "echo BEFORE\n", 12);    ms(500);
    write(mfd, "\003", 1);              ms(500);   /* ^C at the prompt -> SIGINT to the fg pgrp */
    write(mfd, "echo AFTER-CTRLC\n", 17); ms(500);
    write(mfd, "exit\n", 5);            ms(500);

    char b[8192]; int t = 0; ssize_t n;
    while ((n = read(mfd, b + t, sizeof b - 1 - t)) > 0) { t += n; if (t >= (int)sizeof b - 1) break; }
    if (t < 0) t = 0;
    b[t] = '\0';

    int ok = strstr(b, "BEFORE") && strstr(b, "AFTER-CTRLC");
    printf("    dash survived ^C and ran the next command: %s\n", ok ? "yes" : "NO");
    printf("  ctrlc_pty: %s\n", ok ? "PASS" : "FAIL");
    waitpid(pid, 0, 0);
    return ok ? 0 : 1;
}
