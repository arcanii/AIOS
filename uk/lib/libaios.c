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
/* 4-argument variant (the *at family: dirfd, path, flags/statbuf, mode/flags). */
static long asys4(long nr, long a0, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory", "cc");
    return x0;
}

int errno;   /* POSIX errno; set by the standard-named wrappers (see __ret) and read by perror */
char *strerror(int errnum);   /* defined below; declared early so perror (stdio) can use it */
int tolower(int c), toupper(int c);   /* defined below; used earlier by strcasecmp (gcc14: no implicit decls) */

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
/* Convert an unsigned value to digits (most-significant first) in `out`; returns the digit count. */
static size_t u2buf(char *out, unsigned long v, int base, int upper) {
    char tmp[24]; int i = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[i++] = dig[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    for (int j = 0; j < i; j++) out[j] = tmp[i - 1 - j];
    return (size_t)i;
}
static void sink_pad(struct sink *s, int n, char c) { while (n-- > 0) sink_write(s, &c, 1); }

/* printf-family core: flags (- 0), width (incl. *), precision (incl. .*), the l/z/h length
 * modifiers, and %s %d %i %u %x %X %o %c %p %%. Enough for sbase (ls -l columns align) + dash. */
static void vformat(struct sink *s, const char *fmt, va_list ap) {
    char digits[24];
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { sink_write(s, p, 1); continue; }
        p++;
        int left = 0, zero = 0;
        for (;; p++) {                                       /* flags */
            if      (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == '+' || *p == ' ' || *p == '#') { /* accepted, ignored */ }
            else break;
        }
        int width = 0;                                       /* field width */
        if (*p == '*') { width = va_arg(ap, int); p++; if (width < 0) { left = 1; width = -width; } }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int prec = -1;                                       /* precision */
        if (*p == '.') {
            p++; prec = 0;
            if (*p == '*') { prec = va_arg(ap, int); p++; if (prec < 0) prec = -1; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        int lng = 0;                                         /* length modifiers */
        while (*p == 'l' || *p == 'z' || *p == 'h') { if (*p == 'l' || *p == 'z') lng = 1; p++; }

        const char *body = digits; size_t blen = 0;          /* the value text (digits or string) */
        char prefix[2]; int plen = 0;                        /* sign / 0x -- emitted before zero-fill */
        int isnum = 0;
        switch (*p) {
        case 's': { const char *a = va_arg(ap, const char *); if (!a) a = "(null)";
                    blen = strlen(a); if (prec >= 0 && (size_t)prec < blen) blen = (size_t)prec;
                    body = a; break; }
        case 'c': digits[0] = (char)va_arg(ap, int); blen = 1; break;
        case 'd': case 'i': { long v = lng ? va_arg(ap, long) : (long)va_arg(ap, int);
                    unsigned long uv;
                    if (v < 0) { prefix[0] = '-'; plen = 1; uv = (unsigned long)(-v); } else uv = (unsigned long)v;
                    blen = u2buf(digits, uv, 10, 0); isnum = 1; break; }
        case 'u': blen = u2buf(digits, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 10, 0); isnum = 1; break;
        case 'x': blen = u2buf(digits, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16, 0); isnum = 1; break;
        case 'X': blen = u2buf(digits, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16, 1); isnum = 1; break;
        case 'o': blen = u2buf(digits, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int),  8, 0); isnum = 1; break;
        case 'p': prefix[0] = '0'; prefix[1] = 'x'; plen = 2;
                  blen = u2buf(digits, (unsigned long)(uintptr_t)va_arg(ap, void *), 16, 0); isnum = 1; break;
        case '%': digits[0] = '%'; blen = 1; break;
        case '\0': p--; continue;                            /* trailing '%' -> stop cleanly */
        default:  digits[0] = '%'; digits[1] = *p; blen = 2; break;
        }

        int zfill = 0;                                       /* numeric precision = min digit count */
        if (isnum && prec >= 0) { if ((size_t)prec > blen) zfill = (int)((size_t)prec - blen); zero = 0; }
        int content = plen + zfill + (int)blen;
        int padw = width > content ? width - content : 0;
        int usezero = zero && isnum;                         /* '0' flag never pads strings */

        if (!left && !usezero) sink_pad(s, padw, ' ');       /* right-justify, space pad */
        if (plen) sink_write(s, prefix, (size_t)plen);       /* sign / 0x before any zero-fill */
        if (!left && usezero)  sink_pad(s, padw, '0');       /* right-justify, zero pad */
        sink_pad(s, zfill, '0');                             /* precision zeros */
        sink_write(s, body, blen);
        if (left) sink_pad(s, padw, ' ');                    /* left-justify, trailing spaces */
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
int  execve(const char *p, char *const argv[], char *const envp[]) { return (int)__ret(aios_execve(p, argv, envp)); }
int  vfork(void)                                    { return (int)__ret(aios_fork()); }  /* fork: separate AS (safe) */
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
/* umask: tracked in the libc (mkdir/chmod apply it in userspace before the syscall). NOTE: the
 * kernel does NOT yet enforce a per-guest umask on create -- the host's umask still applies on the
 * real open/mkdir -- so this is advisory for kernel-applied masking. Per-guest umask is future
 * kernel work. */
static unsigned int __aios_umask = 022;
unsigned int umask(unsigned int m) { unsigned int old = __aios_umask; __aios_umask = m & 0777; return old; }

/* the *at family (path resolved relative to a dir fd, or AT_FDCWD). The recurse-based utilities
 * (rm, ls, cp) walk a tree through these. */
int openat(int dirfd, const char *path, int flags, ...) {
    int mode = 0;
    if (flags & AIOS_O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return (int)__ret(asys4(AIOS_SYS_OPENAT, dirfd, (long)path, flags, mode));
}
int fstatat(int dirfd, const char *path, void *st, int flags) {
    return (int)__ret(asys4(AIOS_SYS_FSTATAT, dirfd, (long)path, (long)st, flags));
}
int unlinkat(int dirfd, const char *path, int flags) {
    return (int)__ret(asys(AIOS_SYS_UNLINKAT, dirfd, (long)path, flags));
}
int faccessat(int dirfd, const char *path, int amode, int flags) {
    (void)flags;                                      /* AT_EACCESS not modelled */
    return (int)__ret(asys(AIOS_SYS_FACCESSAT, dirfd, (long)path, amode));
}
int access(const char *path, int amode) { return faccessat(AIOS_AT_FDCWD, path, amode, 0); }
long readlink(const char *path, char *buf, unsigned long bufsize) {
    return __ret(asys(AIOS_SYS_READLINK, (long)path, (long)buf, (long)bufsize));
}
int fcntl(int fd, int cmd, ...) {                  /* variadic arg used by F_DUPFD/F_SETFD/F_SETFL */
    va_list ap; va_start(ap, cmd); long arg = va_arg(ap, long); va_end(ap);
    return (int)__ret(asys(AIOS_SYS_FCNTL, fd, cmd, arg));
}
int dup(int fd) { return (int)__ret(asys(AIOS_SYS_FCNTL, fd, AIOS_F_DUPFD, 0)); }
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

/* --- directory streams (opendir/readdir/closedir) over AIOS_SYS_GETDENTS ---
 * A DIR wraps a directory fd + a buffer of raw aios_dirent records; readdir refills via GETDENTS
 * and hands back one fixed `struct dirent` at a time. `struct dirent` and `struct _AIOS_DIR` here
 * MUST stay byte-identical to the shadow <dirent.h> copies (the program reads the bytes readdir
 * fills) -- the same discipline as struct aios_stat <-> struct stat. */
struct dirent {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[256];
};
struct _AIOS_DIR {
    int           fd;
    int           pos;                 /* cursor into buf (bytes consumed) */
    int           len;                 /* valid bytes in buf */
    struct dirent de;                  /* the entry returned by the last readdir */
    unsigned char buf[4096];           /* raw aios_dirent records from the last GETDENTS */
};
typedef struct _AIOS_DIR DIR;

DIR *opendir(const char *path) {
    long fd = aios_open(path, AIOS_O_RDONLY, 0);
    if (fd < 0) { errno = (int)-fd; return 0; }
    struct aios_stat st;                                 /* reject a non-directory up front (ENOTDIR) */
    if (aios_fstat((int)fd, &st) == 0 && (st.st_mode & AIOS_S_IFMT) != AIOS_S_IFDIR) {
        aios_close((int)fd); errno = AIOS_ENOTDIR; return 0;
    }
    DIR *d = malloc(sizeof *d);
    if (!d) { aios_close((int)fd); errno = AIOS_ENOMEM; return 0; }
    d->fd = (int)fd; d->pos = 0; d->len = 0;
    return d;
}
struct dirent *readdir(DIR *d) {
    if (!d) { errno = AIOS_EBADF; return 0; }
    if (d->pos >= d->len) {                              /* buffer drained -> refill from the kernel */
        long n = asys(AIOS_SYS_GETDENTS, d->fd, (long)d->buf, (long)sizeof d->buf);
        if (n <= 0) { if (AIOS_IS_ERR(n)) errno = (int)-n; return 0; }   /* 0 = end, <0 = error */
        d->len = (int)n; d->pos = 0;
    }
    struct aios_dirent *ad = (struct aios_dirent *)(d->buf + d->pos);
    d->pos += ad->d_reclen;
    d->de.d_ino    = ad->d_ino;
    d->de.d_off    = ad->d_off;
    d->de.d_reclen = ad->d_reclen;
    d->de.d_type   = ad->d_type;
    size_t i = 0;
    while (ad->d_name[i] && i < sizeof d->de.d_name - 1) { d->de.d_name[i] = ad->d_name[i]; i++; }
    d->de.d_name[i] = '\0';
    return &d->de;
}
int closedir(DIR *d) {
    if (!d) { errno = AIOS_EBADF; return -1; }
    int fd = d->fd;
    free(d);                                             /* free is a no-op today; DIRs are not reclaimed */
    return aios_close(fd);
}
/* Wrap an already-open directory fd (from openat O_DIRECTORY) in a DIR -- recurse opens a subdir by
 * fd, then streams it. dirfd() hands the fd back so callers can resolve names relative to it. */
DIR *fdopendir(int fd) {
    if (fd < 0) { errno = AIOS_EBADF; return 0; }
    DIR *d = malloc(sizeof *d);
    if (!d) { errno = AIOS_ENOMEM; return 0; }
    d->fd = fd; d->pos = 0; d->len = 0;
    return d;
}
int dirfd(DIR *d) { return d ? d->fd : -1; }

/* sys/wait */
int wait(int *status)                          { return (int)__ret(aios_wait(status)); }
int waitpid(int pid, int *status, int options) { return (int)__ret(aios_waitpid(pid, status, options)); }
int wait3(int *status, int options, void *rusage) { (void)rusage; return waitpid(-1, status, options); }

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
int strcasecmp(const char *a, const char *b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (!a[i]) break;
    }
    return 0;
}
size_t strspn(const char *s, const char *accept) {
    size_t n = 0; for (; s[n]; n++) if (!strchr(accept, s[n])) break; return n;
}
size_t strcspn(const char *s, const char *reject) {
    size_t n = 0; for (; s[n]; n++) if (strchr(reject, s[n])) break; return n;
}
char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++) if (strchr(accept, *s)) return (char *)s;
    return 0;
}
char *strtok(char *s, const char *delim) {
    static char *save;
    if (!s) s = save;
    if (!s) return 0;
    s += strspn(s, delim);
    if (!*s) { save = 0; return 0; }
    char *tok = s;
    s = strpbrk(tok, delim);
    if (s) { *s = '\0'; save = s + 1; } else save = 0;
    return tok;
}
char *stpncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    char *end = d + i;
    for (; i < n; i++) d[i] = '\0';
    return end;
}
char *strndup(const char *s, size_t n) {                /* copy at most n bytes + a NUL */
    size_t len = 0; while (len < n && s[len]) len++;
    char *p = malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = '\0'; }
    return p;
}
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
long long strtoll(const char *s, char **end, int base) { return strtol(s, end, base); }            /* LP64: long == long long */
unsigned long long strtoull(const char *s, char **end, int base) { return strtoul(s, end, base); }
long atol(const char *s) { return strtol(s, 0, 10); }
char *getenv(const char *name) {
    size_t n = strlen(name);
    for (char **e = environ; e && *e; e++)
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return *e + n + 1;
    return 0;
}

