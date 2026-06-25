/*
 * prog_errno.c -- exercises errno (M3e step 3), real C via the -nostdinc shadow headers.
 *
 * A failing AIOS syscall now returns a negated error code (-errno); the libaios POSIX wrappers
 * translate that to the libc contract (set errno, return -1). This is the layer real sbase/dash
 * error reporting depends on (eprintf + strerror(errno)). Checks ENOENT (bad open) and EBADF (bad
 * read), the success path, and perror formatting.
 */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int main(void) {
    int fails = 0;

    int fd = open("/no/such/file", O_RDONLY, 0);
    printf("open(/no/such/file) -> %d, errno=%d (%s)\n", fd, errno, strerror(errno));
    if (!(fd < 0 && errno == ENOENT)) fails++;

    char c;
    long n = read(77, &c, 1);
    printf("read(77)            -> %ld, errno=%d (%s)\n", n, errno, strerror(errno));
    if (!(n < 0 && errno == EBADF)) fails++;

    fd = open("/etc/hostname", O_RDONLY, 0);
    printf("open(/etc/hostname) -> %d (success: >= 0)\n", fd);
    if (fd < 0) fails++; else close(fd);

    errno = EACCES;
    perror("perror demo");        /* prints "perror demo: Permission denied" */

    printf("prog_errno: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
