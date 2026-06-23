/* mmaptest.c -- v0.4.295 regression test for demand-paged anonymous mmap.
 * Exercises the lazy MAP_ANONYMOUS path that replaced the old 4 MB eager cap:
 * reserve a 16 MB anon region, verify pages fault in zero-filled, write a
 * per-page pattern across all 4096 pages, read it back, then munmap.
 * One-line PASS/FAIL output so the netconsole relay does not truncate it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int main(void) {
    size_t big = 16u * 1024 * 1024;            /* > old 4 MB eager cap */
    size_t npg = big / 4096;
    unsigned char *q = mmap(NULL, big, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED || q == NULL) {
        printf("MMAPTEST: FAIL mmap16 (errno path / old cap?)\n");
        return 1;
    }
    int zero_ok = 1, rw_ok = 1;
    for (size_t i = 0; i < npg; i++) {
        volatile unsigned int *w = (unsigned int *)(q + i * 4096);
        if (*w != 0) zero_ok = 0;              /* fresh frames must be zero */
        *w = (unsigned int)(i * 2654435761u);
    }
    for (size_t i = 0; i < npg; i++) {
        volatile unsigned int *w = (unsigned int *)(q + i * 4096);
        if (*w != (unsigned int)(i * 2654435761u)) rw_ok = 0;
    }
    int mu = munmap(q, big);
    if (zero_ok && rw_ok && mu == 0) {
        printf("MMAPTEST: PASS 16MB anon demand-paged (%lu pages)\n",
               (unsigned long)npg);
        return 0;
    }
    printf("MMAPTEST: FAIL zero=%d rw=%d munmap=%d\n", zero_ok, rw_ok, mu);
    return 2;
}
