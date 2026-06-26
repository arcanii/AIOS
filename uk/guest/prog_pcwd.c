/*
 * prog_pcwd.c -- proves the cwd is PER-PROCESS (kernel-owned), not a single global. A child's chdir
 * must NOT leak into the parent (the bug a single shared cwd had: a subshell's `cd` moved everyone),
 * and a relative path must resolve against the calling process's own cwd. Plain C via the shadow
 * headers. Exits 0 iff all checks pass.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

int main(void) {
    int fails = 0;
    char a[256], b[256];

    if (!getcwd(a, sizeof a)) { printf("getcwd failed\n"); return 1; }
    printf("parent cwd before fork: %s\n", a);

    long pid = fork();
    if (pid == 0) {                                  /* child: chdir away, confirm it took */
        char c[256];
        if (chdir("/tmp") != 0) _exit(8);
        if (!getcwd(c, sizeof c)) _exit(8);
        printf("  child chdir(/tmp); getcwd -> %s\n", c);
        _exit(strcmp(c, "/tmp") == 0 ? 7 : 8);
    }
    int st; waitpid(pid, &st, 0);
    if (WEXITSTATUS(st) != 7) { printf("  FAIL: child chdir/getcwd wrong (exit %d)\n", WEXITSTATUS(st)); fails++; }

    if (!getcwd(b, sizeof b)) { printf("getcwd failed\n"); return 1; }
    printf("parent cwd after child exited: %s\n", b);
    if (strcmp(a, b) != 0) { printf("  FAIL: the child's chdir LEAKED into the parent (%s -> %s)\n", a, b); fails++; }
    else printf("  ok: parent cwd unchanged by the child's chdir (cwd is per-process)\n");

    /* a relative path resolves against THIS process's cwd */
    if (chdir("/tmp") == 0) {
        int fd = open("aios_pcwd_test.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { write(fd, "hi\n", 3); close(fd); }
        if (access("/tmp/aios_pcwd_test.txt", F_OK) == 0)
            printf("  ok: relative create resolved against cwd -> /tmp/aios_pcwd_test.txt\n");
        else { printf("  FAIL: relative create did not land in /tmp\n"); fails++; }
        unlink("/tmp/aios_pcwd_test.txt");
    } else { printf("  FAIL: chdir(/tmp): could not\n"); fails++; }

    printf("prog_pcwd: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
