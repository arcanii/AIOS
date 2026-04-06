/* pipe_test.c -- pipe diagnostic tool (v2, pure POSIX)
 *
 * Tests pipe mechanism through POSIX APIs only.
 * No seL4 references -- this is a PosixApp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char *argv[]) {
    printf("=== AIOS Pipe Diagnostic v2 ===\n");

    /* Test 1: single-process pipe */
    printf("\nTest 1: single-process pipe...\n");
    {
        int fds[2];
        if (pipe(fds) < 0) { printf("  FAIL: pipe()\n"); return 1; }
        printf("  pipe fds: read=%d write=%d\n", fds[0], fds[1]);
        write(fds[1], "ok", 2);
        close(fds[1]);
        char buf[16] = {0};
        int r = read(fds[0], buf, 15);
        close(fds[0]);
        printf("  read %d: [%s] %s\n", r, buf, r == 2 ? "PASS" : "FAIL");
    }

    /* Test 2: fork + pipe (no exec) */
    printf("\nTest 2: fork + pipe (no exec)...\n");
    {
        int fds[2];
        pipe(fds);
        pid_t p = fork();
        if (p == 0) {
            close(fds[0]);
            write(fds[1], "child!", 6);
            close(fds[1]);
            _exit(0);
        }
        close(fds[1]);
        char buf[32] = {0};
        int r = read(fds[0], buf, 31);
        printf("  read %d: [%s]\n", r, buf);
        close(fds[0]);
        waitpid(p, 0, 0);
        printf("  %s\n", r == 6 ? "PASS" : "FAIL");
    }

    /* Test 3: fork+exec with dup2 redirect (the real pipeline path) */
    printf("\nTest 3: fork+exec+dup2 (echo through pipe)...\n");
    {
        int fds[2];
        pipe(fds);
        printf("  pipe fds: read=%d write=%d\n", fds[0], fds[1]);

        pid_t p = fork();
        if (p < 0) {
            printf("  FAIL: fork() returned %d\n", (int)p);
        } else if (p == 0) {
            /* Child: redirect stdout to pipe write end */
            close(fds[0]);
            dup2(fds[1], 1);
            close(fds[1]);
            char *av[] = {"echo", "pipetest", NULL};
            execv("/bin/echo", av);
            _exit(127);
        } else {
            close(fds[1]);
            printf("  child pid=%d, reading...\n", (int)p);
            char buf[256] = {0};
            int total = 0;
            while (total < 255) {
                int r = read(fds[0], buf + total, 255 - total);
                printf("  read() returned %d\n", r);
                if (r <= 0) break;
                total += r;
            }
            buf[total] = 0;
            printf("  total=%d: [%s]\n", total, buf);
            close(fds[0]);
            int st;
            waitpid(p, &st, 0);
            printf("  child exit=%d\n", WEXITSTATUS(st));
            printf("  %s\n", total > 0 ? "PASS" : "FAIL");
        }
    }

    /* Test 4: two-stage pipeline (writer child + reader child) */
    printf("\nTest 4: two-child pipeline...\n");
    {
        int fds[2];
        pipe(fds);
        printf("  pipe fds: read=%d write=%d\n", fds[0], fds[1]);

        pid_t w = fork();
        if (w < 0) {
            printf("  FAIL: fork writer\n");
        } else if (w == 0) {
            close(fds[0]);
            write(fds[1], "hello from writer\n", 18);
            close(fds[1]);
            _exit(0);
        }

        pid_t r = fork();
        if (r < 0) {
            printf("  FAIL: fork reader (w=%d)\n", (int)w);
        } else if (r == 0) {
            close(fds[1]);
            char buf[64] = {0};
            int n = read(fds[0], buf, 63);
            close(fds[0]);
            printf("  reader got %d: [%s]\n", n, buf);
            _exit(n > 0 ? 0 : 1);
        }

        close(fds[0]);
        close(fds[1]);
        int ws, rs;
        waitpid(w, &ws, 0);
        waitpid(r, &rs, 0);
        printf("  writer exit=%d, reader exit=%d\n",
               WEXITSTATUS(ws), WEXITSTATUS(rs));
        printf("  %s\n", WEXITSTATUS(rs) == 0 ? "PASS" : "FAIL");
    }

    printf("\n=== Done ===\n");
    return 0;
}
