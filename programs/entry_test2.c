#include "aios.h"
#include "posix.h"

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    sys->puts_direct("ENTRY TEST2: with posix.h\n");
    return 0;
}
