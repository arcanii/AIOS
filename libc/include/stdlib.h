#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);
void  exit(int status);
void  abort(void);
int   atoi(const char *str);
long  atol(const char *str);
long  strtol(const char *str, char **endptr, int base);
unsigned long strtoul(const char *str, char **endptr, int base);
int   abs(int n);
long  labs(long n);
void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#endif
