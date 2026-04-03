/* GNU-style hello world — NO AIOS headers, NO AIOS_INIT.
 * Just standard POSIX C. If this works, GNU tools can work. */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char *argv[]) {
    printf("Hello from a pure POSIX program!\n");
    printf("PID: %d\n", getpid());
    printf("UID: %d\n", getuid());

    /* Test file I/O */
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd >= 0) {
        char buf[64];
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("Hostname: %s", buf);
        }
        close(fd);
    }

    printf("This program has NO AIOS-specific code.\n");
    return 0;
}
