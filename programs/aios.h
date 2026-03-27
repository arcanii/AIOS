/* aios.h — AIOS sandbox program interface */
#ifndef AIOS_H
#define AIOS_H

typedef unsigned long size_t;

typedef struct {
    void (*puts)(const char *s);
    void (*putc)(char c);
    void (*put_dec)(unsigned int n);
    void (*put_hex)(unsigned int n);
    void *(*malloc)(size_t size);
    void  (*free)(void *ptr);
    void *(*memcpy)(void *dst, const void *src, size_t n);
    void *(*memset)(void *dst, int c, size_t n);
    int   (*strlen)(const char *s);
    int   (*strcmp)(const char *a, const char *b);
    char *(*strcpy)(char *dst, const char *src);
    char *(*strncpy)(char *dst, const char *src, size_t n);
} aios_syscalls_t;

/* Convenience macros — use after: aios_syscalls_t *sys = ...; */
#define puts(s)         sys->puts(s)
#define putc(c)         sys->putc(c)
#define put_dec(n)      sys->put_dec(n)
#define put_hex(n)      sys->put_hex(n)
#define malloc(sz)      sys->malloc(sz)
#define free(p)         sys->free(p)
#define memcpy(d,s,n)   sys->memcpy(d,s,n)
#define memset(d,c,n)   sys->memset(d,c,n)
#define strlen(s)       sys->strlen(s)
#define strcmp(a,b)      sys->strcmp(a,b)
#define strcpy(d,s)     sys->strcpy(d,s)
#define strncpy(d,s,n)  sys->strncpy(d,s,n)

#define NULL ((void *)0)

#endif
