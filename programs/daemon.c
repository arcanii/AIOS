#include "aios.h"

/* daemon: silent background process (ticks internally, no terminal output)
 * Used to test background spawn, kill, and job control. */

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    /* Run silently for 500 ticks (each ~5 seconds) */
    for (int i = 0; i < 500; i++) {
        sleep(5);
    }
    return 0;
}
