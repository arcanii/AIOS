/*
 * prog_pipeline.c -- the shell pipeline pattern, distilled: run "prog_args alpha beta | prog_wc"
 * with pipe + dup2 + fork + exec. This is precisely how dash builds `cmd1 | cmd2`:
 *
 *   pipe(fds)
 *   fork stage 1 (writer): dup2(fds[1] -> stdout), close pipe fds, exec prog_args
 *   fork stage 2 (reader): dup2(fds[0] -> stdin),  close pipe fds, exec prog_wc
 *   parent: close both pipe fds, wait for both
 *
 * prog_args writes its banner + argv to the pipe; prog_wc reads stdin (the pipe) and prints the
 * line/word/byte counts. Output is wc's count -- so a number appearing here means a real two-stage
 * pipeline ran across two exec'd AIOS programs connected by an AIOS kernel pipe.
 */
#include "libaios.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int fds[2];
    if (aios_pipe(fds) < 0) { printf("prog_pipeline: pipe failed\n"); return 1; }

    long w = aios_fork();
    if (w == 0) {                            /* stage 1: producer -> pipe write end is its stdout */
        aios_dup2(fds[1], STDOUT_FILENO);
        aios_close(fds[0]); aios_close(fds[1]);
        char *a[] = { "prog_args", "alpha", "beta", 0 };
        aios_exec("./prog_args", a);
        aios_exit(127);
    }

    long r = aios_fork();
    if (r == 0) {                            /* stage 2: consumer <- pipe read end is its stdin */
        aios_dup2(fds[0], STDIN_FILENO);
        aios_close(fds[0]); aios_close(fds[1]);
        char *a[] = { "prog_wc", 0 };
        aios_exec("./prog_wc", a);
        aios_exit(127);
    }

    aios_close(fds[0]); aios_close(fds[1]);   /* parent holds neither end; wait for both stages */
    int st = 0;
    aios_waitpid(w, &st, 0);
    aios_waitpid(r, &st, 0);
    printf("prog_pipeline: done (prog_args | prog_wc); last-stage exit %d\n", WEXITSTATUS(st));
    return WEXITSTATUS(st);
}
