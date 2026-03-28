#include "aios.h"
#include "posix.h"
AIOS_ENTRY {
    const char *args = posix_args();
    if (!args || !args[0]) {
        write(STDERR_FILENO, "usage: head <file> [n]\n", 23);
        return 1;
    }
    char filename[128];
    int i = 0, max_lines = 10;
    while (args[i] && args[i] != ' ' && i < 127) { filename[i] = args[i]; i++; }
    filename[i] = '\0';
    if (args[i] == ' ') {
        i++;
        max_lines = 0;
        while (args[i] >= '0' && args[i] <= '9') max_lines = max_lines * 10 + (args[i++] - '0');
        if (max_lines == 0) max_lines = 10;
    }
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        write(STDERR_FILENO, filename, strlen(filename));
        write(STDERR_FILENO, ": not found\n", 12);
        return 1;
    }
    int lines = 0;
    char buf[256];
    ssize_t n;
    while (lines < max_lines && (n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t j = 0; j < n && lines < max_lines; j++) {
            write(STDOUT_FILENO, &buf[j], 1);
            if (buf[j] == '\n') lines++;
        }
    }
    close(fd);
    return 0;
}
