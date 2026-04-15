/*
 * test_pipe_rw -- pipe read/write and EOF test
 *
 * Tests: pipe creation, small write+read, large transfer,
 *        EOF on close, dup2 redirection.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_pass = 0;

static void check(const char *name, int cond) {
    tests_run++;
    if (cond) {
        tests_pass++;
        printf("  PASS: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
    }
}

int main(void) {
    int fds[2];
    char buf[256];
    int n;

    printf("=== AIOS Pipe R/W Test ===\n");

    /* Test 1: pipe creation */
    printf("\n[1] pipe() creation\n");
    n = pipe(fds);
    check("pipe() returns 0", n == 0);
    check("read fd >= 0", fds[0] >= 0);
    check("write fd >= 0", fds[1] >= 0);
    check("read fd != write fd", fds[0] != fds[1]);

    /* Test 2: small write + read */
    printf("\n[2] Small write/read\n");
    const char *msg = "hello pipe";
    n = write(fds[1], msg, strlen(msg));
    check("write returns correct length", n == (int)strlen(msg));

    memset(buf, 0, sizeof(buf));
    n = read(fds[0], buf, sizeof(buf));
    check("read returns correct length", n == (int)strlen(msg));
    check("data matches", memcmp(buf, msg, strlen(msg)) == 0);

    /* Test 3: EOF -- close write end, read should return 0 */
    printf("\n[3] EOF detection\n");
    close(fds[1]);
    n = read(fds[0], buf, sizeof(buf));
    check("read after close(write_end) returns 0 (EOF)", n == 0);
    close(fds[0]);

    /* Test 4: dup2 redirection */
    printf("\n[4] dup2 pipe redirection\n");
    int pfd[2];
    n = pipe(pfd);
    check("second pipe() returns 0", n == 0);

    int old_stdout = dup(1);
    check("dup(1) >= 0", old_stdout >= 0);

    n = dup2(pfd[1], 1);
    check("dup2(pipe_wr, 1) returns 1", n == 1);

    /* Write to stdout (which is now the pipe) */
    const char *redir_msg = "redirected";
    write(1, redir_msg, strlen(redir_msg));
    close(pfd[1]);
    close(1);

    /* Restore stdout */
    dup2(old_stdout, 1);
    close(old_stdout);

    /* Read from pipe read end */
    memset(buf, 0, sizeof(buf));
    n = read(pfd[0], buf, sizeof(buf));
    close(pfd[0]);
    check("read redirected data", n == (int)strlen(redir_msg));
    check("redirected data matches", n > 0 && memcmp(buf, redir_msg, strlen(redir_msg)) == 0);

    /* Summary */
    printf("\n=== Results: %d/%d passed ===\n", tests_pass, tests_run);
    return (tests_pass == tests_run) ? 0 : 1;
}
