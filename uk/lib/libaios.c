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
long aios_lseek(int fd, long off, int whence) { return asys(AIOS_SYS_LSEEK, fd, off, whence); }
int  aios_fstat(int fd, struct aios_stat *st) { return (int)asys(AIOS_SYS_FSTAT, fd, (long)st, 0); }
void aios_exit(int code) { asys(AIOS_SYS_EXIT, code, 0, 0); for (;;) { } }
long aios_execve(const char *path, char *const argv[], char *const envp[]) {
    return asys(AIOS_SYS_EXEC, (long)path, (long)argv, (long)envp);
}
long aios_exec(const char *path, char *const argv[]) { return aios_execve(path, argv, environ); }
long aios_fork(void) { return asys(AIOS_SYS_FORK, 0, 0, 0); }
long aios_wait(int *status) { return asys(AIOS_SYS_WAIT, (long)AIOS_WAIT_ANY, (long)status, 0); }
long aios_waitpid(long pid, int *status, int flags) {
    return asys(AIOS_SYS_WAIT, pid, (long)status, flags);
}
int  aios_pipe(int fds[2]) { return (int)asys(AIOS_SYS_PIPE, (long)fds, 0, 0); }
long aios_dup2(int oldfd, int newfd) { return asys(AIOS_SYS_DUP2, oldfd, newfd, 0); }

/* --- string/memory --- */
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) break;
    }
    return 0;
}
char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    if (dd < ss) for (size_t i = 0; i < n; i++) dd[i] = ss[i];
    else         for (size_t i = n; i-- > 0; )  dd[i] = ss[i];
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *dd = d;
    for (size_t i = 0; i < n; i++) dd[i] = (unsigned char)c;
    return d;
}
int atoi(const char *s) {
    int sign = 1, v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return sign * v;
}

/* --- ctype --- */
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
int isdigit(int c) { return c >= '0' && c <= '9'; }

/* --- bump allocator over mmap'd regions (real heap; the AIOS kernel injects the host mmap).
 * free is still a no-op; when a region is exhausted we mmap another. Simple, but real memory. --- */
#define AIOS_MMAP_CHUNK (1024 * 1024)           /* grow the heap a MB at a time */
static unsigned char *g_brk;                    /* next free byte in the current region */
static unsigned char *g_brk_end;                /* end of the current region            */
void *malloc(size_t n) {
    n = (n + 15) & ~(size_t)15;                 /* 16-byte align */
    if (g_brk + n > g_brk_end) {
        size_t chunk = n > AIOS_MMAP_CHUNK ? n : AIOS_MMAP_CHUNK;
        chunk = (chunk + 4095) & ~(size_t)4095; /* page-align */
        long addr = asys(AIOS_SYS_MMAP, (long)chunk, 0, 0);
        if (addr == 0) return NULL;             /* out of memory */
        g_brk = (unsigned char *)addr;
        g_brk_end = g_brk + chunk;              /* note: any tail of the old region is dropped */
    }
    void *p = g_brk;
    g_brk += n;
    return p;
}
void free(void *p) { (void)p; }

/* --- output --- */
int puts(const char *s) { aios_write(AIOS_FD_STDOUT, s, strlen(s)); aios_write(AIOS_FD_STDOUT, "\n", 1); return 0; }
void fdputs(int fd, const char *s) { aios_write(fd, s, strlen(s)); }

/* The formatter writes to a "sink" -- either an fd (printf) or a bounded buffer (snprintf) -- so a
 * single core serves both. (FILE*-buffered stdio lands in the next step.) */
struct sink { char *buf; size_t cap; size_t len; int fd; };
static void sink_write(struct sink *s, const char *p, size_t n) {
    if (s->buf) { for (size_t i = 0; i < n; i++) if (s->len + i < s->cap) s->buf[s->len + i] = p[i]; }
    else if (n) aios_write(s->fd, p, n);
    s->len += n;
}
static void sink_uint(struct sink *s, unsigned long v, int base, int upper) {
    char b[24]; int i = sizeof b;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { b[--i] = dig[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    sink_write(s, &b[i], (size_t)(sizeof b - i));
}
static void sink_int(struct sink *s, long v) {
    if (v < 0) { sink_write(s, "-", 1); sink_uint(s, (unsigned long)(-v), 10, 0); }
    else         sink_uint(s, (unsigned long)v, 10, 0);
}

/* Minimal printf-family core: %s %d %i %u %x %X %o %c %p %% and the l/z length modifiers
 * (%ld %lu %lx %zu ...). No field width/precision yet -- grow as sbase/dash need it. */
static void vformat(struct sink *s, const char *fmt, va_list ap) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { sink_write(s, p, 1); continue; }
        p++;
        int lng = 0;
        while (*p == 'l' || *p == 'z') { lng = 1; p++; }   /* long / size_t width */
        switch (*p) {
        case 's': { const char *a = va_arg(ap, const char *); if (!a) a = "(null)"; sink_write(s, a, strlen(a)); break; }
        case 'd': case 'i': sink_int(s, lng ? va_arg(ap, long) : (long)va_arg(ap, int)); break;
        case 'u': sink_uint(s, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 10, 0); break;
        case 'x': sink_uint(s, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16, 0); break;
        case 'X': sink_uint(s, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16, 1); break;
        case 'o': sink_uint(s, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int),  8, 0); break;
        case 'p': sink_write(s, "0x", 2); sink_uint(s, (unsigned long)va_arg(ap, void *), 16, 0); break;
        case 'c': { char c = (char)va_arg(ap, int); sink_write(s, &c, 1); break; }
        case '%': sink_write(s, "%", 1); break;
        default:  sink_write(s, "%", 1); if (*p) sink_write(s, p, 1); break;
        }
        if (!*p) break;
    }
}

