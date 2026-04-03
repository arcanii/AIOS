/* POSIX ps — show process status */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "aios_posix.h"

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);
    int fd = open("/proc/status", O_RDONLY);
    if (fd < 0) { fprintf(stderr, "ps: cannot read /proc/status\n"); return 1; }
    char buf[4096];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
    return 0;
}
