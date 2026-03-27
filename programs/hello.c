#include "aios.h"

int _start(aios_syscalls_t *sys) {
    puts("Hello from AIOS sandbox!\n");
    puts("This program was written by Claude,\n");
    puts("compiled on the host, and executed\n");
    puts("inside an seL4 protection domain.\n");
    return 0;
}
