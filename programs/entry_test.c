#include "aios.h"

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    sys->puts_direct("ENTRY TEST: direct works\n");
    return 0;
}
