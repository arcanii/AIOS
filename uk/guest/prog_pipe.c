/*
 * prog_pipe.c -- exercises AIOS_SYS_PIPE (M3d step 3: pipes).
 *
 * Parent creates a pipe and forks. The child (writer) sends a message down the write end and
 * closes it; the parent (reader) drains the read end to stdout until EOF, then reaps the child.
 * This proves: pipe fds are shared + refcounted across fork; the reader BLOCKS (parks) on the
 * empty pipe and is woken when the child writes; and read returns 0 (EOF) once the last write end
 * closes. No dup2/exec -- just the raw pipe + the park/wake machinery.
 */
#include "libaios.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int fds[2];
    if (aios_pipe(fds) < 0) { printf("prog_pipe: pipe failed\n"); return 1; }

    long pid = aios_fork();
    if (pid < 0) { printf("prog_pipe: fork failed\n"); return 1; }
    if (pid == 0) {                          /* child: the writer */
        aios_close(fds[0]);                  /* drop the read end */
        const char *msg = "hello down an AIOS pipe, child -> parent\n";
        aios_write(fds[1], msg, strlen(msg));
        aios_close(fds[1]);                  /* closing the last write end -> reader sees EOF */
        aios_exit(0);
    }

    aios_close(fds[1]);                       /* parent: drop the write end, then drain */
    char buf[64];
    long n, total = 0;
    while ((n = aios_read(fds[0], buf, sizeof buf)) > 0) {
        aios_write(STDOUT_FILENO, buf, n);
        total += n;
    }
    aios_close(fds[0]);

    int status = 0;
    aios_waitpid(pid, &status, 0);
    printf("prog_pipe: read %d bytes from the pipe, child exit %d\n", (int)total, WEXITSTATUS(status));
    return (total > 0) ? 0 : 2;
}
