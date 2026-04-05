/*
 * AIOS fork() test
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>

int main(int argc, char *argv[]) {
    printf("fork_test: about to fork (my pid=%d)...\n", getpid());

    pid_t pid = fork();

    if (pid < 0) {
        printf("fork_test: fork() FAILED: %d\n", (int)pid);
    } else if (pid == 0) {
        printf("fork_test: CHILD here (pid=%d)\n", getpid());
    } else {
        printf("fork_test: PARENT here (pid=%d), child=%d\n", getpid(), (int)pid);
    }

    return 0;
}
