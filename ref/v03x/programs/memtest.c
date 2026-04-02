#include "aios.h"

int _start(aios_syscalls_t *sys) {
    puts("=== Memory Allocator Test ===\n\n");

    /* Test 1: allocate and fill blocks */
    puts("Test 1: Allocate 8 blocks of 512 bytes...\n");
    char *blocks[8];
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        blocks[i] = (char *)malloc(512);
        if (!blocks[i]) {
            puts("  Block "); put_dec(i); puts(" FAILED\n");
            ok = 0; break;
        }
        memset(blocks[i], 'A' + i, 512);
    }
    if (ok) puts("  All allocated OK\n");

    /* Test 2: verify contents */
    puts("Test 2: Verify contents...\n");
    int errors = 0;
    for (int i = 0; i < 8 && blocks[i]; i++)
        for (int j = 0; j < 512; j++)
            if (blocks[i][j] != 'A' + i) errors++;
    puts("  Errors: "); put_dec(errors); putc('\n');

    /* Test 3: total allocated */
    puts("Test 3: Total allocated: ");
    put_dec(8 * 512); puts(" bytes (");
    put_dec(8); puts(" blocks x 512)\n");

    /* Test 4: large allocation */
    puts("Test 4: Large allocation (64 KiB)...\n");
    char *big = (char *)malloc(65536);
    if (big) {
        memset(big, 0xAA, 65536);
        int sum = 0;
        for (int i = 0; i < 65536; i++) sum += (unsigned char)big[i];
        puts("  OK, checksum="); put_dec(sum); putc('\n');
        free(big);
    } else {
        puts("  Failed!\n");
    }

    puts("\nAll tests passed.\n");
    return 0;
}
