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

/* Position-dependent byte pattern so a shuffled/wrong-block free shows as a
 * content mismatch (not just a size change). */
static unsigned char pat(unsigned int i) {
    return (unsigned char)(i ^ (i >> 7) ^ (i >> 13));
}

static int fill_pattern(const char *path, unsigned int sz) {
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) { printf("FAIL: open %s\n", path); return -1; }
    unsigned char buf[4096];
    for (unsigned int off = 0; off < sz; ) {
        unsigned int n = sz - off; if (n > sizeof buf) n = sizeof buf;
        for (unsigned int j = 0; j < n; j++) buf[j] = pat(off + j);
        if (write(fd, buf, (int)n) != (int)n) { printf("FAIL: write @%u\n", off); close(fd); return -1; }
        off += n;
    }
    close(fd);
    return 0;
}

static int verify_prefix(const char *path, unsigned int sz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("FAIL: reopen %s\n", path); return -1; }
    unsigned char buf[4096];
    for (unsigned int off = 0; off < sz; ) {
        unsigned int n = sz - off; if (n > sizeof buf) n = sizeof buf;
        int r = read(fd, buf, (int)n);
        if (r != (int)n) { printf("FAIL: read @%u got %d\n", off, r); close(fd); return -1; }
        for (unsigned int j = 0; j < n; j++)
            if (buf[j] != pat(off + j)) { printf("FAIL: mismatch @%u\n", off + j); close(fd); return -1; }
        off += n;
    }
    close(fd);
    return 0;
}

/* Exercise the indirect-block free paths: BIG_SZ (320KB) spans direct + single-
 * indirect + double-indirect; truncating to KEEP_SZ (20KB) keeps the direct
 * blocks and part of the single-indirect, freeing deep into the double-indirect. */
#define BIG_SZ   (320u * 1024u)
#define KEEP_SZ  (20u * 1024u)
static int big_truncate_test(void) {
    const char *p = "/ftbig";
    struct stat st;
    if (fill_pattern(p, BIG_SZ) != 0) return 1;
    int fd = open(p, O_RDWR);
    if (fd < 0) { printf("FAIL: reopen for trunc\n"); return 1; }
    int rc = ftruncate(fd, (off_t)KEEP_SZ);
    close(fd);
    if (rc != 0) { printf("FAIL: big ftruncate rc=%d\n", rc); return 1; }
    if (stat(p, &st) != 0 || st.st_size != (off_t)KEEP_SZ) {
        printf("FAIL: big trunc size=%lld\n", (long long)st.st_size); return 1;
    }
    if (verify_prefix(p, KEEP_SZ) != 0) { printf("FAIL: kept data corrupt after big truncate\n"); return 1; }
    printf("OK: big truncate kept %u bytes intact (indirect free)\n", KEEP_SZ);
    fd = open(p, O_RDWR);
    rc = ftruncate(fd, 0);
    close(fd);
    if (rc != 0 || stat(p, &st) != 0 || st.st_size != 0) { printf("FAIL: big trunc to 0\n"); return 1; }
    unlink(p);
    /* the freed blocks must be reclaimable and the fs uncorrupted */
    if (fill_pattern("/ftreuse", 256u * 1024u) != 0) { printf("FAIL: reuse write\n"); return 1; }
    if (verify_prefix("/ftreuse", 256u * 1024u) != 0) { printf("FAIL: reuse verify\n"); return 1; }
    unlink("/ftreuse");
    printf("OK: freed blocks reclaimed, fs intact\n");
    return 0;
}

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

    if (big_truncate_test() != 0) return 1;

    printf("FTRUNC-DONE\n");
    return 0;
}
