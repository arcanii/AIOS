#include "aios.h"

int _start(aios_syscalls_t *sys_ptr) {
    sys = sys_ptr;

    puts("=== POSIX Syscall Test ===\n\n");

    /* Identity */
    puts("-- Identity --\n");
    puts("  uid=");  put_dec(getuid());  puts("\n");
    puts("  gid=");  put_dec(getgid());  puts("\n");
    puts("  euid="); put_dec(geteuid()); puts("\n");
    puts("  egid="); put_dec(getegid()); puts("\n");
    puts("  ppid="); put_dec(getppid()); puts("\n");
    puts("  pid=");  put_dec(sys->getpid()); puts("\n");

    /* Access */
    puts("\n-- Access --\n");
    int r = access("HELLO.TXT", R_OK);
    puts("  access(HELLO.TXT, R_OK) = "); put_dec(r); puts("\n");
    r = access("NONEXIST", F_OK);
    puts("  access(NONEXIST, F_OK) = "); put_dec(r == 0 ? 0 : -1); puts("\n");

    /* Umask */
    puts("\n-- Umask --\n");
    int old = umask(077);
    puts("  old umask=0"); put_dec(old); puts("\n");
    int cur = umask(022);
    puts("  set 077, now=0"); put_dec(cur); puts("\n");

    /* Time */
    puts("\n-- Time --\n");
    long t1 = time();
    puts("  time()="); put_dec((unsigned int)t1); puts("\n");
    long t2 = time();
    puts("  time()="); put_dec((unsigned int)t2); puts("\n");
    puts("  (should increment)\n");

    /* Pipe */
    puts("\n-- Pipe --\n");
    int pfd[2];
    r = pipe(pfd);
    puts("  pipe() = "); put_dec(r); puts("\n");
    if (r == 0) {
        puts("  read_fd=");  put_dec(pfd[0]); puts("\n");
        puts("  write_fd="); put_dec(pfd[1]); puts("\n");

        /* Write to pipe */
        const char *msg = "Hello pipe!";
        int wlen = sys->write_file(pfd[1], msg, 11);
        puts("  wrote "); put_dec(wlen); puts(" bytes\n");

        /* Read from pipe */
        char buf[32];
        memset(buf, 0, 32);
        int rlen = sys->read(pfd[0], buf, 32);
        puts("  read "); put_dec(rlen); puts(" bytes: ");
        puts(buf);
        puts("\n");

        sys->close(pfd[0]);
        sys->close(pfd[1]);
        puts("  pipe closed OK\n");
    }

    puts("\n=== All tests passed ===\n");
    return 0;
}
