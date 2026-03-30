#include "aios.h"
#include "posix.h"

int main(aios_syscalls_t *sys_arg) {
    sys = sys_arg;
    
    puts("=== AIOS Scheduler Stress Test ===\n");
    puts("Spawning multiple background processes...\n\n");
    
    /* Spawn several sieve processes */
    int pids[6];
    int count = 0;
    char *progs[] = {
        "/bin/sieve.bin",
        "/bin/fib.bin",
        "/bin/hello.bin",
        "/bin/sieve.bin",
        "/bin/fib.bin",
        "/bin/hello.bin"
    };
    
    for (int i = 0; i < 6; i++) {
        int pid = spawn(progs[i], "");
        if (pid >= 0) {
            puts("  Spawned ");
            puts(progs[i]);
            puts(" -> PID ");
            put_dec((unsigned long)pid);
            puts("\n");
            pids[count++] = pid;
        } else {
            puts("  Failed to spawn ");
            puts(progs[i]);
            puts(" (rc=");
            put_dec((unsigned long)pid);
            puts(")\n");
        }
    }
    
    puts("\nSpawned ");
    put_dec((unsigned long)count);
    puts(" processes\n");
    
    /* Wait a bit for them to run */
    sleep(2);
    
    /* Try to wait for each */
    puts("\nWaiting for children:\n");
    for (int i = 0; i < count; i++) {
        int status = 0;
        /* Poll for completion */
        for (int attempt = 0; attempt < 20; attempt++) {
            int rc = waitpid(pids[i], &status);
            if (rc >= 0) {
                puts("  PID ");
                put_dec((unsigned long)pids[i]);
                puts(" exited with code ");
                put_dec((unsigned long)status);
                puts("\n");
                break;
            }
            sleep(1);
        }
    }
    
    puts("\n=== Stress test complete ===\n");
    return 0;
}
