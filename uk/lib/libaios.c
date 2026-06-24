/*
 * libaios.c -- minimal C runtime for AIOS-ABI programs (see libaios.h).
 *
 * Everything here is built on the AIOS ABI (svc with AIOS syscall numbers) -- no host calls. It
 * provides _start (reads argc/argv off the stack, calls main, exits with its return), a few
 * string ops, a bump allocator, and a small printf. The seed of the AIOS-ABI libc.
 */
#include "libaios.h"
#include "aios_abi.h"
#include <stdarg.h>

/* --- the AIOS syscall instruction --- */
static long asys(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

long aios_open(const char *p, int fl, int mode) { return asys(AIOS_SYS_OPEN, (long)p, fl, mode); }
long aios_read(int fd, void *b, unsigned long n)  { return asys(AIOS_SYS_READ,  fd, (long)b, n); }
long aios_write(int fd, const void *b, unsigned long n){ return asys(AIOS_SYS_WRITE, fd, (long)b, n); }
int  aios_close(int fd) { return (int)asys(AIOS_SYS_CLOSE, fd, 0, 0); }
void aios_exit(int code) { asys(AIOS_SYS_EXIT, code, 0, 0); for (;;) { } }

/* --- string/memory --- */
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *dd = d;
    for (size_t i = 0; i < n; i++) dd[i] = (unsigned char)c;
    return d;
}

/* --- bump allocator over a static arena (no brk/mmap syscall yet; free is a no-op) --- */
#define AIOS_HEAP_SIZE (256 * 1024)
static unsigned char g_heap[AIOS_HEAP_SIZE];
static size_t g_heap_off;
void *malloc(size_t n) {
    n = (n + 15) & ~(size_t)15;                 /* 16-byte align */
    if (g_heap_off + n > AIOS_HEAP_SIZE) return NULL;
    void *p = &g_heap[g_heap_off];
    g_heap_off += n;
    return p;
}
void free(void *p) { (void)p; }

/* --- output --- */
int puts(const char *s) { aios_write(AIOS_FD_STDOUT, s, strlen(s)); aios_write(AIOS_FD_STDOUT, "\n", 1); return 0; }

static void emit(const char *s, size_t n) { aios_write(AIOS_FD_STDOUT, s, n); }
static void emit_uint(unsigned long v, int base) {
    char b[24]; int i = sizeof b;
    const char *dig = "0123456789abcdef";
    b[--i] = '\0';
    do { b[--i] = dig[v % base]; v /= base; } while (v);
    emit(&b[i], (size_t)(sizeof b - 1 - i));
}
static void emit_int(long v) {
    if (v < 0) { emit("-", 1); emit_uint((unsigned long)(-v), 10); }
    else         emit_uint((unsigned long)v, 10);
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { emit(p, 1); continue; }
        switch (*++p) {
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; emit(s, strlen(s)); break; }
        case 'd': case 'i': emit_int(va_arg(ap, int)); break;
        case 'u': emit_uint((unsigned)va_arg(ap, unsigned int), 10); break;
        case 'x': emit_uint((unsigned)va_arg(ap, unsigned int), 16); break;
        case 'c': { char c = (char)va_arg(ap, int); emit(&c, 1); break; }
        case '%': emit("%", 1); break;
        default:  emit("%", 1); if (*p) emit(p, 1); break;
        }
        if (!*p) break;
    }
    va_end(ap);
    return 0;
}

/* --- runtime entry: _start lifts argc/argv off the stack, runs main, exits with its return --- */
extern int main(int argc, char **argv);
void __libaios_start(long argc, char **argv) { aios_exit(main((int)argc, argv)); }

__asm__(
    ".global _start\n"
    "_start:\n"
    "  ldr x0, [sp]\n"        /* argc            */
    "  add x1, sp, #8\n"      /* argv (&argv[0]) */
    "  bl  __libaios_start\n"
    "1: b 1b\n"
);
