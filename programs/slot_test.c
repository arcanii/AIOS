#include "aios.h"

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    sys->puts_direct("SLOT TEST: I am alive!\n");
    sys->puts_direct("SLOT TEST: pid=");
    put_dec(sys->getpid());
    sys->puts_direct("\n");
    sys->puts_direct("SLOT TEST: done\n");
    return 0;
}
