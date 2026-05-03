/* test_ftruncate -- v0.4.130 ftruncate IPC smoke
 *
 * Create a file with 100 bytes, truncate to 50, stat to verify size.
 * Then truncate to 0 and verify empty. Tests the FS_TRUNCATE IPC and
 * the cached size update on the client side.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

int main(void) {
    const char *path = "/tmp/ft_test";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        printf("FAIL: open: %d\n", fd);
        return 1;
    }

    char buf[100];
    for (int i = 0; i < 100; i++) buf[i] = 'A' + (i % 26);
    int wn = write(fd, buf, 100);
    if (wn != 100) {
        printf("FAIL: write returned %d\n", wn);
        return 1;
    }
    printf("OK: wrote 100 bytes\n");

    /* Truncate to 50 */
    int rc = ftruncate(fd, 50);
    printf("ftruncate(50) rc=%d\n", rc);
    if (rc != 0) {
        printf("FAIL: ftruncate to 50 returned %d\n", rc);
        return 1;
    }

    /* Verify on-disk size via fresh stat */
    close(fd);
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("FAIL: stat after truncate\n");
        return 1;
    }
    printf("stat size=%lld\n", (long long)st.st_size);
    if (st.st_size != 50) {
        printf("FAIL: expected size 50, got %lld\n", (long long)st.st_size);
        return 1;
    }
    printf("OK: file truncated to 50 bytes\n");

    /* Truncate to 0 */
    fd = open(path, O_RDWR);
    if (fd < 0) { printf("FAIL: reopen\n"); return 1; }
    rc = ftruncate(fd, 0);
    printf("ftruncate(0) rc=%d\n", rc);
    if (rc != 0) {
        printf("FAIL: ftruncate to 0 returned %d\n", rc);
        return 1;
    }
    close(fd);
    if (stat(path, &st) != 0 || st.st_size != 0) {
        printf("FAIL: post-truncate-0 size=%lld\n", (long long)st.st_size);
        return 1;
    }
    printf("OK: file truncated to 0 bytes\n");

    unlink(path);
    printf("FTRUNC-DONE\n");
    return 0;
}
