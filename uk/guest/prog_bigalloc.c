/*
 * prog_bigalloc.c -- exercises real, mmap-backed malloc (M3c step 2).
 *
 * Allocates 4 MB in 64 KB chunks -- far more than libaios's old 256 KB static arena -- then fills
 * and verifies every byte. Each time the heap is exhausted, libaios issues AIOS_SYS_MMAP, which the
 * AIOS kernel services by injecting a real mmap into this (stopped) guest. If this prints the
 * success line, the kernel grew the guest's address space on demand.
 */
#include "libaios.h"

#define CHUNKS 64
#define CHUNK_SZ (64 * 1024)

int main(void) {
    char *p[CHUNKS];

    for (int i = 0; i < CHUNKS; i++) {
        p[i] = malloc(CHUNK_SZ);
        if (!p[i]) { printf("malloc FAILED at chunk %d\n", i); return 1; }
        memset(p[i], 'A' + (i % 26), CHUNK_SZ);
    }

    long total = 0;
    for (int i = 0; i < CHUNKS; i++) {
        char expect = (char)('A' + (i % 26));
        for (int j = 0; j < CHUNK_SZ; j++)
            if (p[i][j] != expect) { printf("verify FAILED: chunk %d byte %d\n", i, j); return 2; }
        total += CHUNK_SZ;
    }

    printf("OK: allocated + verified %d KB of real mmap-backed memory across %d chunks\n",
           (int)(total / 1024), CHUNKS);
    return 0;
}