/* --- time (UTC; no timezone, no RTC) ---
 * localtime == gmtime (no zone). gmtime converts a time_t to a broken-down struct tm via the civil-
 * from-days algorithm; strftime formats it (the subset ls -l needs, with manual zero-padding since
 * the printf core has no field widths yet). time() has no clock to read, so it returns a fixed 0
 * "now" -- enough for ls's recent-vs-old date heuristic; a real clock syscall is future work.
 * struct tm here MUST match the shadow <time.h> one (ls reads the fields). */
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
static struct tm _tm;
struct tm *gmtime(const long *tp) {
    long t = *tp, days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    _tm.tm_hour = (int)(rem / 3600); rem %= 3600;
    _tm.tm_min  = (int)(rem / 60);   _tm.tm_sec = (int)(rem % 60);
    _tm.tm_wday = (int)(((days % 7) + 4 + 7) % 7);          /* 1970-01-01 was a Thursday (4) */
    long z = days + 719468;                                  /* shift epoch to 0000-03-01 */
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);            /* day of era   [0, 146096] */
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;  /* year of era [0, 399] */
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);       /* day of year  [0, 365] */
    unsigned mp = (5*doy + 2) / 153;                        /* month        [0, 11] (Mar=0) */
    _tm.tm_mday = (int)(doy - (153*mp + 2)/5 + 1);          /* day of month [1, 31] */
    _tm.tm_mon  = (int)(mp < 10 ? mp + 2 : mp - 10);        /* month        [0, 11] (Jan=0) */
    _tm.tm_year = (int)(y + (mp >= 10) - 1900);
    _tm.tm_yday = 0;                                         /* not computed (ls does not use it) */
    _tm.tm_isdst = 0;
    return &_tm;
}
struct tm *localtime(const long *tp) { return gmtime(tp); }
long time(long *tp) { if (tp) *tp = 0; return 0; }          /* no RTC: a fixed "now" */

