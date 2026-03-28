#include "aios.h"
#include "posix.h"

AIOS_ENTRY {
    const char *dirname = posix_args();
    if (!dirname || !dirname[0]) {
        write(STDERR_FILENO, "usage: rmdir <dir>\n", 19);
        return 1;
    }
    if (rmdir(dirname) < 0) {
        write(STDERR_FILENO, "rmdir: failed to remove ", 24);
        write(STDERR_FILENO, dirname, strlen(dirname));
        write(STDERR_FILENO, "\n", 1);
        return 1;
    }
    return 0;
}
