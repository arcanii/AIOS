#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

int main(int argc, char *argv[]) {
    write(1, "DIAG1: start\n", 13);
    
    /* Test printf */
    printf("DIAG2: printf works\n");
    fflush(stdout);
    
    /* Test time (no fs needed) */
    write(1, "DIAG3: before time\n", 19);
    struct timespec ts;
    clock_gettime(0, &ts);
    printf("DIAG4: time=%ld\n", (long)ts.tv_sec);
    fflush(stdout);
    
    /* Test open */
    write(1, "DIAG5: before open\n", 19);
    int fd = open("/etc/hostname", O_RDONLY);
    printf("DIAG6: open=%d errno=%d\n", fd, fd < 0 ? errno : 0);
    fflush(stdout);
    
    if (fd >= 0) {
        char buf[64] = {0};
        int n = read(fd, buf, 63);
        printf("DIAG7: read=%d data=%s\n", n, buf);
        fflush(stdout);
        close(fd);
    }
    
    write(1, "DIAG8: done\n", 12);
    return 0;
}