static const char _mon3[12][4] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
static const char _wday3[7][4]  = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    size_t n = 0;
    if (!max) return 0;
    for (; *fmt && n < max - 1; fmt++) {
        if (*fmt != '%') { s[n++] = *fmt; continue; }
        fmt++;
        if (!*fmt) { s[n++] = '%'; break; }
        const char *str = 0;
        int two = -1, pad = '0';
        switch (*fmt) {
        case 'a': str = _wday3[((tm->tm_wday % 7) + 7) % 7]; break;
        case 'b': case 'h': str = _mon3[((tm->tm_mon % 12) + 12) % 12]; break;
        case 'd': two = tm->tm_mday; break;
        case 'e': two = tm->tm_mday; pad = ' '; break;
        case 'H': two = tm->tm_hour; break;
        case 'M': two = tm->tm_min;  break;
        case 'S': two = tm->tm_sec;  break;
        case 'm': two = tm->tm_mon + 1; break;
        case 'y': two = (tm->tm_year + 1900) % 100; break;
        case 'Y': { unsigned y = (unsigned)(tm->tm_year + 1900); char b[8]; char *p = b + 7; *p = 0;
                    do { *--p = (char)('0' + y % 10); y /= 10; } while (y);
                    while (*p && n < max - 1) s[n++] = *p++;
                    continue; }
        case '%': s[n++] = '%'; continue;
        default:  s[n++] = '%'; if (n < max - 1) s[n++] = *fmt; continue;
        }
        if (two >= 0) {
            if (n < max - 1) s[n++] = (char)(two < 10 ? pad : '0' + (two / 10) % 10);
            if (n < max - 1) s[n++] = (char)('0' + two % 10);
        } else if (str) {
            while (*str && n < max - 1) s[n++] = *str++;
        }
    }
    s[n] = '\0';
    return n;
}

