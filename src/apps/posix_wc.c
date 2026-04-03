/* POSIX wc — pure standard C */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "aios_posix.h"

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    if (argc < 3) { fprintf(stderr, "usage: wc <file>\n"); return 1; }
    for (int i = 2; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "wc: %s: No such file\n", argv[i]); continue; }
        char buf[4096];
        int lines = 0, words = 0, bytes = 0, in_word = 0;
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            for (int j = 0; j < n; j++) {
                bytes++;
                if (buf[j] == '\n') lines++;
                if (buf[j] == ' ' || buf[j] == '\n' || buf[j] == '\t') in_word = 0;
                else if (!in_word) { in_word = 1; words++; }
            }
        }
        close(fd);
        printf("%d %d %d %s\n", lines, words, bytes, argv[i]);
    }
    return 0;
}
