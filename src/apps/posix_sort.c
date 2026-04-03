/* sort — sort lines of a file */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "aios_posix.h"

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    if (argc < 3) { fprintf(stderr, "usage: sort <file>\n"); return 1; }
    int fd = open(argv[2], O_RDONLY);
    if (fd < 0) { fprintf(stderr, "sort: %s: No such file\n", argv[2]); return 1; }
    char buf[4096];
    int total = 0, n;
    while ((n = read(fd, buf + total, sizeof(buf) - total - 1)) > 0) total += n;
    close(fd);
    buf[total] = '\0';

    /* Split into lines */
    char *lines[256];
    int nlines = 0;
    char *p = buf;
    while (*p && nlines < 256) {
        lines[nlines++] = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') *p++ = '\0';
    }

    /* Bubble sort */
    for (int i = 0; i < nlines - 1; i++) {
        for (int j = i + 1; j < nlines; j++) {
            char *a = lines[i], *b = lines[j];
            int k = 0;
            while (a[k] && b[k] && a[k] == b[k]) k++;
            if ((unsigned char)a[k] > (unsigned char)b[k]) {
                char *tmp = lines[i]; lines[i] = lines[j]; lines[j] = tmp;
            }
        }
    }

    for (int i = 0; i < nlines; i++) {
        if (lines[i][0]) printf("%s\n", lines[i]);
    }
    return 0;
}
