#include "aios.h"
#include "posix.h"

int main(aios_syscalls_t *sys_arg) {
    sys = sys_arg;

    puts("=== Fork Test ===\n");
    puts("Before fork, PID=");
    put_dec((unsigned int)getpid());
    puts("\n");

    int pid = fork();

    if (pid < 0) {
        puts("Fork FAILED\n");
    } else if (pid == 0) {
        /* Child — inherits the slot, continues immediately */
        puts("Child: I am the child! PID=");
        put_dec((unsigned int)getpid());
        puts("\n");
        puts("Child: exiting with code 42\n");
        _exit(42);
    } else {
        /* Parent — resumed from swap after child exits */
        puts("Parent: resumed! Child was PID=");
        put_dec((unsigned int)pid);
        puts("\n");
        puts("Parent: my PID=");
        put_dec((unsigned int)getpid());
        puts("\n");
    }

    puts("=== Fork Test Done ===\n");
    return 0;
}
