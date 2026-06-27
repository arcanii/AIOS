/*
 * ctrlz_pty.c -- a HOST test driver proving ^Z suspends a job and `fg` resumes it (dash JOBS=1).
 *
 * Runs `<argv[1...]>` (= ./aios-uk ./dash) on a pty, starts a foreground job that loops forever
 * (`./prog_loop`), sends ^Z (0x1a): the kernel forwards SIGTSTP to the foreground process group, the
 * job STOPS, and dash's wait(WUNTRACED) returns it -- dash prints a "Stopped" notice and returns to
 * the prompt. We then run a command (proving the prompt is back), `fg` to resume the job, ^C to kill
 * it, and a final command. PASS iff both markers printed: dash survived ^Z (suspend) and fg/^C
 * (resume + kill). Compile with -lutil (forkpty); needs CAP_SYS_PTRACE for aios-uk.
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
    write(mfd, "./prog_loop\n", 12);        ms(700);   /* a foreground job that loops forever */
    write(mfd, "\032", 1);                  ms(700);   /* ^Z -> SIGTSTP to the fg group: suspends the job */
    write(mfd, "echo SUSPENDED\n", 15);     ms(500);   /* dash is back at the prompt -> runs this */
    write(mfd, "fg\n", 3);                  ms(700);   /* resume prog_loop in the foreground */
    write(mfd, "\003", 1);                  ms(600);   /* ^C -> kill the resumed job */
    write(mfd, "echo RESUMED-DONE\n", 18);  ms(500);
    write(mfd, "exit\n", 5);                ms(500);

    char b[8192]; int t = 0; ssize_t n;
    struct pollfd pf = { mfd, POLLIN, 0 };
    while (t < (int)sizeof b - 1 && poll(&pf, 1, 1500) > 0) {
        n = read(mfd, b + t, sizeof b - 1 - t);
        if (n <= 0) break;
        t += n;
    }
    b[t] = '\0';

    if (getenv("CZ_DEBUG")) fprintf(stderr, "==== PTY OUTPUT (%d bytes) ====\n%s\n==== END ====\n", t, b);
    int ok = strstr(b, "SUSPENDED") && strstr(b, "RESUMED-DONE");
    printf("    ^Z suspended the job, fg resumed it, dash survived: %s\n", ok ? "yes" : "NO");
    printf("  ctrlz_pty: %s\n", ok ? "PASS" : "FAIL");
    waitpid(pid, 0, 0);
    return ok ? 0 : 1;
}
