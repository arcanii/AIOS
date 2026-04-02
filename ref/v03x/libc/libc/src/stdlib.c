#include <stdlib.h>
#include <string.h>

/* Simple bump allocator — will be replaced with proper malloc */
static char _heap[4 * 1024 * 1024];  /* 4 MiB heap */
static size_t _heap_used = 0;

void *malloc(size_t size) {
    /* Align to 16 bytes */
    size = (size + 15) & ~15UL;
    if (_heap_used + size > sizeof(_heap)) return (void *)0;
    void *ptr = &_heap[_heap_used];
    _heap_used += size;
    return ptr;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    /* Simple: always allocate new, can't free old */
    void *new = malloc(size);
    if (new && ptr) memcpy(new, ptr, size); /* might overread, but safe for bump */
    return new;
}

void free(void *ptr) {
    (void)ptr; /* bump allocator: no-op */
}

int atoi(const char *str) {
    return (int)strtol(str, (void *)0, 10);
}

long atol(const char *str) {
    return strtol(str, (void *)0, 10);
}

long strtol(const char *str, char **endptr, int base) {
    long result = 0;
    int neg = 0;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { neg = 1; str++; }
    else if (*str == '+') str++;

    if (base == 0) {
        if (*str == '0' && (str[1] == 'x' || str[1] == 'X')) { base = 16; str += 2; }
        else if (*str == '0') { base = 8; str++; }
        else base = 10;
    } else if (base == 16 && *str == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }

    while (*str) {
        int digit;
        if (*str >= '0' && *str <= '9') digit = *str - '0';
        else if (*str >= 'a' && *str <= 'f') digit = *str - 'a' + 10;
        else if (*str >= 'A' && *str <= 'F') digit = *str - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        str++;
    }
    if (endptr) *endptr = (char *)str;
    return neg ? -result : result;
}

unsigned long strtoul(const char *str, char **endptr, int base) {
    return (unsigned long)strtol(str, endptr, base);
}

int abs(int n) { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    /* Simple insertion sort — fine for small arrays */
    char *b = (char *)base;
    char tmp[256]; /* max element size */
    if (size > sizeof(tmp)) return;
    for (size_t i = 1; i < nmemb; i++) {
        memcpy(tmp, b + i * size, size);
        size_t j = i;
        while (j > 0 && compar(b + (j-1) * size, tmp) > 0) {
            memcpy(b + j * size, b + (j-1) * size, size);
            j--;
        }
        memcpy(b + j * size, tmp, size);
    }
}

void exit(int status) {
    /* TODO: PPC to proc_server */
    (void)status;
    for (;;) __asm__ volatile("wfi");
}

void abort(void) {
    exit(127);
}
