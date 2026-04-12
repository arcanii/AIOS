/*
 * aios_entropy.c -- mbedTLS hardware entropy callback for AIOS
 *
 * Reads from /dev/urandom (splitmix64 PRNG in AIOS POSIX shim).
 * Required when MBEDTLS_ENTROPY_HARDWARE_ALT is enabled.
 */
#include <fcntl.h>
#include <unistd.h>
#include <stddef.h>

int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen)
{
    (void)data;
    int fd = open("/dev/urandom", 0);  /* O_RDONLY */
    if (fd >= 0) {
        ssize_t n = read(fd, output, len);
        close(fd);
        *olen = (n > 0) ? (size_t)n : 0;
        return 0;
    }
    /* Fallback: ARM cycle counter */
    unsigned long long cnt;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(cnt));
    for (size_t i = 0; i < len; i++) {
        cnt = cnt * 6364136223846793005ULL + 1442695040888963407ULL;
        output[i] = (unsigned char)(cnt >> 32);
    }
    *olen = len;
    return 0;
}
