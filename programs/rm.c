#include "aios.h"
#include "posix.h"
AIOS_ENTRY {
    const char *filename = posix_args();
    if (!filename || !filename[0]) {
        write(STDERR_FILENO, "usage: rm <file>\n", 17);
        return 1;
    }

    if (unlink(filename) < 0) {
        write(STDERR_FILENO, filename, strlen(filename));
        write(STDERR_FILENO, ": delete failed\n", 16);
        return 1;
    }
    return 0;
}
