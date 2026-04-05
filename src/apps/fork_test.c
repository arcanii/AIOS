#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

int main(int argc, char *argv[]) {
    printf("fork_test: about to fork+exec (pid=%d)...\n", getpid());

    pid_t pid = fork();
    if (pid < 0) {
        printf("fork_test: fork FAILED\n");
        return 1;
    } else if (pid == 0) {
        /* Child — exec sysinfo (it prints and exits) */
        execl("/bin/sysinfo", "sysinfo", NULL);
        printf("fork_test: exec FAILED\n");
        return 1;
    } else {
        int status = 0;
        pid_t w = waitpid(pid, &status, 0);
        printf("fork_test: child=%d exited with %d\n", (int)w, WEXITSTATUS(status));
        return 0;
    }
}
