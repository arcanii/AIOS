/*
 * test_fswrite -- ext2 file write/read/unlink test
 *
 * Tests: open(O_CREAT|O_WRONLY), write, close, re-open, read-back,
 *        stat, unlink, verify unlink.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int tests_run = 0;
static int tests_pass = 0;

static void check(const char *name, int cond) {
    tests_run++;
    if (cond) {
        tests_pass++;
        printf("  PASS: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
    }
}

int main(void) {
    const char *path = "/tmp/test_fswrite.dat";
    const char *msg  = "AIOS ext2 write test 1234567890";
    char buf[128];
    int fd, n;
    struct stat st;

    printf("=== AIOS FS Write Test ===\n");

    /* Test 1: create and write */
    printf("\n[1] Create and write\n");
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("open(O_CREAT|O_WRONLY) >= 0", fd >= 0);
    if (fd < 0) {
        printf("Cannot continue without file\n");
        printf("\n=== Results: %d/%d passed ===\n", tests_pass, tests_run);
        return 1;
    }

    n = write(fd, msg, strlen(msg));
    check("write returns correct length", n == (int)strlen(msg));
    close(fd);

    /* Test 2: stat the file */
    printf("\n[2] Stat\n");
    n = stat(path, &st);
    check("stat returns 0", n == 0);
    check("stat size matches written length", st.st_size == (off_t)strlen(msg));
    check("stat mode is regular file", (st.st_mode & S_IFMT) == S_IFREG);

    /* Test 3: read back and verify */
    printf("\n[3] Read back\n");
    memset(buf, 0, sizeof(buf));
    fd = open(path, O_RDONLY);
    check("open(O_RDONLY) >= 0", fd >= 0);
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf));
        check("read returns correct length", n == (int)strlen(msg));
        check("data matches what was written", memcmp(buf, msg, strlen(msg)) == 0);
        close(fd);
    }

    /* Test 4: append mode */
    printf("\n[4] Append\n");
    fd = open(path, O_WRONLY | O_APPEND);
    check("open(O_APPEND) >= 0", fd >= 0);
    if (fd >= 0) {
        const char *extra = "XTRA";
        n = write(fd, extra, 4);
        check("append write returns 4", n == 4);
        close(fd);

        n = stat(path, &st);
        check("stat after append: size grew", st.st_size == (off_t)(strlen(msg) + 4));
    }

    /* Test 5: unlink */
    printf("\n[5] Unlink\n");
    n = unlink(path);
    check("unlink returns 0", n == 0);

    n = stat(path, &st);
    check("stat after unlink returns -1", n == -1);

    /* Summary */
    printf("\n=== Results: %d/%d passed ===\n", tests_pass, tests_run);
    return (tests_pass == tests_run) ? 0 : 1;
}
