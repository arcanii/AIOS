/* stdlib.h -- AIOS shadow header (see sys/types.h). Implemented in libaios. */
#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>

void  *malloc(size_t n);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *p, size_t n);
void   free(void *p);
void   exit(int code) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));
int    atoi(const char *s);
long   atol(const char *s);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
int       abs(int v);
long      labs(long v);
long long llabs(long long v);
char  *getenv(const char *name);
void   qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void  *bsearch(const void *key, const void *base, size_t n, size_t size,
               int (*cmp)(const void *, const void *));

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#endif /* _STDLIB_H */
