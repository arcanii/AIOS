#include "aios.h"
#include <posix.h>

static void print(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }
static void print_num(unsigned long n) {
    char buf[20]; int i = 0;
    if (n == 0) { write(STDOUT_FILENO, "0", 1); return; }
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (--i >= 0) write(STDOUT_FILENO, &buf[i], 1);
}

AIOS_ENTRY {
    int pass = 0, total = 0;
    struct statvfs st;

    total++;
    if (statvfs("/", &st) == 0 && st.f_bsize == 1024) {
        pass++; print("PASS");
    } else print("FAIL");
    print(": statvfs f_bsize=");
    print_num(st.f_bsize);
    print("\n");

    total++;
    if (st.f_namemax == 255) {
        pass++; print("PASS");
    } else print("FAIL");
    print(": f_namemax=");
    print_num(st.f_namemax);
    print("\n");

    total++;
    if (st.f_blocks > 0 && st.f_bfree > 0) {
        pass++; print("PASS");
    } else print("FAIL");
    print(": blocks=");
    print_num(st.f_blocks);
    print(" free=");
    print_num(st.f_bfree);
    print("\n");

    total++;
    if (fstatvfs(0, &st) == 0 && st.f_bsize == 1024) {
        pass++; print("PASS");
    } else print("FAIL");
    print(": fstatvfs works\n");

    if (pass == total) print("ALL PASS\n");
    return 0;
}
