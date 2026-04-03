/* POSIX cat — pure standard C */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "aios_posix.h"

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    if (argc < 3) {
        fprintf(stderr, "usage: cat <file> [file...]\n");
        return 1;
    }
    char buf[4096];
    for (int i = 2; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "cat: %s: No such file\n", argv[i]); continue; }
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        close(fd);
    }
    return 0;
}
