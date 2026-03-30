#include "aios.h"
#include "posix.h"

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;

    printf("=== FILE I/O Test ===\n");

    /* Test fopen/fwrite */
    FILE *fp = fopen("ftest.txt", "w");
    if (!fp) { printf("FAIL: fopen(w) returned NULL\n"); return 1; }
    fprintf(fp, "Hello from fprintf!\n");
    fwrite("Line 2\n", 1, 7, fp);
    fclose(fp);
    printf("1. fopen/fwrite/fprintf/fclose: OK\n");

    /* Test fopen/fread */
    fp = fopen("ftest.txt", "r");
    if (!fp) { printf("FAIL: fopen(r) returned NULL\n"); return 1; }
    char buf[128];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    printf("2. fread got %d bytes: %s", (int)n, buf);
    fclose(fp);

    /* Test fgets */
    fp = fopen("ftest.txt", "r");
    if (fp) {
        char line[64];
        printf("3. fgets lines:\n");
        while (fgets(line, sizeof(line), fp)) {
            printf("   > %s", line);
        }
        printf("   feof=%d\n", feof(fp));
        fclose(fp);
    }

    /* Test strdup/strerror */
    char *dup = strdup("hello strdup");
    printf("4. strdup: %s\n", dup ? dup : "NULL");
    printf("5. strerror(2): %s\n", strerror(2));
    printf("6. strerror(13): %s\n", strerror(13));

    printf("=== All tests passed ===\n");
    return 0;
}
