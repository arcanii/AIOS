#include "aios.h"

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    for (int i = 0; i < 10000; i++) {
        sleep(60);
    }
    return 0;
}
