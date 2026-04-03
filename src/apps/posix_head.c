/* POSIX head — pure standard C */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "aios_posix.h"

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    if (argc < 3) { fprintf(stderr, "usage: head <file>\n"); return 1; }
    int fd = open(argv[2], O_RDONLY);
    if (fd < 0) { fprintf(stderr, "head: %s: No such file\n", argv[2]); return 1; }
    char buf[4096];
    int lines = 0;
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0 && lines < 10) {
        for (int j = 0; j < n && lines < 10; j++) {
            write(1, &buf[j], 1);
            if (buf[j] == '\n') lines++;
        }
    }
    close(fd);
    return 0;
}
