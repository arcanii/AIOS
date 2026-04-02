#include "aios.h"
#include "posix.h"

#define print(s) sys->puts_direct(s)

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    
    char buf[256];
    getcwd(buf, sizeof(buf));
    print("cwd: "); print(buf); print("\n");
    
    print("chdir(bin) = ");
    int rc = chdir("bin");
    if (rc == 0) print("OK\n"); else print("FAIL\n");
    
    getcwd(buf, sizeof(buf));
    print("cwd now: "); print(buf); print("\n");
    
    print("--- readdir after cd bin ---\n");
    DIR *d = opendir(".");
    if (d) {
        struct dirent *ent;
        int count = 0;
        while ((ent = readdir(d)) != NULL && count < 5) {
            print("  "); print(ent->d_name); print("\n");
            count++;
        }
        closedir(d);
    } else {
        print("opendir(.) failed\n");
    }
    
    chdir("/");
    print("\nBack to /, testing opendir(bin):\n");
    
    d = opendir("bin");
    if (d) {
        struct dirent *ent;
        int count = 0;
        while ((ent = readdir(d)) != NULL && count < 5) {
            print("  "); print(ent->d_name); print("\n");
            count++;
        }
        closedir(d);
    } else {
        print("opendir(bin) failed\n");
    }
    
    getcwd(buf, sizeof(buf));
    print("cwd after opendir(bin): "); print(buf); print("\n");
    
    return 0;
}
