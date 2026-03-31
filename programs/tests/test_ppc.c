#include "aios.h"

int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    sys->puts_direct("PPC works!\n");
    return 42;
}
