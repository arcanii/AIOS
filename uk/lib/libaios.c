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

int errno;   /* POSIX errno; set by the standard-named wrappers (see __ret) and read by perror */
char *strerror(int errnum);   /* defined below; declared early so perror (stdio) can use it */

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

/* --- FILE* buffered stdio ---
 * A FILE wraps an fd with one buffer. stdout is LINE-buffered (flushes at '\n' -- which also keeps
 * it empty between lines, so fork doesn't duplicate pending output); stderr is unbuffered; opened
 * files are fully buffered. Output is flushed on exit / main-return (see exit + __libaios_start).
 * Minimal: a FILE is primarily a read OR a write stream (r+/w+ work but not finely interleaved). */
#ifndef _IOFBF
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#endif
#ifndef EOF
#define EOF (-1)
#endif
#define AIOS_BUFSIZ 1024

struct _IO_FILE {
    int    fd;
    int    rd, wr;          /* opened for reading / writing */
    int    eof, err;
    int    bufmode;         /* _IOFBF / _IOLBF / _IONBF */
    int    is_static;       /* stdin/stdout/stderr -- never freed */
    unsigned char *buf;
    size_t cap;             /* buffer capacity */
    size_t pos;             /* read: cursor; write: pending byte count */
    size_t end;             /* read: count of valid bytes in buf */
};
typedef struct _IO_FILE FILE;

static unsigned char _sin_buf[AIOS_BUFSIZ], _sout_buf[AIOS_BUFSIZ];
static FILE _stdin  = { .fd = 0, .rd = 1, .bufmode = _IOFBF, .is_static = 1, .buf = _sin_buf,  .cap = AIOS_BUFSIZ };
static FILE _stdout = { .fd = 1, .wr = 1, .bufmode = _IOLBF, .is_static = 1, .buf = _sout_buf, .cap = AIOS_BUFSIZ };
static FILE _stderr = { .fd = 2, .wr = 1, .bufmode = _IONBF, .is_static = 1 };
FILE *stdin = &_stdin, *stdout = &_stdout, *stderr = &_stderr;

int fflush(FILE *f) {
    if (!f) return fflush(stdout);
    if (f->wr && f->pos > 0) {
        long w = aios_write(f->fd, f->buf, f->pos);
        if (w < 0) { f->err = 1; return EOF; }
        f->pos = 0;
    }
    return 0;
}
int fputc(int c, FILE *f) {
    unsigned char b = (unsigned char)c;
    if (!f->buf || f->bufmode == _IONBF) { aios_write(f->fd, &b, 1); return c; }
    f->buf[f->pos++] = b;
    if (f->pos >= f->cap || (f->bufmode == _IOLBF && b == '\n')) fflush(f);
    return c;
}
size_t fwrite(const void *ptr, size_t sz, size_t nm, FILE *f) {
    size_t total = sz * nm; const unsigned char *s = ptr;
    if (!f->buf || f->bufmode == _IONBF) { if (total) aios_write(f->fd, s, total); return sz ? nm : 0; }
    size_t done = 0; int nl = 0;
    while (done < total) {
        if (f->pos >= f->cap) fflush(f);
        size_t space = f->cap - f->pos, chunk = total - done < space ? total - done : space;
        memcpy(f->buf + f->pos, s + done, chunk);
        if (f->bufmode == _IOLBF)
            for (size_t i = 0; i < chunk; i++) if (s[done + i] == '\n') { nl = 1; break; }
        f->pos += chunk; done += chunk;
    }
    if (nl) fflush(f);
    return sz ? nm : 0;
}
static int _refill(FILE *f) {
    if (!f->rd || !f->buf) return -1;
    long n = aios_read(f->fd, f->buf, f->cap);
    if (n < 0) { f->err = 1; return -1; }
    if (n == 0) { f->eof = 1; return 0; }
    f->pos = 0; f->end = (size_t)n; return (int)n;
}
int fgetc(FILE *f) {
    if (f->pos >= f->end) { if (_refill(f) <= 0) return EOF; }
    return f->buf[f->pos++];
}
size_t fread(void *ptr, size_t sz, size_t nm, FILE *f) {
    size_t total = sz * nm, done = 0; unsigned char *d = ptr;
    while (done < total) { int c = fgetc(f); if (c == EOF) break; d[done++] = (unsigned char)c; }
    return sz ? done / sz : 0;
}
int fputs(const char *s, FILE *f) { fwrite(s, 1, strlen(s), f); return 0; }
char *fgets(char *s, int n, FILE *f) {
    int i = 0;
    while (i < n - 1) { int c = fgetc(f); if (c == EOF) break; s[i++] = (char)c; if (c == '\n') break; }
    if (i == 0) return 0;                            /* EOF with nothing read */
    s[i] = '\0';
    return s;
}
int  getchar(void)        { return fgetc(stdin); }
int  feof(FILE *f)        { return f->eof; }
int  ferror(FILE *f)      { return f->err; }
void clearerr(FILE *f)    { f->eof = f->err = 0; }
int  fileno(FILE *f)      { return f->fd; }

