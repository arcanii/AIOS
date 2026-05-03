/* test_mprotect -- v0.4.126 mprotect IPC smoke
 *
 * Mmap a 2-page anonymous region R/W, prove R/W works, then mprotect
 * to R/O and observe the return value plus a successful read-back.
 * A subsequent write would fault and kill the process; we don't do
 * that here because there's no SIGSEGV handler in AIOS yet -- the
 * point is to confirm the IPC + kernel Page_Map mechanism.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

int main(void) {
    size_t len = 2 * 4096;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED || p == NULL) {
        printf("FAIL: mmap returned %p\n", p);
        return 1;
    }
    printf("OK: mmap returned %p\n", p);

    /* Touch both pages so they exist in the kernel PTE. */
    volatile uint32_t *w = (uint32_t *)p;
    w[0] = 0xCAFEBABE;
    w[1024] = 0xDEADBEEF;
    if (w[0] != 0xCAFEBABE || w[1024] != 0xDEADBEEF) {
        printf("FAIL: read-back before mprotect\n");
        return 1;
    }
    printf("OK: pre-mprotect read/write works\n");

    /* mprotect R/O -- should succeed. */
    int rc = mprotect(p, len, PROT_READ);
    printf("mprotect(R/O) rc=%d\n", rc);
    if (rc != 0) {
        printf("FAIL: mprotect returned non-zero\n");
        return 1;
    }

    /* Reads should still work. */
    if (w[0] != 0xCAFEBABE || w[1024] != 0xDEADBEEF) {
        printf("FAIL: post-mprotect read mismatch\n");
        return 1;
    }
    printf("OK: post-mprotect read works\n");

    /* Flip back to R/W -- should also succeed. */
    rc = mprotect(p, len, PROT_READ | PROT_WRITE);
    printf("mprotect(R/W) rc=%d\n", rc);
    if (rc != 0) {
        printf("FAIL: mprotect back to R/W failed\n");
        return 1;
    }

    /* Now writing should work again. */
    w[0] = 0x12345678;
    if (w[0] != 0x12345678) {
        printf("FAIL: post-restore write didn't take\n");
        return 1;
    }
    printf("OK: post-restore write works\n");

    /* v0.4.127: PROT_NONE round trip -- IPC succeeds; subsequent
     * accesses would fault but we can't test that without a SIGSEGV
     * handler. Restoring to PROT_READ|PROT_WRITE proves the cap survived. */
    rc = mprotect(p, len, PROT_NONE);
    printf("mprotect(NONE) rc=%d\n", rc);
    if (rc != 0) {
        printf("FAIL: PROT_NONE rejected\n");
        return 1;
    }
    rc = mprotect(p, len, PROT_READ | PROT_WRITE);
    printf("mprotect(R/W after NONE) rc=%d\n", rc);
    if (rc != 0 || w[0] != 0x12345678) {
        printf("FAIL: restore after PROT_NONE\n");
        return 1;
    }
    printf("OK: PROT_NONE round-trip works\n");

    /* PROT_EXEC -- IPC accepts and clears the XN bit. */
    rc = mprotect(p, len, PROT_READ | PROT_EXEC);
    printf("mprotect(R/X) rc=%d\n", rc);
    if (rc != 0) {
        printf("FAIL: PROT_EXEC rejected\n");
        return 1;
    }
    printf("OK: PROT_EXEC accepted\n");

    printf("MPROTECT-DONE\n");
    return 0;
}
