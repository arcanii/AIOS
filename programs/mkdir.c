#include "aios.h"
#include "posix.h"

AIOS_ENTRY {
    const char *dirname = posix_args();
    if (!dirname || !dirname[0]) {
        write(STDERR_FILENO, "usage: mkdir <dir>\n", 19);
        return 1;
    }
    if (mkdir(dirname, 0755) < 0) {
        write(STDERR_FILENO, "mkdir: failed to create ", 24);
        write(STDERR_FILENO, dirname, strlen(dirname));
        write(STDERR_FILENO, "\n", 1);
        return 1;
    }
    return 0;
}
