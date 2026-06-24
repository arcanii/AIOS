/* bulkwrite.c -- v0.4.298 regression test for the bulk file write path
 * (PIPE_PWRITE_BULK, armed via `cat /proc/pwritebulk.1`). Writes a large buffer
 * with a position-dependent pattern in ONE write() per file so the bulk fast path
 * engages (each write is far above the 4 KB threshold), reads it back with read(),
 * and compares byte for byte. A second pass writes from a deliberately UNALIGNED
 * source pointer (buf+1) to exercise the server-side mid-page bounce, and a third
 * uses an odd length (neither a page nor a 64 KB-chunk multiple). The test behaves
 * identically whether the feature is on or off -- the SAME binary validates
 * byte-exactness in both modes; /proc/pwritebulk counters prove which path ran.
 * One-line PASS/FAIL output so the netconsole relay does not truncate it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define N (256 * 1024)   /* 256 KB -- > the 64 KB per-IPC chunk, so the caller loops */

static void fill(unsigned char *b, int n) {
    for (int i = 0; i < n; i++)
        b[i] = (unsigned char)((i * 131 + (i >> 8) * 7 + 11) & 0xFF);
}

/* Write all of buf in write() calls (each as large as possible -> bulk path),
 * read it back, return 1 if byte-exact. */
static int roundtrip(const char *path, const unsigned char *buf, int n) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { printf("  open-w %s failed\n", path); return 0; }
    int w = 0;
    while (w < n) {
        int r = (int)write(fd, buf + w, (size_t)(n - w));
        if (r <= 0) { printf("  write %s short at %d\n", path, w); close(fd); return 0; }
        w += r;
    }
    close(fd);
    unsigned char *rb = malloc((size_t)n);
    if (!rb) { printf("  malloc rb failed\n"); return 0; }
    memset(rb, 0xAB, (size_t)n);
    fd = open(path, O_RDONLY, 0);
    if (fd < 0) { printf("  open-r %s failed\n", path); free(rb); return 0; }
    int rd = 0;
    while (rd < n) {
        int r = (int)read(fd, rb + rd, (size_t)(n - rd));
        if (r <= 0) break;
        rd += r;
    }
    close(fd);
    int ok = (rd == n) && (memcmp(buf, rb, (size_t)n) == 0);
    if (!ok) {
        int mm = -1;
        for (int i = 0; i < n && i < rd; i++) if (buf[i] != rb[i]) { mm = i; break; }
        printf("  %s MISMATCH rd=%d/%d firstmiss=%d\n", path, rd, n, mm);
    }
    free(rb);
    return ok;
}

int main(void) {
    unsigned char *buf = malloc(N + 16);
    if (!buf) { printf("BULKWRITE: FAIL malloc\n"); return 1; }
    fill(buf, N + 16);
    int a = roundtrip("/tmp/bw_aligned", buf, N);          /* page-aligned source */
    int b = roundtrip("/tmp/bw_unalign", buf + 1, N);      /* mid-page source start */
    int c = roundtrip("/tmp/bw_odd", buf, 100000);         /* odd length */
    free(buf);
    if (a && b && c) {
        printf("BULKWRITE: PASS aligned+unaligned+odd byte-exact (%d KB)\n", N / 1024);
        return 0;
    }
    printf("BULKWRITE: FAIL aligned=%d unaligned=%d odd=%d\n", a, b, c);
    return 2;
}
