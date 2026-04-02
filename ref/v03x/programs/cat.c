#include "aios.h"
#include "posix.h"
AIOS_ENTRY {
    const char *filename = posix_args();
    if (!filename || !filename[0]) {
        sys->puts_direct("usage: cat <file>\n");
        return 1;
    }

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        sys->puts_direct(filename);
        sys->puts_direct(": not found\n");
        return 1;
    }

    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, (size_t)n);
    }
    close(fd);
    return 0;
}