int printf(const char *fmt, ...) {
    struct sink s = { 0, 0, 0, AIOS_FD_STDOUT };
    va_list ap; va_start(ap, fmt); vformat(&s, fmt, ap); va_end(ap);
    return (int)s.len;
}
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct sink s = { buf, size, 0, -1 };
    vformat(&s, fmt, ap);
    if (size) buf[s.len < size ? s.len : size - 1] = '\0';
    return (int)s.len;
}
int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, size, fmt, ap); va_end(ap);
    return r;
}
int putchar(int c) { char b = (char)c; aios_write(AIOS_FD_STDOUT, &b, 1); return c; }

/* ===== POSIX libc surface =====
 * Standard-named entry points so real C sources compile UNMODIFIED against the shadow headers in
 * lib/include (-nostdinc). Thin wrappers over the aios_* core / the AIOS ABI. Definitions use plain
 * types (the shadow typedefs ssize_t/off_t/pid_t are long/long/int -- compatible). */

/* unistd / fcntl */
long read (int fd, void *b, unsigned long n)        { return aios_read(fd, b, n); }
long write(int fd, const void *b, unsigned long n)  { return aios_write(fd, b, n); }
int  close(int fd)                                  { return aios_close(fd); }
long lseek(int fd, long off, int whence)            { return aios_lseek(fd, off, whence); }
int  pipe (int fds[2])                              { return aios_pipe(fds); }
int  dup2 (int o, int n)                            { return (int)aios_dup2(o, n); }
int  fork (void)                                    { return (int)aios_fork(); }
int  execv (const char *p, char *const argv[])      { return (int)aios_execve(p, argv, environ); }
int  execvp(const char *f, char *const argv[])      { return (int)aios_execve(f, argv, environ); } /* no PATH search yet */
void _exit(int code)                                { aios_exit(code); }
int  getpid(void)  { return 1; }                    /* TODO: a real AIOS_SYS_GETPID */
int  isatty(int fd){ (void)fd; return 0; }          /* no tty layer yet */
int  open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & AIOS_O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return (int)aios_open(path, flags, mode);
}

/* sys/wait */
int wait(int *status)                          { return (int)aios_wait(status); }
int waitpid(int pid, int *status, int options) { return (int)aios_waitpid(pid, status, options); }

/* string extras */
char *strrchr(const char *s, int c) {
    const char *last = 0;
    for (;; s++) { if (*s == (char)c) last = s; if (!*s) break; }
    return (char *)last;
}
char *strstr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    for (; *h; h++) { const char *a = h, *b = n; while (*a && *b && *a == *b) { a++; b++; } if (!*b) return (char *)h; }
    return 0;
}
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = '\0';
    return d;
}
char *strcat(char *d, const char *s) { char *r = d; while (*d) d++; while ((*d++ = *s++)) ; return r; }
char *strncat(char *d, const char *s, size_t n) {
    char *r = d; while (*d) d++;
    while (n-- && *s) *d++ = *s++;
    *d = '\0'; return r;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++) if (p[i] == (unsigned char)c) return (void *)(p + i);
    return 0;
}
char *strdup(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }
char *strerror(int e) { (void)e; return "error"; }   /* minimal; real errno strings come with errno */

/* ctype extras */
int isalpha(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isprint(int c)  { return c >= 0x20 && c < 0x7f; }
int isgraph(int c)  { return c > 0x20 && c < 0x7f; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
int iscntrl(int c)  { return c < 0x20 || c == 0x7f; }
int isblank(int c)  { return c == ' ' || c == '\t'; }
int toupper(int c)  { return islower(c) ? c - 32 : c; }
int tolower(int c)  { return isupper(c) ? c + 32 : c; }

/* stdlib extras */
void  exit(int code)  { aios_exit(code); }
void  abort(void)     { aios_exit(134); }              /* 128 + SIGABRT */
void *calloc(size_t nmemb, size_t size) {
    size_t t = nmemb * size; void *p = malloc(t); if (p) memset(p, 0, t); return p;
}
void *realloc(void *old, size_t n) {                    /* bump heap: fresh block + copy (no shrink) */
    void *p = malloc(n); if (p && old) memcpy(p, old, n); return p;
}
long strtol(const char *s, char **end, int base) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0 && s[0] == '0') base = 8;
    else if (base == 0) base = 10;
    long v = 0;
    for (;; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtol(s, end, base); }
long atol(const char *s) { return strtol(s, 0, 10); }
char *getenv(const char *name) {
    size_t n = strlen(name);
    for (char **e = environ; e && *e; e++)
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return *e + n + 1;
    return 0;
}

/* --- runtime entry: _start lifts argc/argv off the stack, runs main, exits with its return --- */
extern int main(int argc, char **argv);
char **environ;                                   /* POSIX env; envp follows argv on the stack */
void __libaios_start(long argc, char **argv) {
    environ = argv + argc + 1;                    /* [argc][argv..][NULL][envp..] -> envp slot */
    aios_exit(main((int)argc, argv));
}

__asm__(
    ".global _start\n"
    "_start:\n"
    "  ldr x0, [sp]\n"        /* argc            */
    "  add x1, sp, #8\n"      /* argv (&argv[0]) */
    "  bl  __libaios_start\n"
    "1: b 1b\n"
);
