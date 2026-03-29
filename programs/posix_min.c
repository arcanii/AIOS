#include "aios.h"
#include "posix.h"

AIOS_ENTRY {
    write(STDOUT_FILENO, "POSIX MIN: alive\n", 17);
    return 0;
}