/* --- users / groups: no passwd/group database yet, so the lookups fail (return NULL). Callers
 * (ls -l) fall back to the numeric uid/gid -- correct minimal behaviour. --- */
struct passwd { char *pw_name; char *pw_passwd; unsigned int pw_uid, pw_gid;
                char *pw_gecos, *pw_dir, *pw_shell; };
struct group  { char *gr_name; char *gr_passwd; unsigned int gr_gid; char **gr_mem; };
struct passwd *getpwuid(unsigned int uid) { (void)uid; return 0; }
struct passwd *getpwnam(const char *name) { (void)name; return 0; }
struct group  *getgrgid(unsigned int gid) { (void)gid; return 0; }
struct group  *getgrnam(const char *name) { (void)name; return 0; }

/* --- process identity (single host-side identity for now; the kernel runs as the launching user) --- */
int getppid(void) { return 1; }                    /* no parent-pid syscall yet; $PPID is cosmetic */
int getuid(void)  { return 0; }
int geteuid(void) { return 0; }
int getgid(void)  { return 0; }
int getegid(void) { return 0; }

/* --- sysconf / rlimit / times: minimal so dash's miscbltin (ulimit/times) + paths compile + run. --- */
long sysconf(int name) {
    switch (name) {
    case 2:  return 100;        /* _SC_CLK_TCK  */
    case 4:  return 64;         /* _SC_OPEN_MAX (matches the kernel's per-proc fd table) */
    case 30: return 4096;       /* _SC_PAGESIZE */
    default: return -1;
    }
}
struct aios_rlimit { unsigned long long rlim_cur, rlim_max; };
int getrlimit(int res, struct aios_rlimit *rl) {   /* everything unlimited for now */
    (void)res; if (rl) { rl->rlim_cur = ~0ULL; rl->rlim_max = ~0ULL; } return 0;
}
int setrlimit(int res, const struct aios_rlimit *rl) { (void)res; (void)rl; return 0; }
struct aios_tms { long tms_utime, tms_stime, tms_cutime, tms_cstime; };
long times(struct aios_tms *t) {                   /* no clock yet -> all zero */
    if (t) { t->tms_utime = t->tms_stime = t->tms_cutime = t->tms_cstime = 0; }
    return 0;
}

