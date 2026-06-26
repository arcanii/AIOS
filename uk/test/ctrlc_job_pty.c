/*
 * ctrlc_job_pty.c -- a HOST test driver proving ^C kills a FOREGROUND JOB and the shell survives.
 *
 * The harder half of interactive ^C (ctrlc_pty covers ^C at the prompt): it runs `<argv[1...]>` (=
 * ./aios-uk ./dash) on a pty, starts a foreground job that loops forever (`./prog_loop`), then sends
 * ^C. The kernel must forward SIGINT to the foreground process group -- terminating the looping job
 * (RUNNING) AND interrupting dash's wait (PARKED) so its handler returns it to the prompt -- then run
 * a following command. PASS iff dash printed the AFTER marker, i.e. ^C killed the job and the shell
 * lived. Compile with -lutil (forkpty); needs CAP_SYS_PTRACE for aios-uk.
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

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <cmd...>\n", argv[0]); return 2; }
    int mfd;
    pid_t pid = forkpty(&mfd, 0, 0, 0);
    if (pid < 0) { perror("forkpty"); return 2; }
    if (pid == 0) { execvp(argv[1], &argv[1]); _exit(127); }

    ms(700);                                       /* let dash reach its prompt */
    write(mfd, "echo BEFORE\n", 12);        ms(400);
    write(mfd, "./prog_loop\n", 12);        ms(700);   /* a foreground job that loops forever */
    write(mfd, "\003", 1);                  ms(600);   /* ^C -> SIGINT to the fg group: kills the job */
    write(mfd, "echo AFTER-JOB\n", 15);     ms(500);
    write(mfd, "exit\n", 5);                ms(500);

    char b[8192]; int t = 0; ssize_t n;
    struct pollfd pf = { mfd, POLLIN, 0 };
    while (t < (int)sizeof b - 1 && poll(&pf, 1, 1500) > 0) {   /* read until 1.5s of silence (or EOF) */
        n = read(mfd, b + t, sizeof b - 1 - t);
        if (n <= 0) break;
        t += n;
    }
    b[t] = '\0';

    if (getenv("CJ_DEBUG")) fprintf(stderr, "==== PTY OUTPUT (%d bytes) ====\n%s\n==== END ====\n", t, b);
    int ok = strstr(b, "BEFORE") && strstr(b, "AFTER-JOB");
    printf("    ^C killed the foreground job and dash survived: %s\n", ok ? "yes" : "NO");
    printf("  ctrlc_job_pty: %s\n", ok ? "PASS" : "FAIL");
    waitpid(pid, 0, 0);
    return ok ? 0 : 1;
}
