/* test_fileread.c -- test regular file read */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *path = "/tmp/hello.c";
    int fd, n, i;
    char buf[256];

    (void)argc; (void)argv;

    printf("test_fileread: opening %s\n", path);
    fd = open(path, O_RDONLY);
    printf("fd = %d\n", fd);
    if (fd < 0) {
        printf("FAIL: open returned %d\n", fd);
        return 1;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    printf("read returned %d\n", n);
    if (n <= 0) {
        printf("FAIL: read returned %d\n", n);
        close(fd);
        return 1;
    }
    buf[n] = 0;

    printf("content (%d bytes):\n", n);
    /* Print byte by byte to see what we got */
    for (i = 0; i < n; i++) {
        if (buf[i] >= 32 && buf[i] < 127)
            printf("%c", buf[i]);
        else if (buf[i] == 10)
            printf("\n");
        else
            printf("[%02x]", (unsigned char)buf[i]);
    }
    printf("\n--- end ---\n");
    close(fd);
    return 0;
}
