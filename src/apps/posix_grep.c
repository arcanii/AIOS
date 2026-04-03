/* grep — simple substring search */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "aios_posix.h"

static int contains(const char *line, const char *pat) {
    for (int i = 0; line[i]; i++) {
        int j = 0;
        while (pat[j] && line[i+j] == pat[j]) j++;
        if (!pat[j]) return 1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    if (argc < 4) { fprintf(stderr, "usage: grep <pattern> <file>\n"); return 1; }
    const char *pattern = argv[2];
    int fd = open(argv[3], O_RDONLY);
    if (fd < 0) { fprintf(stderr, "grep: %s: No such file\n", argv[3]); return 1; }
    char buf[4096];
    int total = 0, n;
    while ((n = read(fd, buf + total, sizeof(buf) - total - 1)) > 0) total += n;
    close(fd);
    buf[total] = '\0';

    int found = 0;
    char *p = buf;
    while (*p) {
        char *start = p;
        while (*p && *p != '\n') p++;
        char save = *p;
        *p = '\0';
        if (contains(start, pattern)) {
            printf("%s\n", start);
            found = 1;
        }
        *p = save;
        if (*p) p++;
    }
    return found ? 0 : 1;
}
