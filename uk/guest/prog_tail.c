/*
 * prog_tail.c -- a real `tail -n N` as an AIOS program (exercises fstat + lseek).
 *
 * `tail [-n N] [file]`. For a file it fstat()s the size and, if large, lseek()s to read only the
 * trailing window -- then prints the last N lines. With no file it reads stdin. Ordinary C against
 * libaios; demonstrates the new structured fstat syscall and lseek working together.
 */
#include "libaios.h"

#define CAP 8192          /* trailing window we scan for the last N lines */

int main(int argc, char **argv) {
    int n = 10;
    const char *path = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) n = atoi(argv[++i]);
        else path = argv[i];
    }

    int fd = STDIN_FILENO;
    if (path) {
        fd = (int)aios_open(path, O_RDONLY, 0);
        if (fd < 0) { printf("tail: cannot open %s\n", path); return 1; }
    }

    char buf[CAP + 1];
    long len = 0;

    struct aios_stat st;
    if (path && aios_fstat(fd, &st) == 0 && st.size > (unsigned long long)CAP) {
        aios_lseek(fd, (long)(st.size - CAP), AIOS_SEEK_SET);   /* skip to the trailing window */
        len = aios_read(fd, buf, CAP);
    } else {
        long r;                                                  /* stdin or a small file */
        while (len < CAP && (r = aios_read(fd, buf + len, CAP - len)) > 0) len += r;
    }
    if (len < 0) len = 0;

    /* Walk back from the end counting newlines to find the start of the last n lines. */
    long i = len - 1;
    if (i >= 0 && buf[i] == '\n') i--;          /* ignore a single trailing newline */
    int seen = 0;
    for (; i >= 0; i--)
        if (buf[i] == '\n' && ++seen >= n) { i++; break; }
    long start = (i < 0) ? 0 : i;

    aios_write(STDOUT_FILENO, buf + start, len - start);
    if (path) aios_close(fd);
    return 0;
}
