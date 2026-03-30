#include "aios.h"
#include "posix.h"

#undef strlen
#undef strcmp
#undef strcpy
#undef strncpy
#undef memcpy
#undef memset
#undef malloc
#undef free
#undef open
#undef close
#undef read
#undef write
#undef puts
#undef putc
#undef exit
#undef sleep
#undef time

#define print(s) sys->puts_direct(s)

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    
    print("=== AIOS Network Status ===\n\n");
    
    /* Get hostname */
    char host[64];
    gethostname(host, sizeof(host));
    print("Hostname: "); print(host); print("\n");
    
    /* System info */
    struct utsname uts;
    uname(&uts);
    print("System:   "); print(uts.sysname); print(" "); print(uts.release); print("\n");
    print("Arch:     "); print(uts.machine); print("\n");
    print("Kernel:   "); print(uts.version); print("\n");
    
    /* Test socket creation */
    print("\nSocket test:\n");
    int tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd >= 0) {
        print("  TCP socket: OK (fd=");
        char fbuf[4]; fbuf[0] = '0' + tcp_fd; fbuf[1] = '\0';
        print(fbuf);
        print(")\n");
        close(tcp_fd);
    } else {
        print("  TCP socket: FAILED\n");
    }
    
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd >= 0) {
        print("  UDP socket: OK (fd=");
        char fbuf[4]; fbuf[0] = '0' + udp_fd; fbuf[1] = '\0';
        print(fbuf);
        print(")\n");
        close(udp_fd);
    } else {
        print("  UDP socket: FAILED\n");
    }
    
    /* HTTP server status */
    print("\nServices:\n");
    print("  HTTP server: listening on port 80\n");
    print("  Access via: http://localhost:8888 (QEMU forward)\n");
    
    print("\n=== Done ===\n");
    return 0;
}
