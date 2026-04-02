#include "aios.h"
#include <posix.h>

static void print(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }
static void print_num(long n) {
    char buf[20]; int i = 0;
    if (n == 0) { write(STDOUT_FILENO, "0", 1); return; }
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (--i >= 0) write(STDOUT_FILENO, &buf[i], 1);
}

AIOS_ENTRY {
    int pass = 0, total = 0;
    glob_t g;

    total++;
    if (glob("/bin/*.bin", 0, (void *)0, &g) == 0 && g.gl_pathc > 0) {
        pass++; print("PASS");
    } else print("FAIL");
    print(": /bin/*.bin found ");
    print_num((long)g.gl_pathc);
    print(" files\n");
    globfree(&g);

    total++;
    if (glob("/etc/passwd", 0, (void *)0, &g) == 0 && g.gl_pathc == 1) {
        pass++; print("PASS");
    } else print("FAIL");
    print(": /etc/passwd exact match\n");
    globfree(&g);

    total++;
    if (glob("/etc/*.conf", 0, (void *)0, &g) == 0 && g.gl_pathc > 0) {
        pass++; print("PASS");
    } else print("FAIL");
    print(": /etc/*.conf found ");
    print_num((long)g.gl_pathc);
    print(" files\n");
    globfree(&g);

    total++;
    if (glob("/nonexistent/*.xyz", 0, (void *)0, &g) != 0) {
        pass++; print("PASS");
    } else print("FAIL");
    print(": nonexistent path returns GLOB_NOMATCH\n");

    if (pass == total) print("ALL PASS\n");
    return 0;
}
