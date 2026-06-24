/*
 * prog_spawn.c -- the shell's core loop, distilled: fork, the child EXECs the requested program,
 * the parent WAITs for it and reports the exit code. This is exactly what dash does to run one
 * command, so getting it working on the AIOS kernel is the gate to a real shell.
 *
 *   aios-uk prog_spawn ./prog_args hello world   -> runs prog_args as a child, then "exit code 0"
 *   aios-uk prog_spawn ./prog_wc /etc/hostname   -> runs wc as a child
 */
#include "libaios.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: prog_spawn <program> [args...]\n"); return 2; }

    long pid = aios_fork();
    if (pid < 0) { printf("prog_spawn: fork failed\n"); return 1; }
    if (pid == 0) {                          /* child: become the requested program */
        aios_exec(argv[1], &argv[1]);
        printf("prog_spawn: exec '%s' failed\n", argv[1]);
        aios_exit(127);
    }

    int status = 0;                          /* parent: wait for that specific child */
    long w = aios_waitpid(pid, &status, 0);
    printf("prog_spawn: child pid %d exited with code %d\n", (int)w, WEXITSTATUS(status));
    return WEXITSTATUS(status);
}
