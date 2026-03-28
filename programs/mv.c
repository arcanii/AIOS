#include "aios.h"
#include "posix.h"
AIOS_ENTRY {
    const char *args = posix_args();
    if (!args || !args[0]) {
        write(STDERR_FILENO, "usage: mv <src> <dst>\n", 22);
        return 1;
    }
    char src[128], dst[128];
    int i = 0, j = 0;
    while (args[i] && args[i] != ' ' && j < 127) src[j++] = args[i++];
    src[j] = '\0';
    while (args[i] == ' ') i++;
    j = 0;
    while (args[i] && j < 127) dst[j++] = args[i++];
    dst[j] = '\0';
    if (!dst[0]) {
        write(STDERR_FILENO, "usage: mv <src> <dst>\n", 22);
        return 1;
    }
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) {
        write(STDERR_FILENO, src, strlen(src));
        write(STDERR_FILENO, ": not found\n", 12);
        return 1;
    }
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (dfd < 0) {
        close(sfd);
        write(STDERR_FILENO, "mv: cannot create ", 18);
        write(STDERR_FILENO, dst, strlen(dst));
        write(STDERR_FILENO, "\n", 1);
        return 1;
    }
    char buf[512];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) write(dfd, buf, (size_t)n);
    close(sfd);
    close(dfd);
    unlink(src);
    return 0;
}
