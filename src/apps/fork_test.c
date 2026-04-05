#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

int main(int argc, char *argv[]) {
    printf("fork_test: about to fork (my pid=%d)...\n", getpid());

    pid_t pid = fork();

    if (pid < 0) {
        printf("fork_test: fork() FAILED: %d\n", (int)pid);
        return 1;
    } else if (pid == 0) {
        printf("fork_test: CHILD here (pid=%d)\n", getpid());
        return 42;
    } else {
        int status = 0;
        pid_t w = waitpid(pid, &status, 0);
        printf("fork_test: PARENT (pid=%d), child=%d exited with %d\n",
               getpid(), (int)w, WEXITSTATUS(status));
        return 0;
    }
}
