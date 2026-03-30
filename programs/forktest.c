#include "aios.h"
#include "posix.h"

int main(aios_syscalls_t *sys_arg) {
    sys = sys_arg;
    
    puts("=== Fork Test ===\n");
    puts("Parent PID: ");
    put_dec((unsigned int)getpid());
    puts("\n");
    
    int pid = fork();
    
    if (pid < 0) {
        puts("Fork returned -1 (not yet supported)\n");
        puts("Using spawn() instead...\n\n");
        
        /* Test spawn instead */
        int child = spawn("/bin/hello.bin", "");
        if (child >= 0) {
            puts("Spawned hello.bin as PID ");
            put_dec((unsigned int)child);
            puts("\n");
            sleep(1);
            int status = 0;
            int rc = waitpid(child, &status);
            if (rc >= 0) {
                puts("Child exited with code ");
                put_dec((unsigned int)status);
                puts("\n");
            }
        }
    } else if (pid == 0) {
        puts("Child process running\n");
    } else {
        puts("Parent: forked child PID=");
        put_dec((unsigned int)pid);
        puts("\n");
    }
    
    puts("=== Fork Test Done ===\n");
    return 0;
}
