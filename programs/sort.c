#include "aios.h"

int _start(aios_syscalls_t *sys) {
    int n = 16;
    int *arr = (int *)malloc(n * sizeof(int));
    if (!arr) { puts("malloc failed!\n"); return 1; }
    unsigned int seed = 12345;
    puts("Unsorted: ");
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245 + 12345;
        arr[i] = (seed >> 16) & 0xFF;
        put_dec(arr[i]); putc(' ');
    }
    putc('\n');
    for (int i = 0; i < n - 1; i++)
        for (int j = 0; j < n - 1 - i; j++)
            if (arr[j] > arr[j+1]) {
                int tmp = arr[j]; arr[j] = arr[j+1]; arr[j+1] = tmp;
            }
    puts("Sorted:   ");
    for (int i = 0; i < n; i++) { put_dec(arr[i]); putc(' '); }
    putc('\n');
    free(arr);
    return 0;
}
