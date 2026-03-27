#include "aios.h"

int _start(aios_syscalls_t *sys) {
    int limit = 200;
    char *is_prime = (char *)malloc(limit);
    if (!is_prime) { puts("malloc failed!\n"); return 1; }
    memset(is_prime, 1, limit);
    is_prime[0] = is_prime[1] = 0;
    for (int i = 2; i * i < limit; i++)
        if (is_prime[i])
            for (int j = i * i; j < limit; j += i)
                is_prime[j] = 0;
    puts("Primes up to "); put_dec(limit); puts(":\n");
    int count = 0;
    for (int i = 2; i < limit; i++)
        if (is_prime[i]) {
            put_dec(i); putc(' '); count++;
            if (count % 15 == 0) putc('\n');
        }
    puts("\n\nTotal: "); put_dec(count); puts(" primes\n");
    free(is_prime);
    return 0;
}
