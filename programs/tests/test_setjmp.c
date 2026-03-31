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
    jmp_buf env;
    int val = setjmp(env);
    if (val == 0) {
        print("setjmp returned 0 (first call)\n");
        print("calling longjmp with val=42...\n");
        longjmp(env, 42);
        print("ERROR: should not reach here\n");
    } else {
        print("setjmp returned ");
        print_num((unsigned long)val);
        print(" (after longjmp)\n");
    }

    val = setjmp(env);
    if (val == 0) {
        print("testing longjmp(env, 0)...\n");
        longjmp(env, 0);
    } else {
        print("longjmp(env,0) returned ");
        print_num((unsigned long)val);
        print(" (should be 1)\n");
    }

    print("PASS: setjmp/longjmp working\n");
    return 0;
}
