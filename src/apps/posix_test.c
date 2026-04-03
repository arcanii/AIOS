/*
 * AIOS 0.4.x — POSIX test
 * Uses standard open/read/write/close — NO raw IPC.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "aios_posix.h"

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);

    printf("=== POSIX I/O Test ===\n\n");

    /* Test 1: printf (stdout via IPC) */
    printf("Test 1: printf works\n");

    /* Test 2: open + read + write + close */
    printf("Test 2: open/read/write/close\n");
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd < 0) {
        printf("  FAIL: open returned %d\n", fd);
    } else {
        char buf[256];
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("  /etc/hostname = '%s'\n", buf);
        } else {
            printf("  FAIL: read returned %d\n", n);
        }
        close(fd);
        printf("  PASS: open/read/close cycle\n");
    }

    /* Test 3: open a second file */
    printf("Test 3: second file\n");
    fd = open("/hello.txt", O_RDONLY);
    if (fd < 0) {
        printf("  FAIL: open returned %d\n", fd);
    } else {
        char buf[256];
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("  /hello.txt = '%s'\n", buf);
        }
        close(fd);
        printf("  PASS\n");
    }

    /* Test 4: open nonexistent file */
    printf("Test 4: nonexistent file\n");
    fd = open("/no/such/file", O_RDONLY);
    if (fd < 0) {
        printf("  PASS: open correctly returned %d\n", fd);
    } else {
        printf("  FAIL: should have returned error\n");
        close(fd);
    }

    printf("\n=== All POSIX tests complete ===\n");
    return 0;
}
