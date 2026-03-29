#include "aios.h"

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    sys->puts_direct("SPAWN: step 1 - start\n");
    
    sys->puts_direct("SPAWN: step 2 - calling spawn()\n");
    int child_pid = spawn("SLOT_TEST.BIN", "");
    
    sys->puts_direct("SPAWN: step 3 - spawn returned ");
    if (child_pid < 0) {
        sys->puts_direct("FAIL\n");
        return 1;
    }
    sys->puts_direct("OK\n");

    sys->puts_direct("SPAWN: step 4 - calling waitpid()\n");
    int status = 0;
    int ret = waitpid(child_pid, &status);
    
    sys->puts_direct("SPAWN: step 5 - waitpid returned\n");
    sys->puts_direct("SPAWN: done\n");
    return 0;
}
