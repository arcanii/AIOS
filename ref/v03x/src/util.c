/*
 * AIOS shared utility functions
 *
 * Compiled once, linked into PDs that need basic memory/string ops.
 * See include/aios/util.h for inline versions and documentation.
 */
#include <stdint.h>

void *aios_memcpy(void *dst, const void *src, unsigned long n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *aios_memset(void *dst, int c, unsigned long n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

int aios_strlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

int aios_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int aios_strncmp(const char *a, const char *b, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}

int aios_memcmp(const void *a, const void *b, unsigned long n) {
    const uint8_t *aa = (const uint8_t *)a;
    const uint8_t *bb = (const uint8_t *)b;
    while (n--) { if (*aa != *bb) return *aa - *bb; aa++; bb++; }
    return 0;
}

int aios_str_starts_with(const char *s, const char *prefix) {
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}