/* --- signals: dispositions are RECORDED but not yet delivered (no async signal path through the
 * PAL yet). dash installs handlers + masks signals; with -c scripts nothing fires, so recording is
 * enough to run. kill is a real-ish stub. A real delivery path is future work. --- */
typedef void (*aios_sighandler)(int);
static aios_sighandler g_sigdisp[65];              /* Linux _NSIG = 65 (signals 1..64) */
typedef struct { aios_sighandler sa_handler; unsigned long sa_mask; int sa_flags; } aios_sigaction;
aios_sighandler signal(int sig, aios_sighandler h) {
    if (sig < 0 || sig >= 65) return (aios_sighandler)-1;
    aios_sighandler old = g_sigdisp[sig]; g_sigdisp[sig] = h; return old;
}
int sigaction(int sig, const aios_sigaction *act, aios_sigaction *old) {
    if (sig < 0 || sig >= 65) { errno = AIOS_EINVAL; return -1; }
    if (old) { old->sa_handler = g_sigdisp[sig]; old->sa_mask = 0; old->sa_flags = 0; }
    if (act) g_sigdisp[sig] = act->sa_handler;
    return 0;
}
int kill(int pid, int sig) { (void)pid; (void)sig; return 0; }   /* no delivery path yet */
int raise(int sig) { (void)sig; return 0; }
unsigned int alarm(unsigned int sec) { (void)sec; return 0; }
/* signal sets + masking are no-ops (no pending/blocked model yet). */
int sigemptyset(unsigned long *set) { if (set) *set = 0; return 0; }
int sigfillset(unsigned long *set)  { if (set) *set = ~0UL; return 0; }
int sigaddset(unsigned long *set, int s) { if (set) *set |= (1UL << (s & 63)); return 0; }
int sigdelset(unsigned long *set, int s) { if (set) *set &= ~(1UL << (s & 63)); return 0; }
int sigismember(const unsigned long *set, int s) { return set ? (int)((*set >> (s & 63)) & 1) : 0; }
int sigprocmask(int how, const unsigned long *set, unsigned long *old) {
    (void)how; (void)set; if (old) *old = 0; return 0;
}
int sigsuspend(const unsigned long *mask) { (void)mask; return -1; }   /* nothing to wait for */
char *strsignal(int sig) {
    static const char *n[] = { 0, "Hangup", "Interrupt", "Quit", "Illegal instruction",
        "Trace/breakpoint trap", "Aborted", "Bus error", "Floating point exception", "Killed",
        "User defined signal 1", "Segmentation fault", "User defined signal 2", "Broken pipe",
        "Alarm clock", "Terminated" };
    if (sig > 0 && sig < (int)(sizeof n / sizeof n[0])) return (char *)n[sig];
    return "Unknown signal";
}

