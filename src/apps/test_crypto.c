/* test_crypto.c -- crypto_server test
 *
 * Reads /dev/urandom and getrandom() to verify the
 * ChaCha20 CSPRNG is producing non-repeating output.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/syscall.h>

static void print_hex(const char *label, const unsigned char *buf, int len)
{
    printf("%s: ", label);
    int i;
    for (i = 0; i < len; i++)
        printf("%02x", buf[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    unsigned char buf1[32], buf2[32], buf3[32], buf4[32];
    int fd, n;

    (void)argc; (void)argv;

    printf("=== crypto_server test ===\n");

    /* Test 1: read /dev/urandom twice, should differ */
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        printf("FAIL: open /dev/urandom: fd=%d\n", fd);
        return 1;
    }
    n = read(fd, buf1, 32);
    printf("read1: %d bytes\n", n);
    if (n != 32) { printf("FAIL: short read1\n"); return 1; }

    n = read(fd, buf2, 32);
    printf("read2: %d bytes\n", n);
    if (n != 32) { printf("FAIL: short read2\n"); return 1; }
    close(fd);

    print_hex("urandom-1", buf1, 32);
    print_hex("urandom-2", buf2, 32);

    if (memcmp(buf1, buf2, 32) == 0) {
        printf("FAIL: two reads returned identical data\n");
        return 1;
    }
    printf("OK: /dev/urandom reads differ\n\n");

    /* Test 2: /dev/random (same backing) */
    fd = open("/dev/random", O_RDONLY);
    if (fd < 0) {
        printf("SKIP: /dev/random not available\n");
    } else {
        n = read(fd, buf3, 32);
        close(fd);
        print_hex("random  ", buf3, 32);
        if (n == 32 && memcmp(buf3, buf1, 32) != 0)
            printf("OK: /dev/random differs from /dev/urandom\n\n");
        else if (n == 32)
            printf("WARN: /dev/random matched /dev/urandom\n\n");
        else
            printf("WARN: /dev/random short read: %d\n\n", n);
    }

    /* Test 3: getrandom() syscall */
    n = syscall(278, buf4, 32, 0);  /* SYS_getrandom */
    printf("getrandom: %d bytes\n", n);
    if (n != 32) { printf("FAIL: getrandom short\n"); return 1; }
    print_hex("getrandom", buf4, 32);

    if (memcmp(buf4, buf1, 32) != 0 && memcmp(buf4, buf2, 32) != 0) {
        printf("OK: getrandom differs from urandom reads\n\n");
    } else {
        printf("FAIL: getrandom matched a previous read\n");
        return 1;
    }

    /* Test 4: check for non-zero (extremely unlikely with CSPRNG) */
    int all_zero = 1;
    int i;
    for (i = 0; i < 32; i++) {
        if (buf1[i] != 0) { all_zero = 0; break; }
    }
    if (all_zero) {
        printf("FAIL: urandom returned all zeros\n");
        return 1;
    }

    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