FILE *fopen(const char *path, const char *mode) {
    int flags, rd = 0, wr = 0;
    if (mode[0] == 'r')      { flags = AIOS_O_RDONLY; rd = 1;
                               if (mode[1]=='+'){ flags = AIOS_O_RDWR; wr = 1; } }
    else if (mode[0] == 'w') { flags = AIOS_O_WRONLY|AIOS_O_CREAT|AIOS_O_TRUNC; wr = 1;
                               if (mode[1]=='+'){ flags = AIOS_O_RDWR|AIOS_O_CREAT|AIOS_O_TRUNC; rd = 1; } }
    else if (mode[0] == 'a') { flags = AIOS_O_WRONLY|AIOS_O_CREAT|AIOS_O_APPEND; wr = 1;
                               if (mode[1]=='+'){ flags = AIOS_O_RDWR|AIOS_O_CREAT|AIOS_O_APPEND; rd = 1; } }
    else return 0;
    long fd = aios_open(path, flags, 0644);
    if (fd < 0) return 0;
    FILE *f = malloc(sizeof *f);
    if (!f) { aios_close((int)fd); return 0; }
    memset(f, 0, sizeof *f);
    f->fd = (int)fd; f->rd = rd; f->wr = wr; f->bufmode = _IOFBF;
    f->buf = malloc(AIOS_BUFSIZ); f->cap = f->buf ? AIOS_BUFSIZ : 0;
    if (!f->buf) f->bufmode = _IONBF;
    return f;
}
int fclose(FILE *f) {
    if (!f) return EOF;
    fflush(f);
    int fd = f->fd;
    if (!f->is_static) { free(f->buf); free(f); }   /* free is a no-op today; FILEs are not reclaimed */
    return aios_close(fd);
}
void perror(const char *s) {
    if (s && *s) { fputs(s, stderr); fputs(": ", stderr); }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

/* --- output --- */
int puts(const char *s) { fputs(s, stdout); fputc('\n', stdout); return 0; }
void fdputs(int fd, const char *s) { aios_write(fd, s, strlen(s)); }

/* The formatter writes to a "sink" -- either a FILE (printf/fprintf, buffered) or a bounded buffer
 * (snprintf) -- so a single core serves both. */
struct sink { char *buf; size_t cap; size_t len; FILE *fp; };
static void sink_write(struct sink *s, const char *p, size_t n) {
    if (s->buf) { for (size_t i = 0; i < n; i++) if (s->len + i < s->cap) s->buf[s->len + i] = p[i]; }
    else if (s->fp && n) fwrite(p, 1, n, s->fp);
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
    struct sink s = { .fp = stdout };
    va_list ap; va_start(ap, fmt); vformat(&s, fmt, ap); va_end(ap);
    return (int)s.len;
}
int vfprintf(FILE *f, const char *fmt, va_list ap) {
    struct sink s = { .fp = f };
    vformat(&s, fmt, ap);
    return (int)s.len;
}
int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap);
    return r;
}
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct sink s = { .buf = buf, .cap = size };
    vformat(&s, fmt, ap);
    if (size) buf[s.len < size ? s.len : size - 1] = '\0';
    return (int)s.len;
}
int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, size, fmt, ap); va_end(ap);
    return r;
}
int putchar(int c) { return fputc(c, stdout); }

/* ===== POSIX libc surface =====
 * Standard-named entry points so real C sources compile UNMODIFIED against the shadow headers in
 * lib/include (-nostdinc). Thin wrappers over the aios_* core / the AIOS ABI. Definitions use plain
 * types (the shadow typedefs ssize_t/off_t/pid_t are long/long/int -- compatible). */

/* A failing syscall returns a negated AIOS error code (-errno); these wrappers translate that to
 * the POSIX contract: set `errno`, return -1. (The lower-level aios_* functions return the raw
 * value.) `errno` itself (defined near the top) is what the shadow <errno.h> declares extern. */
