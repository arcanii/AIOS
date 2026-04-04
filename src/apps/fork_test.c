/*
 * AIOS fork() test
 *
 * Tests basic fork: parent gets child PID, child gets 0.
 */
#include <stdio.h>
#include <sel4/sel4.h>
#include <unistd.h>
#include <sys/types.h>

int main(int argc, char *argv[]) {
    printf("fork_test: about to fork...\n");

    pid_t pid = fork();

    if (pid < 0) {
        printf("fork_test: fork() failed with %d\n", (int)pid);
        return 1;
    } else if (pid == 0) {
        /* Child */
        printf("fork_test: I am the CHILD (getpid=%d)\n", getpid());
        return 0;
    } else {
        /* Parent */
        printf("fork_test: I am the PARENT, child PID = %d\n", (int)pid);
        return 0;
    }
}
