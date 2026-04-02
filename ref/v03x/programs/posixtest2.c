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
    
    print("=== POSIX Tier 1 Tests ===\n");
    
    /* 1. uname */
    struct utsname uts;
    uname(&uts);
    print("1. uname: "); print(uts.sysname); print(" ");
    print(uts.nodename); print(" ");
    print(uts.release); print(" ");
    print(uts.machine); print("\n");
    
    /* 2. gethostname */
    char host[64];
    gethostname(host, sizeof(host));
    print("2. hostname: "); print(host); print("\n");
    
    /* 3. gettimeofday */
    struct timeval tv;
    gettimeofday(&tv, (void *)0);
    print("3. gettimeofday: ");
    if (tv.tv_sec > 0) print("OK (time > 0)\n");
    else print("WARN (time = 0)\n");
    
    /* 4. clock_gettime */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    print("4. clock_gettime: ");
    if (ts.tv_sec > 0) print("OK\n");
    else print("WARN\n");
    
    /* 5. getpwuid */
    struct passwd *pw = getpwuid(0);
    print("5. getpwuid(0): ");
    if (pw) { print(pw->pw_name); print(" dir="); print(pw->pw_dir); print(" shell="); print(pw->pw_shell); }
    else print("NULL");
    print("\n");
    
    /* 6. getpwnam */
    pw = getpwnam("root");
    print("6. getpwnam(root): ");
    if (pw) { print("uid="); if (pw->pw_uid == 0) print("0"); else print("?"); }
    else print("NULL");
    print("\n");
    
    /* 7. getgrgid */
    struct group *gr = getgrgid(0);
    print("7. getgrgid(0): ");
    if (gr) print(gr->gr_name);
    else print("NULL");
    print("\n");
    
    /* 8. sigaction */
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = 0;
    int rc = sigaction(SIGINT, &sa, (void *)0);
    print("8. sigaction: ");
    if (rc == 0) print("OK\n"); else print("FAIL\n");
    
    /* 9. sigset operations */
    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGTERM);
    print("9. sigset: SIGTERM member=");
    if (sigismember(&ss, SIGTERM)) print("yes"); else print("no");
    print(" SIGINT member=");
    if (sigismember(&ss, SIGINT)) print("yes"); else print("no");
    print("\n");
    
    /* 10. setsockopt */
    rc = setsockopt(0, 0, 0, (void *)0, 0);
    print("10. setsockopt: ");
    if (rc == 0) print("OK\n"); else print("FAIL\n");
    
    /* 11. readlink (should fail with ENOSYS) */
    char lbuf[64];
    ssize_t lr = readlink("/bin/hello.bin", lbuf, sizeof(lbuf));
    print("11. readlink: ");
    if (lr == -1 && errno == ENOSYS) print("ENOSYS (correct)\n");
    else print("unexpected\n");
    
    print("=== All Tier 1 tests done ===\n");
    return 0;
}
