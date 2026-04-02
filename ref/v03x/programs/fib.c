#include "aios.h"

int _start(aios_syscalls_t *sys) {
    puts("Fibonacci sequence (first 20 terms):\n");
    unsigned int a = 0, b = 1;
    for (int i = 0; i < 20; i++) {
        puts("  fib("); put_dec(i); puts(") = "); put_dec(a); putc('\n');
        unsigned int next = a + b; a = b; b = next;
    }
    puts("\nDone.\n");
    return 0;
}
