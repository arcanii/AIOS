/* prog_sigpipe.c -- a gate for SIGPIPE on a broken pipe. POSIX: writing to a pipe whose read ends are
 * all closed generates SIGPIPE -- which by default TERMINATES the writer (so `producer | head` dies
 * quietly when head exits early), or, if SIGPIPE is ignored/caught, makes the write return -1/EPIPE.
 * Exit 0 iff both cases hold. No pty needed. */

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
    int fail = 0;

    /* Case 1: DEFAULT disposition -- the writer is TERMINATED by SIGPIPE when the readers close. */
    int pfd[2];
    if (pipe(pfd) != 0) { printf("FAIL pipe\n"); return 1; }
    int kid = fork();
    if (kid == 0) {
        close(pfd[0]);                          /* child keeps only the write end */
        char buf[4096];
        memset(buf, 'x', sizeof buf);
        for (int i = 0; i < 100000; i++)        /* far more than the pipe holds -> we park, then the */
            write(pfd[1], buf, sizeof buf);     /* parent's close makes the next write raise SIGPIPE */
        _exit(0);                               /* reached only if SIGPIPE never fired (a FAIL) */
    }
    close(pfd[1]);                              /* parent keeps only the read end */
    char b[64];
    read(pfd[0], b, sizeof b);                  /* drain a little so the child is mid-stream */
    close(pfd[0]);                              /* no readers left -> the child's write gets SIGPIPE */
    int st = 0;
    waitpid(kid, &st, 0);
    if (!(WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE)) {   /* killed by SIGPIPE -> WIFSIGNALED (POSIX) */
        printf("FAIL default: writer status 0x%x (want WIFSIGNALED, WTERMSIG %d = SIGPIPE)\n", st, SIGPIPE);
        fail = 1;
    } else {
        printf("default SIGPIPE -> writer terminated (signal %d) when the reader closed\n", WTERMSIG(st));
    }

    /* Case 2: SIGPIPE IGNORED -- the write returns -1/EPIPE instead of terminating. */
    signal(SIGPIPE, SIG_IGN);
    int qfd[2];
    if (pipe(qfd) != 0) { printf("FAIL pipe2\n"); return 1; }
    close(qfd[0]);                              /* no readers at all */
    errno = 0;
    long n = write(qfd[1], "hi", 2);
    if (n != -1 || errno != EPIPE) {
        printf("FAIL ignored: write returned %ld errno %d (want -1, EPIPE)\n", n, errno);
        fail = 1;
    } else {
        printf("SIGPIPE ignored -> write to a no-reader pipe returns -1 EPIPE\n");
    }

    printf("sigpipe: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