static long __ret(long r) { if (AIOS_IS_ERR(r)) { errno = (int)-r; return -1; } return r; }

/* unistd / fcntl */
long read (int fd, void *b, unsigned long n)        { return __ret(aios_read(fd, b, n)); }
long write(int fd, const void *b, unsigned long n)  { return __ret(aios_write(fd, b, n)); }
int  close(int fd)                                  { return (int)__ret(aios_close(fd)); }
long lseek(int fd, long off, int whence)            { return __ret(aios_lseek(fd, off, whence)); }
int  pipe (int fds[2])                              { return (int)__ret(aios_pipe(fds)); }
int  dup2 (int o, int n)                            { return (int)__ret(aios_dup2(o, n)); }
int  fork (void)                                    { return (int)__ret(aios_fork()); }
int  execv (const char *p, char *const argv[])      { return (int)__ret(aios_execve(p, argv, environ)); }
int  execvp(const char *f, char *const argv[])      { return (int)__ret(aios_execve(f, argv, environ)); } /* no PATH yet */
void _exit(int code)                                { aios_exit(code); }
int  getpid(void)  { return (int)asys(AIOS_SYS_GETPID, 0, 0, 0); }
int  isatty(int fd){ (void)fd; return 0; }          /* no tty layer yet */
int  open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & AIOS_O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return (int)__ret(aios_open(path, flags, mode));
}

/* filesystem namespace (sys/stat.h + unistd.h + stdio.h). The stat pointer is forwarded straight to
 * the kernel, which fills it -- libaios never touches the struct (the program's `struct stat` ==
 * the kernel's `struct aios_stat`, byte for byte), so a void* keeps libaios independent of it. */
int  stat (const char *path, void *st) { return (int)__ret(asys(AIOS_SYS_STAT,  (long)path, (long)st, 0)); }
int  lstat(const char *path, void *st) { return (int)__ret(asys(AIOS_SYS_LSTAT, (long)path, (long)st, 0)); }
int  fstat(int fd, void *st)           { return (int)__ret(asys(AIOS_SYS_FSTAT, fd, (long)st, 0)); }
int  unlink(const char *path)          { return (int)__ret(asys(AIOS_SYS_UNLINK, (long)path, 0, 0)); }
int  rmdir (const char *path)          { return (int)__ret(asys(AIOS_SYS_RMDIR,  (long)path, 0, 0)); }
int  chdir (const char *path)          { return (int)__ret(asys(AIOS_SYS_CHDIR,  (long)path, 0, 0)); }
int  mkdir (const char *path, unsigned int mode)  { return (int)__ret(asys(AIOS_SYS_MKDIR, (long)path, (long)mode, 0)); }
int  rename(const char *o, const char *n)         { return (int)__ret(asys(AIOS_SYS_RENAME, (long)o, (long)n, 0)); }
int  remove(const char *path) {                   /* POSIX: unlink a file, or rmdir a directory */
    int r = unlink(path);
    if (r != 0 && errno == AIOS_EISDIR) r = rmdir(path);
    return r;
}
char *getcwd(char *buf, unsigned long size) {
    long r = asys(AIOS_SYS_GETCWD, (long)buf, (long)size, 0);
    if (AIOS_IS_ERR(r)) { errno = (int)-r; return 0; }
    return buf;
}

/* sys/wait */
int wait(int *status)                          { return (int)__ret(aios_wait(status)); }
int waitpid(int pid, int *status, int options) { return (int)__ret(aios_waitpid(pid, status, options)); }

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
char *strerror(int e) {
    switch (e) {
    case 0:                   return "Success";
    case AIOS_EPERM:          return "Operation not permitted";
    case AIOS_ENOENT:         return "No such file or directory";
    case AIOS_ESRCH:          return "No such process";
    case AIOS_EINTR:          return "Interrupted system call";
    case AIOS_EIO:            return "Input/output error";
    case AIOS_EBADF:          return "Bad file descriptor";
    case AIOS_ECHILD:         return "No child processes";
    case AIOS_EAGAIN:         return "Resource temporarily unavailable";
    case AIOS_ENOMEM:         return "Cannot allocate memory";
    case AIOS_EACCES:         return "Permission denied";
    case AIOS_EFAULT:         return "Bad address";
    case AIOS_EBUSY:          return "Device or resource busy";
    case AIOS_EEXIST:         return "File exists";
    case AIOS_ENOTDIR:        return "Not a directory";
    case AIOS_EISDIR:         return "Is a directory";
    case AIOS_EINVAL:         return "Invalid argument";
    case AIOS_EMFILE:         return "Too many open files";
    case AIOS_ESPIPE:         return "Illegal seek";
    case AIOS_EPIPE:          return "Broken pipe";
    case AIOS_ERANGE:         return "Numerical result out of range";
    case AIOS_ENAMETOOLONG:   return "File name too long";
    case AIOS_ENOSYS:         return "Function not implemented";
    default:                  return "Unknown error";
    }
}

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
void  exit(int code)  { fflush(stdout); aios_exit(code); }   /* flush buffered stdout like libc */
void  abort(void)     { fflush(stdout); aios_exit(134); }    /* 128 + SIGABRT */
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

