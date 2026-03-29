#include "aios.h"

static void put_num(int n) {
    char buf[12];
    int i = 0;
    if (n == 0) { sys->puts_direct("0"); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    char out[12];
    int j = 0;
    while (i > 0) out[j++] = buf[--i];
    out[j] = 0;
    sys->puts_direct(out);
}

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    sys->puts_direct("daemon: starting (ticking every 5s)\n");
    for (int i = 1; i <= 100; i++) {
        sleep(5);
        sys->puts_direct("daemon: tick ");
        put_num(i);
        sys->puts_direct("\n");
    }
    sys->puts_direct("daemon: finished\n");
    return 0;
}
