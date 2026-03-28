#include "aios.h"
#include "posix.h"
AIOS_ENTRY {
    const char *msg = posix_args();
    if (msg && msg[0]) {
        write(STDOUT_FILENO, msg, strlen(msg));
    }
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}