/* --- qsort: generic in-place heapsort (O(n log n) worst case, no recursion, no aux allocation).
 * sbase's sort + ls sort their lines/entries through this; heapsort keeps it allocation-free. --- */
static void aios_qswap(char *a, char *b, size_t sz) {
    while (sz--) { char t = *a; *a++ = *b; *b++ = t; }
}
static void aios_sift(char *base, size_t root, size_t n, size_t sz,
                      int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= n) break;
        if (child + 1 < n && cmp(base + child * sz, base + (child + 1) * sz) < 0) child++;
        if (cmp(base + root * sz, base + child * sz) >= 0) break;
        aios_qswap(base + root * sz, base + child * sz, sz);
        root = child;
    }
}
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    char *b = base;
    if (n < 2 || sz == 0) return;
    for (size_t i = n / 2; i-- > 0; ) aios_sift(b, i, n, sz, cmp);   /* build a max-heap */
    for (size_t end = n; --end > 0; ) {                             /* pop the max into the tail */
        aios_qswap(b, b + end * sz, sz);
        aios_sift(b, 0, end, sz, cmp);
    }
}

/* --- getopt: POSIX single-shot option parsing (no GNU permutation -- stops at the first
 * non-option). sbase parses args via its own arg.h, but getopt is part of the libc surface and
 * plenty of programs lean on it. State lives in the standard globals. --- */
char *optarg;
int   optind = 1, opterr = 1, optopt;
int getopt(int argc, char *const argv[], const char *optstring) {
    static int optpos = 1;                        /* char index within a clustered "-abc" group */
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || argv[optind][1] == '\0')
        return -1;                                /* no (more) options */
    if (argv[optind][1] == '-' && argv[optind][2] == '\0') { optind++; return -1; }  /* "--" ends */
    int c = (unsigned char)argv[optind][optpos];
    const char *o = strchr(optstring, c);
    if (c == ':' || !o) {                         /* unknown option */
        optopt = c;
        if (opterr && optstring[0] != ':') fprintf(stderr, "%s: illegal option -- %c\n", argv[0], c);
        if (argv[optind][++optpos] == '\0') { optind++; optpos = 1; }
        return '?';
    }
    if (o[1] == ':') {                            /* this option takes an argument */
        if (argv[optind][optpos + 1] != '\0') { optarg = &argv[optind][optpos + 1]; optind++; }
        else if (optind + 1 < argc)            { optarg = argv[optind + 1];        optind += 2; }
        else {                                    /* argument missing */
            optopt = c; optind++; optpos = 1;
            if (optstring[0] == ':') return ':';
            if (opterr) fprintf(stderr, "%s: option requires an argument -- %c\n", argv[0], c);
            return '?';
        }
        optpos = 1;
        return c;
    }
    if (argv[optind][++optpos] == '\0') { optind++; optpos = 1; }   /* simple flag */
    return c;
}

/* --- runtime entry: _start lifts argc/argv off the stack, runs main, exits with its return --- */
extern int main(int argc, char **argv);
char **environ;                                   /* POSIX env; envp follows argv on the stack */
void __libaios_start(long argc, char **argv) {
    environ = argv + argc + 1;                    /* [argc][argv..][NULL][envp..] -> envp slot */
    int rc = main((int)argc, argv);
    fflush(stdout);                               /* flush buffered output on main-return, like libc */
    aios_exit(rc);
}

__asm__(
    ".global _start\n"
    "_start:\n"
    "  ldr x0, [sp]\n"        /* argc            */
    "  add x1, sp, #8\n"      /* argv (&argv[0]) */
    "  bl  __libaios_start\n"
    "1: b 1b\n"
);
