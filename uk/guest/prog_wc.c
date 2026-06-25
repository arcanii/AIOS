/*
 * prog_wc.c -- a real `wc` (lines, words, bytes) as an AIOS program, in ordinary C against libaios.
 *
 * Reads the file named in argv[1], or stdin if none -- so host-shell redirection/pipes flow into
 * the AIOS program through the kernel's std streams (`aios-uk prog_wc < file` or `... prog_wc file`).
 * Uses only libaios (open/read/write/printf/isspace) -- never Linux. A step toward sbase.
 */
#include "libaios.h"

static void count_fd(int fd, long *lines, long *words, long *bytes) {
    char buf[1024];
    long n;
    int in_word = 0;
    while ((n = aios_read(fd, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) {
            unsigned char c = (unsigned char)buf[i];
            (*bytes)++;
            if (c == '\n') (*lines)++;
            if (isspace(c)) in_word = 0;
            else if (!in_word) { in_word = 1; (*words)++; }
        }
    }
}

int main(int argc, char **argv) {
    long lines = 0, words = 0, bytes = 0;

    if (argc >= 2) {
        int fd = aios_open(argv[1], O_RDONLY, 0);
        if (fd < 0) { printf("wc: cannot open %s\n", argv[1]); return 1; }
        count_fd(fd, &lines, &words, &bytes);
        aios_close(fd);
        printf("%d %d %d %s\n", (int)lines, (int)words, (int)bytes, argv[1]);
    } else {
        count_fd(STDIN_FILENO, &lines, &words, &bytes);
        printf("%d %d %d\n", (int)lines, (int)words, (int)bytes);
    }
    return 0;
}
