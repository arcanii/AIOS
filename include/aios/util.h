#ifndef AIOS_UTIL_H
#define AIOS_UTIL_H

/*
 * AIOS shared utility functions
 *
 * Consolidates the my_memcpy, my_memset, my_strlen, my_strcmp
 * functions that were previously duplicated in every PD source file.
 *
 * PDs that need these should include this header and link util.o.
 * For PDs that cannot link util.o (e.g. sandbox), these are also
 * available as static inline in this header via AIOS_UTIL_INLINE.
 */

#include <stdint.h>

#ifdef AIOS_UTIL_INLINE

/* Inline versions for PDs that cannot link util.o */

static inline void *aios_memcpy(void *dst, const void *src, unsigned long n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static inline void *aios_memset(void *dst, int c, unsigned long n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

static inline int aios_strlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

static inline int aios_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline int aios_strncmp(const char *a, const char *b, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}

static inline int aios_memcmp(const void *a, const void *b, unsigned long n) {
    const uint8_t *aa = (const uint8_t *)a;
    const uint8_t *bb = (const uint8_t *)b;
    while (n--) { if (*aa != *bb) return *aa - *bb; aa++; bb++; }
    return 0;
}

static inline int aios_str_starts_with(const char *s, const char *prefix) {
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

#else

/* Extern declarations for PDs that link util.o */

void *aios_memcpy(void *dst, const void *src, unsigned long n);
void *aios_memset(void *dst, int c, unsigned long n);
int   aios_strlen(const char *s);
int   aios_strcmp(const char *a, const char *b);
int   aios_strncmp(const char *a, const char *b, unsigned long n);
int   aios_memcmp(const void *a, const void *b, unsigned long n);
int   aios_str_starts_with(const char *s, const char *prefix);

#endif /* AIOS_UTIL_INLINE */

/* Convenience macros that map the old my_ names to aios_ names.
 * New code should use the aios_ prefix directly. */
#define my_memcpy   aios_memcpy
#define my_memset   aios_memset
#define my_strlen   aios_strlen
#define my_strcmp    aios_strcmp
#define my_memcmp   aios_memcmp

#endif /* AIOS_UTIL_H */