/* gettimeofday / ioctl: no clock or terminal-geometry source yet -> zero / failure. dash uses these
 * for timing + terminal width; both degrade gracefully. */
struct aios_timeval { long tv_sec; long tv_usec; };
int gettimeofday(struct aios_timeval *tv, void *tz) {
    (void)tz; if (tv) { tv->tv_sec = 0; tv->tv_usec = 0; } return 0;
}
int ioctl(int fd, unsigned long req, ...) { (void)fd; (void)req; errno = AIOS_ENOTTY; return -1; }

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

/* bsearch: generic binary search over a sorted array (sbase's isspacerune/runetype lookups). */
void *bsearch(const void *key, const void *base, size_t n, size_t sz,
              int (*cmp)(const void *, const void *)) {
    const char *b = base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, b + mid * sz);
        if (c < 0)      hi = mid;
        else if (c > 0) lo = mid + 1;
        else            return (void *)(b + mid * sz);
    }
    return 0;
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
/* getopt_long: AIOS has no long-option parsing; fall back to short getopt (ignore longopts). dash's
 * SMALL histedit.c includes <getopt.h> but does not rely on long options. */
struct option;   /* opaque here -- the shadow <getopt.h> defines it for callers */
int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex) {
    (void)longopts; (void)longindex;
    return getopt(argc, argv, optstring);
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

/* setjmp/longjmp (aarch64): save/restore the callee-saved regs (x19-x28), fp (x29), lr (x30), sp,
 * and the callee-saved FP regs (d8-d15). sigsetjmp/siglongjmp ignore the savemask (no signal-mask
 * model yet), so they alias setjmp/longjmp. jmp_buf must hold >= 22 doublewords (see <setjmp.h>). */
__asm__(
    ".global setjmp\n"
    ".global sigsetjmp\n"
    "setjmp:\n"
    "sigsetjmp:\n"
    "  stp x19, x20, [x0, #0]\n"
    "  stp x21, x22, [x0, #16]\n"
    "  stp x23, x24, [x0, #32]\n"
    "  stp x25, x26, [x0, #48]\n"
    "  stp x27, x28, [x0, #64]\n"
    "  stp x29, x30, [x0, #80]\n"
    "  mov x1, sp\n"
    "  str x1, [x0, #96]\n"
    "  stp d8,  d9,  [x0, #104]\n"
    "  stp d10, d11, [x0, #120]\n"
    "  stp d12, d13, [x0, #136]\n"
    "  stp d14, d15, [x0, #152]\n"
    "  mov w0, #0\n"
    "  ret\n"
    ".global longjmp\n"
    ".global siglongjmp\n"
    "longjmp:\n"
    "siglongjmp:\n"
    "  ldp x19, x20, [x0, #0]\n"
    "  ldp x21, x22, [x0, #16]\n"
    "  ldp x23, x24, [x0, #32]\n"
    "  ldp x25, x26, [x0, #48]\n"
    "  ldp x27, x28, [x0, #64]\n"
    "  ldp x29, x30, [x0, #80]\n"
    "  ldr x2,  [x0, #96]\n"
    "  mov sp, x2\n"
    "  ldp d8,  d9,  [x0, #104]\n"
    "  ldp d10, d11, [x0, #120]\n"
    "  ldp d12, d13, [x0, #136]\n"
    "  ldp d14, d15, [x0, #152]\n"
    "  cmp w1, #0\n"
    "  csinc w0, w1, wzr, ne\n"   /* return val ? val : 1 */
    "  ret\n"
);
