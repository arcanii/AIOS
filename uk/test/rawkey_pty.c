/*
 * rawkey_pty.c -- a HOST test driver proving RAW terminal mode (tcsetattr) works over a pty.
 *
 * Runs `<argv[1...]>` (= ./aios-uk ./prog_rawkey) on a pty. prog_rawkey switches stdin to raw mode
 * and reads ONE byte. We send a single byte WITH NO NEWLINE: in raw mode the read completes at once
 * (and the byte is not echoed), so "rawkey got: Z" appears; in canonical mode the read would block
 * waiting for Enter and this would time out. PASS iff the marker is seen. Compile with -lutil.
 */
#define _GNU_SOURCE
#include <pty.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <poll.h>

static void ms(int m) { struct timespec t = { m / 1000, (m % 1000) * 1000000L }; nanosleep(&t, 0); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <cmd...>\n", argv[0]); return 2; }
    int mfd;
    pid_t pid = forkpty(&mfd, 0, 0, 0);
    if (pid < 0) { perror("forkpty"); return 2; }
    if (pid == 0) { execvp(argv[1], &argv[1]); _exit(127); }

    ms(800);                          /* let prog_rawkey set raw mode and block in read(1) */
    write(mfd, "Z", 1);               /* a single byte, NO newline -- only raw mode returns from this */

    char b[4096]; int t = 0; ssize_t n;
    struct pollfd pf = { mfd, POLLIN, 0 };
    while (t < (int)sizeof b - 1 && poll(&pf, 1, 1500) > 0) {
        n = read(mfd, b + t, sizeof b - 1 - t);
        if (n <= 0) break;
        t += n;
    }
    b[t] = '\0';

    if (getenv("RK_DEBUG")) fprintf(stderr, "==== PTY OUTPUT (%d bytes) ====\n%s\n==== END ====\n", t, b);
    int ok = strstr(b, "rawkey got: Z") != 0;
    printf("    raw mode read one keypress (no Enter, no echo): %s\n", ok ? "yes" : "NO");
    printf("  rawkey_pty: %s\n", ok ? "PASS" : "FAIL");
    waitpid(pid, 0, 0);
    return ok ? 0 : 1;
}
