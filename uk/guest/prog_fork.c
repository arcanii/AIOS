/*
 * prog_fork.c -- exercises AIOS_SYS_FORK / EXIT / WAIT (M3d step 2: the multi-process kernel).
 *
 * The parent forks N children; each child proves fork returned 0 in the child (its own copy of the
 * address space) and exits with a distinct code. The parent reaps them all with wait() (any order)
 * and checks the exit codes. 1+2+3 = 6 is the pass condition -- a single number that is only right
 * if fork created real independent children, each exited with its own code, and wait delivered each
 * status to the (parked, then woken) parent.
 */
#include "libaios.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const int N = 3;
    printf("prog_fork: parent forking %d children\n", N);

    for (int i = 0; i < N; i++) {
        long pid = aios_fork();
        if (pid < 0) { printf("prog_fork: fork failed\n"); return 1; }
        if (pid == 0) {                      /* child i */
            printf("  child %d: fork returned 0, exiting with code %d\n", i, i + 1);
            aios_exit(i + 1);
        }
        printf("prog_fork: forked child %d -> pid %d\n", i, (int)pid);
    }

    int reaped = 0, sum = 0;
    for (int i = 0; i < N; i++) {
        int status = 0;
        long pid = aios_wait(&status);
        if (pid < 0) break;
        printf("prog_fork: reaped pid %d, exit code %d\n", (int)pid, WEXITSTATUS(status));
        reaped++;
        sum += WEXITSTATUS(status);
    }
    printf("prog_fork: reaped %d children, exit-code sum %d (expect 6)\n", reaped, sum);
    return (reaped == N && sum == 6) ? 0 : 2;
}
