#include "aios.h"
#include <posix.h>

static void print(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }
static void print_num(long n) {
    char buf[20]; int i = 0;
    if (n == 0) { write(STDOUT_FILENO, "0", 1); return; }
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (--i >= 0) write(STDOUT_FILENO, &buf[i], 1);
}

AIOS_ENTRY {
    int pass = 0, total = 0;

    /* TIOCGWINSZ */
    total++;
    struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
    struct winsize ws = {0, 0, 0, 0};
    if (ioctl(STDOUT_FILENO, 0x5413, &ws) == 0 && ws.ws_row == 24 && ws.ws_col == 80) {
        pass++; print("PASS");
    } else print("FAIL");
    print(": TIOCGWINSZ row=");
    print_num(ws.ws_row);
    print(" col=");
    print_num(ws.ws_col);
    print("\n");

    /* FIONREAD */
    total++;
    int avail = -1;
    if (ioctl(STDIN_FILENO, 0x541B, &avail) == 0 && avail == 0) {
        pass++; print("PASS");
    } else print("FAIL");
    print(": FIONREAD\n");

    /* Unknown ioctl */
    total++;
    if (ioctl(STDOUT_FILENO, 0x9999) == -1) {
        pass++; print("PASS");
    } else print("FAIL");
    print(": unknown ioctl returns -1\n");

    if (pass == total) print("ALL PASS\n");
    return 0;
}
