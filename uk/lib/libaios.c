/*
 * libaios.c -- minimal C runtime for AIOS-ABI programs (see libaios.h).
 *
 * Everything here is built on the AIOS ABI (svc with AIOS syscall numbers) -- no host calls. It
 * provides _start (reads argc/argv off the stack, calls main, exits with its return), a few
 * string ops, a bump allocator, and a small printf. The seed of the AIOS-ABI libc.
 */
#include "libaios.h"
#include "aios_abi.h"
#include "aios_version.h"   /* AIOS_VERSION_STR / _LINE for uname() -- pure macros, no host dependency */
#include <stdarg.h>

/* --- the AIOS syscall instruction ---
 * The trap goes through the Linux/aarch64 PAL GATEWAY convention (aios_abi.h): x8 = AIOS_GATEWAY (an
 * in-range real syscall so seccomp traps it), the real AIOS number in x9, args in x0.. as usual. The
 * ptrace + seccomp PALs both decode (x8==AIOS_GATEWAY ? x9 : escape). x9 is a caller-saved temporary,
 * not a syscall-argument register, so it is free to carry the number. */
static long asys(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = AIOS_GATEWAY;
    register long x9 __asm__("x9") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x9), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}
/* 4-argument variant (the *at family: dirfd, path, flags/statbuf, mode/flags). */
static long asys4(long nr, long a0, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = AIOS_GATEWAY;
    register long x9 __asm__("x9") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x9), "r"(x1), "r"(x2), "r"(x3) : "memory", "cc");
    return x0;
}
/* 5-argument variant (fchownat: dirfd,path,owner,group,flags; linkat: olddirfd,old,newdirfd,new,flags). */
static long asys5(long nr, long a0, long a1, long a2, long a3, long a4) {
    register long x8 __asm__("x8") = AIOS_GATEWAY;
    register long x9 __asm__("x9") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x9), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory", "cc");
    return x0;
}

int errno;   /* POSIX errno; set by the standard-named wrappers (see __ret) and read by perror */
char *strerror(int errnum);   /* defined below; declared early so perror (stdio) can use it */
int tolower(int c), toupper(int c);   /* defined below; used earlier by strcasecmp (gcc14: no implicit decls) */
void *realloc(void *p, size_t n);     /* defined below; used earlier by getdelim */
char *strrchr(const char *s, int c);  /* defined below; used earlier by basename/dirname */
static long __ret(long r);            /* defined below; used earlier by fopen (errno on failure) */

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
    int    is_mem;          /* fmemopen stream: buf is the whole content, fd is -1 (no host I/O) */
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
    if (f->is_mem) { f->eof = 1; return 0; }   /* mem stream: content is already in buf, so this is a clean EOF */
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
/* getdelim/getline: read a record (up to + including `delim`) into a malloc'd, auto-growing buffer.
 * POSIX semantics -- the lineptr+n pair owns the buffer across calls; -1 at EOF with nothing read.
 * head and many libc programs read their input this way. */
long getdelim(char **lineptr, size_t *n, int delim, FILE *f) {
    if (!lineptr || !n) { errno = AIOS_EINVAL; return -1; }
    if (!*lineptr || *n == 0) { *n = 128; *lineptr = malloc(*n); if (!*lineptr) { *n = 0; return -1; } }
    size_t len = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) { if (len == 0) return -1; break; }
        if (len + 1 >= *n) {
            size_t nn = *n * 2;
            char *p = realloc(*lineptr, nn);
            if (!p) return -1;
            *lineptr = p; *n = nn;
        }
        (*lineptr)[len++] = (char)c;
        if (c == delim) break;
    }
    (*lineptr)[len] = '\0';
    return (long)len;
}
long getline(char **lineptr, size_t *n, FILE *f) { return getdelim(lineptr, n, '\n', f); }

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
    long fd = __ret(aios_open(path, flags, 0644));   /* __ret sets errno on failure (fopen must too) */
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
    return fd >= 0 ? aios_close(fd) : 0;             /* mem streams (fd<0) have no host fd to close */
}

/* fmemopen -- a read-mode in-memory stream over a PRIVATE copy of buf[0..size). grep/sed open their
 * -e/-f and literal patterns this way (then getline over the FILE). Reads return the bytes then a
 * clean EOF (is_mem, so ferror stays false). Write modes are not implemented -- no in-scope util
 * needs them. */
FILE *fmemopen(void *buf, size_t size, const char *mode) {
    if (!mode || mode[0] != 'r') { errno = AIOS_EINVAL; return 0; }
    FILE *f = malloc(sizeof *f);
    if (!f) return 0;
    memset(f, 0, sizeof *f);
    f->fd = -1; f->rd = 1; f->is_mem = 1; f->bufmode = _IOFBF;
    f->buf = malloc(size ? size : 1);
    if (!f->buf) { free(f); return 0; }
    if (size && buf) memcpy(f->buf, buf, size);
    f->cap = size; f->pos = 0; f->end = size;
    return f;
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

/* ---- floating-point conversion (%f/%e/%g) for printf. AIOS has HW doubles (aarch64), so this is a
 * scaled-integer digit extraction: normalize + round `prec+1` significant digits into a u64 (round-half-
 * to-even, glibc's rule) and emit them. Byte-identical to glibc across the normal range seq/printf use.
 * HONEST LIMITS (all stem from using DOUBLE, not bignum, for the intermediate arithmetic -- matching
 * glibc exactly requires arbitrary precision, which is out of scope for a minimal libc):
 *   - precision is capped at 17 (a double's true information content; beyond that glibc emits the exact
 *     binary expansion);
 *   - %f of a huge magnitude (|x| * 10^prec >= ~1.8e19) exceeds the u64 range (%g/%e go scientific
 *     there, so only an explicit %f of a giant value is affected);
 *   - a value whose EXACT decimal sits within ~1 ULP of the rounding boundary at the requested
 *     precision (e.g. 0.005, 2.675) can round the other way, because v*10^prec rounds in double before
 *     we inspect the halfway bit. Real seq/printf inputs don't hit this; it is not soft-float-fixable
 *     (long double is binary128 -> needs __multf3, absent under -nostdlib). */
static const unsigned long long _pow10u[19] = {
    1ULL,10ULL,100ULL,1000ULL,10000ULL,100000ULL,1000000ULL,10000000ULL,100000000ULL,1000000000ULL,
    10000000000ULL,100000000000ULL,1000000000000ULL,10000000000000ULL,100000000000000ULL,
    1000000000000000ULL,10000000000000000ULL,100000000000000000ULL,1000000000000000000ULL};

/* Round a non-negative double to a u64 using round-half-to-EVEN (glibc's rule), so ties go to the even
 * neighbour (0.5 -> 0, 2.5 -> 2, 1.5 -> 2) -- matching printf, not round-half-up. */
static unsigned long long round_even(double x) {
    unsigned long long f = (unsigned long long)x;         /* floor, since x >= 0 */
    double r = x - (double)f;
    if (r > 0.5) return f + 1;
    if (r < 0.5) return f;
    return (f & 1ULL) ? f + 1 : f;                        /* exact tie -> the even value */
}
/* %f: fixed-point, value >= 0 and finite. Rounds to `prec` fractional digits (half-to-even). */
static size_t f_fixed(char *o, double v, int prec, int alt) {
    if (prec > 17) prec = 17;
    unsigned long long ip, fp;
    if (prec == 0) { ip = round_even(v); fp = 0; }
    else { unsigned long long su = _pow10u[prec];
           unsigned long long tot = round_even(v * (double)su);
           ip = tot / su; fp = tot % su; }
    char ib[24]; int ni = 0; unsigned long long t = ip;
    do { ib[ni++] = (char)('0' + (int)(t % 10)); t /= 10; } while (t);
    size_t i = 0; while (ni > 0) o[i++] = ib[--ni];
    if (prec > 0 || alt) {                                /* '#' keeps the point even at precision 0 */
        o[i++] = '.';
        if (prec > 0) {
            char fb[24]; int nf = 0; t = fp;
            do { fb[nf++] = (char)('0' + (int)(t % 10)); t /= 10; } while (t);
            while (nf < prec) fb[nf++] = '0';             /* left-pad the fraction to prec digits */
            while (nf > 0) o[i++] = fb[--nf];
        }
    }
    return i;
}
/* %e: scientific, value >= 0 and finite. `prec` fractional (mantissa) digits; exponent >= 2 digits. */
static size_t f_sci(char *o, double v, int prec, int alt, int up) {
    if (prec > 17) prec = 17;
    int E = 0;
    if (v != 0.0) { while (v >= 10.0) { v /= 10.0; E++; } while (v < 1.0) { v *= 10.0; E--; } }
    unsigned long long su = _pow10u[prec];
    unsigned long long m = round_even(v * (double)su);    /* prec+1 significant digits (half-to-even) */
    if (m >= su * 10ULL) { m /= 10; E++; }                /* rounding carried 9.99..->10 -> 1.0, E++ */
    char ds[24]; int nd = 0; unsigned long long t = m;
    do { ds[nd++] = (char)('0' + (int)(t % 10)); t /= 10; } while (t);
    while (nd < prec + 1) ds[nd++] = '0';                 /* pad to prec+1 significant digits */
    size_t i = 0; o[i++] = ds[--nd];                      /* the single integer digit */
    if (prec > 0 || alt) { o[i++] = '.'; while (nd > 0) o[i++] = ds[--nd]; }
    o[i++] = up ? 'E' : 'e';
    o[i++] = (E < 0) ? '-' : '+';
    int ae = (E < 0) ? -E : E; char eb[8]; int en = 0;
    do { eb[en++] = (char)('0' + ae % 10); ae /= 10; } while (ae);
    while (en < 2) eb[en++] = '0';                        /* at least two exponent digits */
    while (en > 0) o[i++] = eb[--en];
    return i;
}
/* Dispatch %f/%e/%g (any case). Writes the unsigned body (digits/point/exponent) to `out`, sets *neg
 * (sign bit, so -0.0 prints '-') and *special (1=inf, 2=nan). */
static size_t fmt_double(char *out, double val, int prec, char conv, int alt, int *neg, int *special) {
    unsigned long long bits; memcpy(&bits, &val, sizeof bits);
    *neg = (int)(bits >> 63); *special = 0;
    double av = *neg ? -val : val;
    unsigned long long e = (bits >> 52) & 0x7ff, f = bits & 0xfffffffffffffULL;
    int up = (conv < 'a'); char c = up ? (char)(conv + 32) : conv;
    if (e == 0x7ff) {                                     /* inf / nan */
        const char *w = f ? (up ? "NAN" : "nan") : (up ? "INF" : "inf");
        *special = f ? 2 : 1; size_t i = 0; while (w[i]) { out[i] = w[i]; i++; } return i;
    }
    if (prec < 0) prec = 6;
    if (c == 'f') return f_fixed(out, av, prec, alt);
    if (c == 'e') return f_sci(out, av, prec, alt, up);
    /* %g: shortest of %e/%f; P significant digits; strip trailing zeros unless '#'. */
    int P = (prec == 0) ? 1 : prec; if (P > 17) P = 17;
    int E = 0; double t = av;
    if (t != 0.0) { while (t >= 10.0) { t /= 10.0; E++; } while (t < 1.0) { t *= 10.0; E--; } }
    /* the e-vs-f decision must use the exponent AFTER rounding to P sig figs: rounding can carry the
     * mantissa across a power of 10 (e.g. 9.9 at %.1g rounds to 10 -> "1e+01", not "10"). */
    if (av != 0.0 && round_even(t * (double)_pow10u[P - 1]) >= _pow10u[P]) E++;
    size_t len = (E < -4 || E >= P) ? f_sci(out, av, P - 1, alt, up) : f_fixed(out, av, P - 1 - E, alt);
    if (!alt) {                                           /* strip trailing zeros (and a bare '.') */
        size_t ep = len; for (size_t k = 0; k < len; k++) if (out[k] == 'e' || out[k] == 'E') { ep = k; break; }
        size_t dot = ep; for (size_t k = 0; k < ep; k++) if (out[k] == '.') { dot = k; break; }
        if (dot < ep) {
            size_t end = ep; while (end > dot + 1 && out[end - 1] == '0') end--;
            if (end == dot + 1) end = dot;                /* nothing left after '.' -> drop it too */
            size_t shift = ep - end;
            if (shift) { for (size_t k = ep; k < len; k++) out[k - shift] = out[k]; len -= shift; }
        }
    }
    return len;
}

/* printf-family core: flags (- 0 + space #), width (incl. *), precision (incl. .*), the l/z/h length
 * modifiers, and %s %d %i %u %x %X %o %c %p %f %e %g (+ upper) %%. Enough for sbase + dash. */
static void vformat(struct sink *s, const char *fmt, va_list ap) {
    char digits[24]; char fbuf[80];
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { sink_write(s, p, 1); continue; }
        p++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; p++) {                                       /* flags */
            if      (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '#') alt = 1;
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
        int isnum = 0, isfloat = 0;
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
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                    double dv = va_arg(ap, double); int neg, special;
                    blen = fmt_double(fbuf, dv, prec, *p, alt, &neg, &special); body = fbuf;
                    if (neg)        { prefix[0] = '-'; plen = 1; }
                    else if (plus)  { prefix[0] = '+'; plen = 1; }
                    else if (space) { prefix[0] = ' '; plen = 1; }
                    isnum = 1; isfloat = 1;                  /* isnum -> '0' flag zero-pads; isfloat -> no precision zfill */
                    if (special) { isnum = 0; }              /* inf/nan: space-pad even with the 0 flag */
                    break; }
        case '%': digits[0] = '%'; blen = 1; break;
        case '\0': p--; continue;                            /* trailing '%' -> stop cleanly */
        default:  digits[0] = '%'; digits[1] = *p; blen = 2; break;
        }

        int zfill = 0;                                       /* integer precision = min digit count */
        if (isnum && !isfloat && prec >= 0) { if ((size_t)prec > blen) zfill = (int)((size_t)prec - blen); zero = 0; }
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
int sprintf(char *buf, const char *fmt, ...) {            /* unbounded -- grep builds its -w/-x patterns with it */
    va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, (size_t)-1, fmt, ap); va_end(ap);
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
int  isatty(int fd){ return asys(AIOS_SYS_ISATTY, fd, 0, 0) > 0 ? 1 : 0; }
int  open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & AIOS_O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return (int)__ret(aios_open(path, flags, mode));
}
/* creat(path, mode) == open for write/create/truncate (sbase cp opens its destination this way). */
int  creat(const char *path, unsigned int mode) {
    return (int)__ret(aios_open(path, AIOS_O_WRONLY | AIOS_O_CREAT | AIOS_O_TRUNC, (int)mode));
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
/* umask: a real PER-PROCESS file-creation mask. The kernel tracks it (inherited across fork +
 * preserved across exec) and applies it on open(O_CREAT)/mkdir; the host umask is neutralized, so
 * this single mask governs created modes. Returns the previous mask. */
unsigned int umask(unsigned int m) { return (unsigned int)asys(AIOS_SYS_UMASK, (long)(m & 0777), 0, 0); }

/* File metadata: real, confinement-aware syscalls (the *at forms are the primitives; the plain forms
 * are them with AT_FDCWD). chmod/chown set mode/owner; symlink/link create links; utimensat sets
 * times. fchmod/fchown have no fd-metadata syscall (nothing in our utils calls them) -> ENOSYS; utime
 * is a legacy no-op; mknod cannot make special files -> ENOSYS. basename/dirname are real string ops. */
int fchmodat(int d, const char *p, unsigned int m, int f) { return (int)__ret(asys4(AIOS_SYS_FCHMODAT, d, (long)p, (long)m, f)); }
int chmod (const char *p, unsigned int m)                 { return fchmodat(AIOS_AT_FDCWD, p, m, 0); }
int fchmod(int fd, unsigned int m)                        { (void)fd; (void)m; errno = AIOS_ENOSYS; return -1; }
int fchownat(int d, const char *p, unsigned int u, unsigned int g, int f) { return (int)__ret(asys5(AIOS_SYS_FCHOWNAT, d, (long)p, (long)u, (long)g, f)); }
int chown (const char *p, unsigned int u, unsigned int g) { return fchownat(AIOS_AT_FDCWD, p, u, g, 0); }
int lchown(const char *p, unsigned int u, unsigned int g) { return fchownat(AIOS_AT_FDCWD, p, u, g, AIOS_AT_SYMLINK_NOFOLLOW); }
int fchown(int fd, unsigned int u, unsigned int g)        { (void)fd; (void)u; (void)g; errno = AIOS_ENOSYS; return -1; }
int symlinkat(const char *t, int d, const char *l)        { return (int)__ret(asys(AIOS_SYS_SYMLINKAT, (long)t, d, (long)l)); }
int symlink(const char *t, const char *l)                 { return symlinkat(t, AIOS_AT_FDCWD, l); }
int linkat(int od, const char *o, int nd, const char *n, int f) { return (int)__ret(asys5(AIOS_SYS_LINKAT, od, (long)o, nd, (long)n, f)); }
int link  (const char *o, const char *n)                  { return linkat(AIOS_AT_FDCWD, o, AIOS_AT_FDCWD, n, 0); }
int utimensat(int d, const char *p, const void *t, int f) { return (int)__ret(asys4(AIOS_SYS_UTIMENSAT, d, (long)p, (long)t, f)); }
int utime (const char *p, const void *t)                  { (void)p; (void)t; return 0; }   /* legacy no-op */
int mknod (const char *p, unsigned int m, unsigned long long d) { (void)p; (void)m; (void)d; errno = AIOS_ENOSYS; return -1; }

static char *__path_tail(char *path, int wantdir) {
    if (!path || !*path) return (char *)".";
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = '\0';   /* strip trailing slashes */
    char *s = strrchr(path, '/');
    if (!wantdir) return s ? s + 1 : path;                  /* basename */
    if (!s) return (char *)".";                             /* dirname: no slash -> "." */
    if (s == path) return (char *)"/";                      /* dirname of "/x" -> "/"  */
    *s = '\0';
    return path;
}
char *basename(char *path) { return __path_tail(path, 0); }
char *dirname (char *path) { return __path_tail(path, 1); }

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
char *strcasestr(const char *h, const char *n) {         /* case-insensitive strstr (grep -iF) */
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return (char *)h;
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
long long llabs(long long v) { return v < 0 ? -v : v; }
long      labs (long v)      { return v < 0 ? -v : v; }
int       abs  (int v)       { return v < 0 ? -v : v; }
/* sleep: no clock/timer syscall yet -- a no-op (tail -f, the only sbase user, just busy-follows). */
unsigned int sleep(unsigned int sec) { (void)sec; return 0; }
unsigned long long strtoull(const char *s, char **end, int base) { return strtoul(s, end, base); }
long atol(const char *s) { return strtol(s, 0, 10); }

/* strtod: parse a decimal floating-point number [+-]ddd[.ddd][(e|E)[+-]ddd] (no hex/inf/nan -- enough
 * for sbase sort -g). aarch64 has hardware FP, so this needs no soft-float runtime. */
double strtod(const char *s, char **end) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    double val = 0.0; int any = 0;
    while (*p >= '0' && *p <= '9') { val = val * 10.0 + (double)(*p - '0'); p++; any = 1; }
    if (*p == '.') {
        p++;
        double frac = 0.1;
        while (*p >= '0' && *p <= '9') { val += (double)(*p - '0') * frac; frac *= 0.1; p++; any = 1; }
    }
    if (any && (*p == 'e' || *p == 'E')) {
        const char *e = p + 1; int eneg = 0;
        if (*e == '+' || *e == '-') { eneg = (*e == '-'); e++; }
        if (*e >= '0' && *e <= '9') {
            int exp = 0;
            while (*e >= '0' && *e <= '9') { exp = exp * 10 + (*e - '0'); e++; }
            double pw = 1.0;
            while (exp-- > 0) pw *= 10.0;
            if (eneg) val /= pw; else val *= pw;
            p = e;
        }
    }
    if (!any) { if (end) *end = (char *)s; return 0.0; }   /* no conversion */
    if (end) *end = (char *)p;
    return neg ? -val : val;
}
char *getenv(const char *name) {
    size_t n = strlen(name);
    for (char **e = environ; e && *e; e++)
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return *e + n + 1;
    return 0;
}

/* --- environment mutation (env/date use these). environ begins as the initial stack vector; an append
 *     mallocs a fresh, larger NULL-terminated array (malloc never frees -- fine for a short-lived util;
 *     an in-place replace just rewrites a slot). --- */
static int env_n(void) { int n = 0; if (environ) while (environ[n]) n++; return n; }
static int env_find(const char *name, size_t nl) {
    for (int i = 0; environ && environ[i]; i++)
        if (strncmp(environ[i], name, nl) == 0 && environ[i][nl] == '=') return i;
    return -1;
}
int putenv(char *string) {                       /* takes ownership of `string` ("NAME=VALUE") */
    char *eq = strchr(string, '='); size_t nl = eq ? (size_t)(eq - string) : strlen(string);
    int i = env_find(string, nl);
    if (i >= 0) { environ[i] = string; return 0; }
    int n = env_n(); char **ne = malloc((size_t)(n + 2) * sizeof(char *)); if (!ne) return -1;
    for (int k = 0; k < n; k++) ne[k] = environ[k];
    ne[n] = string; ne[n + 1] = 0; environ = ne; return 0;
}
int setenv(const char *name, const char *value, int overwrite) {
    size_t nl = strlen(name); int i = env_find(name, nl);
    if (i >= 0 && !overwrite) return 0;
    size_t vl = strlen(value); char *s = malloc(nl + 1 + vl + 1); if (!s) return -1;
    memcpy(s, name, nl); s[nl] = '='; memcpy(s + nl + 1, value, vl); s[nl + 1 + vl] = 0;
    if (i >= 0) { environ[i] = s; return 0; }
    return putenv(s);
}
int unsetenv(const char *name) {
    size_t nl = strlen(name); int i = env_find(name, nl);
    if (i < 0) return 0;
    int n = env_n();
    for (int k = i; k < n; k++) environ[k] = environ[k + 1];   /* shift down, incl. the trailing NULL */
    return 0;
}

/* --- time (UTC; no timezone) ---
 * localtime == gmtime (no zone). gmtime converts a time_t to a broken-down struct tm via the civil-
 * from-days algorithm; strftime formats it (the subset ls -l needs, with manual zero-padding since
 * the printf core has no field widths yet). time()/gettimeofday() now read a REAL clock via
 * AIOS_SYS_CLOCK_GETTIME (the kernel's PAL clock source) -- so ls dates, dash timing, etc. are live.
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
/* mktime: the inverse of gmtime (AIOS time is UTC, so the broken-down tm is treated as UTC). Epoch
 * seconds via days-from-civil (Howard Hinnant); normalizes an out-of-range tm_mon into tm_year. */
long mktime(struct tm *tm) {
    int yr = tm->tm_year + 1900, mon = tm->tm_mon;
    yr += mon / 12; mon %= 12; if (mon < 0) { mon += 12; yr--; }
    int m = mon + 1;                                   /* 1..12 */
    long y = yr - (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;                          /* [0,399] */
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;   /* [0,365] */
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  /* [0,146096] */
    long days = era * 146097 + doe - 719468;
    return days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
}

/* clock_gettime forwards the guest's struct timespec to the kernel, which fills it from the PAL clock
 * (struct timespec == struct aios_timespec, two 8-byte fields -- so a void* keeps libaios struct-free).
 * time()/gettimeofday() are thin REALTIME readers over it. */
int clock_gettime(int clk_id, void *ts) {
    return (int)__ret(asys(AIOS_SYS_CLOCK_GETTIME, clk_id, (long)ts, 0));
}
/* The AIOS clock is READ-ONLY (the kernel's only time source is the PAL host clock; there is no AIOS
 * syscall to set it). clock_settime fails EPERM -- so `date -s` reports it cannot set the time, while
 * reading the date works. (void args: a forward decl of struct timespec suffices.) */
int clock_settime(int clk_id, const void *ts) { (void)clk_id; (void)ts; errno = AIOS_EPERM; return -1; }
long time(long *tp) {
    long long ts[2] = { 0, 0 };                             /* aios_timespec: {sec, nsec} */
    clock_gettime(AIOS_CLOCK_REALTIME, ts);
    if (tp) *tp = (long)ts[0];
    return (long)ts[0];
}

static const char _mon3[12][4] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
static const char _wday3[7][4]  = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char _wdayfull[7][10] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
static const char _monfull[12][10] = {"January","February","March","April","May","June",
                                      "July","August","September","October","November","December"};
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    size_t n = 0;
    if (!max) return 0;
    for (; *fmt && n < max - 1; fmt++) {
        if (*fmt != '%') { s[n++] = *fmt; continue; }
        fmt++;
        if (!*fmt) { s[n++] = '%'; break; }
        const char *str = 0;
        int two = -1, pad = '0';
        int h12;
        switch (*fmt) {
        case 'a': str = _wday3[((tm->tm_wday % 7) + 7) % 7]; break;
        case 'A': str = _wdayfull[((tm->tm_wday % 7) + 7) % 7]; break;
        case 'b': case 'h': str = _mon3[((tm->tm_mon % 12) + 12) % 12]; break;
        case 'B': str = _monfull[((tm->tm_mon % 12) + 12) % 12]; break;
        case 'd': two = tm->tm_mday; break;
        case 'e': two = tm->tm_mday; pad = ' '; break;
        case 'H': two = tm->tm_hour; break;
        case 'I': h12 = tm->tm_hour % 12; two = h12 ? h12 : 12; break;
        case 'M': two = tm->tm_min;  break;
        case 'S': two = tm->tm_sec;  break;
        case 'm': two = tm->tm_mon + 1; break;
        case 'p': str = tm->tm_hour < 12 ? "AM" : "PM"; break;
        case 'Z': str = "UTC"; break;                  /* AIOS time is UTC (no timezone) */
        case 'u': two = ((tm->tm_wday + 6) % 7) + 1; break;  /* 1=Mon..7=Sun */
        case 'w': two = ((tm->tm_wday % 7) + 7) % 7; break;  /* 0=Sun..6=Sat */
        case 'n': s[n++] = '\n'; continue;
        case 't': s[n++] = '\t'; continue;
        case 'y': two = (tm->tm_year + 1900) % 100; break;
        case 'C': two = ((tm->tm_year + 1900) / 100) % 100; break;
        case 'Y': { unsigned y = (unsigned)(tm->tm_year + 1900); char b[8]; char *p = b + 7; *p = 0;
                    do { *--p = (char)('0' + y % 10); y /= 10; } while (y);
                    while (*p && n < max - 1) s[n++] = *p++;
                    continue; }
        /* combined specifiers: expand recursively (no combined spec nests another, so this terminates). */
        case 'F': case 'T': case 'R': case 'D': {
            const char *sub = *fmt == 'F' ? "%Y-%m-%d" : *fmt == 'T' ? "%H:%M:%S" :
                              *fmt == 'R' ? "%H:%M" : "%m/%d/%y";
            char t[24]; size_t k = strftime(t, sizeof t, sub, tm);
            for (size_t j = 0; j < k && n < max - 1; j++) s[n++] = t[j];
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

/* --- users / groups: a real passwd/group database, parsed from /etc/passwd and /etc/group. The
 * lookups return a pointer to STATIC storage (overwritten by the next call, per POSIX); ls -l now
 * shows real user/group NAMES (it fell back to numeric ids while these returned NULL). A missing or
 * unreadable file (e.g. a confined guest whose root has no /etc/passwd) still yields NULL -> the
 * numeric fallback, unchanged. struct passwd/group MUST match the shadow <pwd.h>/<grp.h>. --- */
struct passwd { char *pw_name; char *pw_passwd; unsigned int pw_uid, pw_gid;
                char *pw_gecos, *pw_dir, *pw_shell; };
struct group  { char *gr_name; char *gr_passwd; unsigned int gr_gid; char **gr_mem; };

/* Split `line` in place on `sep` into up to `max` fields (NUL-terminating each); returns the count.
 * The final field keeps any trailing separators (fine: passwd/group fields never embed the sep). */
static int split_fields(char *line, int sep, char **fields, int max) {
    int n = 0;
    char *s = line;
    while (n < max) {
        fields[n++] = s;
        char *c = strchr(s, sep);
        if (!c) break;
        *c = '\0';
        s = c + 1;
    }
    return n;
}
static char *chomp(char *s) { size_t L = strlen(s); if (L && s[L-1] == '\n') s[L-1] = '\0'; return s; }

static struct passwd s_pw;
static char          s_pwline[512];
/* Scan /etc/passwd for a match by uid (by_key=1) or name (by_key=0). Fields: name:passwd:uid:gid:
 * gecos:dir:shell. */
static struct passwd *pw_scan(unsigned int uid, const char *name, int by_key) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 0;
    struct passwd *r = 0;
    while (fgets(s_pwline, sizeof s_pwline, f)) {
        char *fld[7];
        int nf = split_fields(chomp(s_pwline), ':', fld, 7);
        if (nf < 4) continue;                                    /* malformed line */
        unsigned int fuid = (unsigned int)atoi(fld[2]);
        if (by_key ? (fuid == uid) : (name && !strcmp(fld[0], name))) {
            s_pw.pw_name   = fld[0];
            s_pw.pw_passwd = fld[1];
            s_pw.pw_uid    = fuid;
            s_pw.pw_gid    = (unsigned int)atoi(fld[3]);
            s_pw.pw_gecos  = nf > 4 ? fld[4] : (char *)"";
            s_pw.pw_dir    = nf > 5 ? fld[5] : (char *)"";
            s_pw.pw_shell  = nf > 6 ? fld[6] : (char *)"";
            r = &s_pw;
            break;
        }
    }
    fclose(f);
    return r;
}
struct passwd *getpwuid(unsigned int uid) { return pw_scan(uid, 0, 1); }
struct passwd *getpwnam(const char *name) { return name ? pw_scan(0, name, 0) : 0; }

static struct group s_gr;
static char         s_grline[4096];
static char        *s_grmem[256];
/* Scan /etc/group for a match by gid (by_key=1) or name (by_key=0). Fields: name:passwd:gid:m,m,... */
static struct group *gr_scan(unsigned int gid, const char *name, int by_key) {
    FILE *f = fopen("/etc/group", "r");
    if (!f) return 0;
    struct group *r = 0;
    while (fgets(s_grline, sizeof s_grline, f)) {
        char *fld[4];
        int nf = split_fields(chomp(s_grline), ':', fld, 4);
        if (nf < 3) continue;                                    /* malformed line */
        unsigned int fgid = (unsigned int)atoi(fld[2]);
        if (by_key ? (fgid == gid) : (name && !strcmp(fld[0], name))) {
            s_gr.gr_name   = fld[0];
            s_gr.gr_passwd = fld[1];
            s_gr.gr_gid    = fgid;
            int m = 0;
            if (nf > 3 && fld[3][0]) m = split_fields(fld[3], ',', s_grmem, 255);
            s_grmem[m] = 0;                                       /* NULL-terminate the member vector */
            s_gr.gr_mem = s_grmem;
            r = &s_gr;
            break;
        }
    }
    fclose(f);
    return r;
}
struct group *getgrgid(unsigned int gid) { return gr_scan(gid, 0, 1); }
struct group *getgrnam(const char *name) { return name ? gr_scan(0, name, 0) : 0; }

/* --- process groups + controlling-terminal foreground group (job-control foundation) --- */
int setpgid(int pid, int pgid) { return (int)__ret(asys(AIOS_SYS_SETPGID, pid, pgid, 0)); }
int getpgid(int pid)           { return (int)__ret(asys(AIOS_SYS_GETPGID, pid, 0, 0)); }
int getpgrp(void)              { return getpgid(0); }                       /* own process group */
int setpgrp(void)              { return setpgid(0, 0); }                    /* become a group leader */
int tcsetpgrp(int fd, int pgrp){ return (int)__ret(asys(AIOS_SYS_TCSETPGRP, fd, pgrp, 0)); }
int tcgetpgrp(int fd)          { return (int)__ret(asys(AIOS_SYS_TCGETPGRP, fd, 0, 0)); }
/* termios: the kernel/PAL own the struct translation, so these just hand the guest pointer across
 * (a forward decl is enough -- cfmakeraw and the cf-speed helpers are inline in shadow termios.h). */
struct termios;
int tcgetattr(int fd, struct termios *t)                  { return (int)__ret(asys(AIOS_SYS_TCGETATTR, fd, (long)t, 0)); }
int tcsetattr(int fd, int actions, const struct termios *t){ return (int)__ret(asys(AIOS_SYS_TCSETATTR, fd, actions, (long)t)); }

/* --- process identity: per-process real/effective/saved uid+gid, tracked by the kernel (AIOS-internal,
 *     decoupled from the host user). setuid/setgid drop/restore privilege; login uses them. --- */
int getppid(void) { return 1; }                    /* no parent-pid syscall yet; $PPID is cosmetic */
int getuid(void)  { return (int)asys(AIOS_SYS_GETUID,  0, 0, 0); }
int geteuid(void) { return (int)asys(AIOS_SYS_GETEUID, 0, 0, 0); }
int getgid(void)  { return (int)asys(AIOS_SYS_GETGID,  0, 0, 0); }
int getegid(void) { return (int)asys(AIOS_SYS_GETEGID, 0, 0, 0); }
int setuid(unsigned int uid) { return (int)__ret(asys(AIOS_SYS_SETUID, (long)uid, 0, 0)); }
int setgid(unsigned int gid) { return (int)__ret(asys(AIOS_SYS_SETGID, (long)gid, 0, 0)); }

/* getlogin: the login name. Prefer the environment login sets ($LOGNAME/$USER), else the passwd entry
 * for the real uid. Returns a pointer to static storage (POSIX), or NULL if neither is available. */
char *getlogin(void) {
    char *e = getenv("LOGNAME"); if (!e || !*e) e = getenv("USER");
    if (e && *e) {
        static char buf[64]; size_t i = 0;
        for (; e[i] && i < sizeof buf - 1; i++) buf[i] = e[i];
        buf[i] = '\0'; return buf;
    }
    struct passwd *p = getpwuid(getuid());
    return (p && p->pw_name) ? p->pw_name : 0;
}

/* ===== crypt(): SHA-512 ($6$) password hashing, glibc-compatible =====================================
 * /etc/shadow stores a hash, not plaintext: login recomputes crypt(typed_password, stored_hash) and
 * compares it to the stored hash. We implement the SHA-512 ("$6$") scheme (the modern Linux default,
 * Ulrich Drepper's spec) so AIOS's hashes are byte-identical to the host's `openssl passwd -6` / glibc
 * crypt -- a real, verifiable algorithm, no host call. SHA-512 (FIPS 180-4) lives here too; aarch64 has
 * native 64-bit ops so the freestanding -nostdlib guest needs no runtime helpers. */

typedef struct { unsigned long long h[8], len; unsigned char buf[128]; unsigned int n; } sha512_ctx;

static const unsigned long long SHA512_K[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL };

static unsigned long long ror64(unsigned long long x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha512_init(sha512_ctx *c) {
    c->h[0]=0x6a09e667f3bcc908ULL; c->h[1]=0xbb67ae8584caa73bULL;
    c->h[2]=0x3c6ef372fe94f82bULL; c->h[3]=0xa54ff53a5f1d36f1ULL;
    c->h[4]=0x510e527fade682d1ULL; c->h[5]=0x9b05688c2b3e6c1fULL;
    c->h[6]=0x1f83d9abfb41bd6bULL; c->h[7]=0x5be0cd19137e2179ULL;
    c->len = 0; c->n = 0;
}
static void sha512_block(sha512_ctx *c, const unsigned char *p) {
    unsigned long long w[80], a,b,d,e,f,g,hh,t1,t2,s0,s1,ch,maj; int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned long long)p[i*8]<<56)|((unsigned long long)p[i*8+1]<<48)|
               ((unsigned long long)p[i*8+2]<<40)|((unsigned long long)p[i*8+3]<<32)|
               ((unsigned long long)p[i*8+4]<<24)|((unsigned long long)p[i*8+5]<<16)|
               ((unsigned long long)p[i*8+6]<<8)|((unsigned long long)p[i*8+7]);
    }
    for (i = 16; i < 80; i++) {
        s0 = ror64(w[i-15],1) ^ ror64(w[i-15],8) ^ (w[i-15] >> 7);
        s1 = ror64(w[i-2],19) ^ ror64(w[i-2],61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->h[0];b=c->h[1];d=c->h[2];e=c->h[3];f=c->h[4];g=c->h[5];hh=c->h[6];t2=c->h[7];
    /* note: use t2 as 'h7' carrier then reuse; keep an explicit 8th var */
    { unsigned long long va=a,vb=b,vc=d,vd=e,ve=f,vf=g,vg=hh,vh=t2;
      for (i = 0; i < 80; i++) {
        s1 = ror64(ve,14) ^ ror64(ve,18) ^ ror64(ve,41);
        ch = (ve & vf) ^ (~ve & vg);
        t1 = vh + s1 + ch + SHA512_K[i] + w[i];
        s0 = ror64(va,28) ^ ror64(va,34) ^ ror64(va,39);
        maj = (va & vb) ^ (va & vc) ^ (vb & vc);
        t2 = s0 + maj;
        vh=vg; vg=vf; vf=ve; ve=vd+t1; vd=vc; vc=vb; vb=va; va=t1+t2;
      }
      c->h[0]+=va; c->h[1]+=vb; c->h[2]+=vc; c->h[3]+=vd;
      c->h[4]+=ve; c->h[5]+=vf; c->h[6]+=vg; c->h[7]+=vh;
    }
}
static void sha512_update(sha512_ctx *c, const void *data, unsigned long len) {
    const unsigned char *p = (const unsigned char *)data;
    c->len += len;
    while (len) {
        unsigned int take = 128 - c->n; if (take > len) take = (unsigned int)len;
        for (unsigned int i = 0; i < take; i++) c->buf[c->n + i] = p[i];
        c->n += take; p += take; len -= take;
        if (c->n == 128) { sha512_block(c, c->buf); c->n = 0; }
    }
}
static void sha512_final(sha512_ctx *c, unsigned char out[64]) {
    unsigned long long bits = c->len << 3, hi = c->len >> 61;
    unsigned char pad0 = 0x80, z = 0; int i;
    unsigned long long origlen = c->len;
    sha512_update(c, &pad0, 1);
    while (c->n != 112) sha512_update(c, &z, 1);
    unsigned char lenbuf[16];
    for (i = 0; i < 8; i++) lenbuf[i]   = (unsigned char)(hi   >> (56 - i*8));
    for (i = 0; i < 8; i++) lenbuf[8+i] = (unsigned char)(bits >> (56 - i*8));
    (void)origlen;
    sha512_update(c, lenbuf, 16);   /* exactly fills the block -> processed */
    for (i = 0; i < 8; i++) {
        out[i*8]   = (unsigned char)(c->h[i] >> 56); out[i*8+1] = (unsigned char)(c->h[i] >> 48);
        out[i*8+2] = (unsigned char)(c->h[i] >> 40); out[i*8+3] = (unsigned char)(c->h[i] >> 32);
        out[i*8+4] = (unsigned char)(c->h[i] >> 24); out[i*8+5] = (unsigned char)(c->h[i] >> 16);
        out[i*8+6] = (unsigned char)(c->h[i] >> 8);  out[i*8+7] = (unsigned char)(c->h[i]);
    }
}

static const char b64t[65] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
static char *b64_24(char *cp, unsigned int b2, unsigned int b1, unsigned int b0, int n) {
    unsigned int w = (b2 << 16) | (b1 << 8) | b0;
    while (n-- > 0) { *cp++ = b64t[w & 0x3f]; w >>= 6; }
    return cp;
}

/* crypt(key, setting): supports the SHA-512 "$6$[rounds=N$]salt$" scheme. Returns a pointer to static
 * storage holding "$6$[rounds=N$]salt$hash" (POSIX). Returns NULL for an unsupported setting. */
char *crypt(const char *key, const char *setting) {
    static char result[200];
    if (strncmp(setting, "$6$", 3) != 0) return 0;        /* only $6$ (SHA-512) supported */
    const char *s = setting + 3;
    unsigned long rounds = 5000; int rounds_custom = 0;
    if (strncmp(s, "rounds=", 7) == 0) {
        char *endp; unsigned long r = strtoul(s + 7, &endp, 10);
        if (*endp == '$') { s = endp + 1; rounds = r; rounds_custom = 1;
            if (rounds < 1000) rounds = 1000;
            if (rounds > 999999999UL) rounds = 999999999UL; }
    }
    unsigned int salt_len = 0; while (salt_len < 16 && s[salt_len] && s[salt_len] != '$') salt_len++;
    const char *salt = s;
    unsigned long key_len = strlen(key);
    if (key_len > 256) return 0;                          /* defensive: login passwords are short */

    sha512_ctx ctx; unsigned char A[64], B[64], DP[64], DS[64], C[64];
    unsigned char Pbuf[256], Sbuf[256]; unsigned long i, cnt;

    /* B = SHA512(key . salt . key) */
    sha512_init(&ctx); sha512_update(&ctx, key, key_len);
    sha512_update(&ctx, salt, salt_len); sha512_update(&ctx, key, key_len); sha512_final(&ctx, B);
    /* A = SHA512(key . salt . B[for key_len bytes] . (per key_len bits: B if set else key)) */
    sha512_init(&ctx); sha512_update(&ctx, key, key_len); sha512_update(&ctx, salt, salt_len);
    for (cnt = key_len; cnt > 64; cnt -= 64) sha512_update(&ctx, B, 64);
    sha512_update(&ctx, B, cnt);
    for (cnt = key_len; cnt > 0; cnt >>= 1)
        if (cnt & 1) sha512_update(&ctx, B, 64); else sha512_update(&ctx, key, key_len);
    sha512_final(&ctx, A);
    /* DP = SHA512(key key_len times) -> P = key_len bytes repeating DP */
    sha512_init(&ctx); for (i = 0; i < key_len; i++) sha512_update(&ctx, key, key_len); sha512_final(&ctx, DP);
    for (i = 0; i + 64 <= key_len; i += 64) memcpy(Pbuf + i, DP, 64);
    memcpy(Pbuf + i, DP, key_len - i);
    /* DS = SHA512(salt (16 + A[0]) times) -> S = salt_len bytes repeating DS */
    sha512_init(&ctx); for (i = 0; i < 16U + A[0]; i++) sha512_update(&ctx, salt, salt_len); sha512_final(&ctx, DS);
    for (i = 0; i + 64 <= salt_len; i += 64) memcpy(Sbuf + i, DS, 64);
    memcpy(Sbuf + i, DS, salt_len - i);
    /* the stretch loop (rounds; default 5000) */
    memcpy(C, A, 64);
    for (unsigned long r = 0; r < rounds; r++) {
        sha512_init(&ctx);
        if (r & 1) sha512_update(&ctx, Pbuf, key_len); else sha512_update(&ctx, C, 64);
        if (r % 3) sha512_update(&ctx, Sbuf, salt_len);
        if (r % 7) sha512_update(&ctx, Pbuf, key_len);
        if (r & 1) sha512_update(&ctx, C, 64); else sha512_update(&ctx, Pbuf, key_len);
        sha512_final(&ctx, C);
    }
    /* assemble "$6$[rounds=N$]salt$" + the permuted base-64 of C */
    char *cp = result; *cp++ = '$'; *cp++ = '6'; *cp++ = '$';
    if (rounds_custom) {
        char num[16]; int ni = 0; unsigned long rr = rounds;
        char tmp[16]; int ti = 0; if (rr == 0) tmp[ti++] = '0';
        while (rr) { tmp[ti++] = (char)('0' + rr % 10); rr /= 10; }
        while (ti) num[ni++] = tmp[--ti];
        num[ni] = '\0';
        const char *rp = "rounds="; while (*rp) *cp++ = *rp++;
        for (int k = 0; k < ni; k++) *cp++ = num[k];
        *cp++ = '$';
    }
    memcpy(cp, salt, salt_len); cp += salt_len; *cp++ = '$';
    cp = b64_24(cp, C[0],  C[21], C[42], 4); cp = b64_24(cp, C[22], C[43], C[1],  4);
    cp = b64_24(cp, C[44], C[2],  C[23], 4); cp = b64_24(cp, C[3],  C[24], C[45], 4);
    cp = b64_24(cp, C[25], C[46], C[4],  4); cp = b64_24(cp, C[47], C[5],  C[26], 4);
    cp = b64_24(cp, C[6],  C[27], C[48], 4); cp = b64_24(cp, C[28], C[49], C[7],  4);
    cp = b64_24(cp, C[50], C[8],  C[29], 4); cp = b64_24(cp, C[9],  C[30], C[51], 4);
    cp = b64_24(cp, C[31], C[52], C[10], 4); cp = b64_24(cp, C[53], C[11], C[32], 4);
    cp = b64_24(cp, C[12], C[33], C[54], 4); cp = b64_24(cp, C[34], C[55], C[13], 4);
    cp = b64_24(cp, C[56], C[14], C[35], 4); cp = b64_24(cp, C[15], C[36], C[57], 4);
    cp = b64_24(cp, C[37], C[58], C[16], 4); cp = b64_24(cp, C[59], C[17], C[38], 4);
    cp = b64_24(cp, C[18], C[39], C[60], 4); cp = b64_24(cp, C[40], C[61], C[19], 4);
    cp = b64_24(cp, C[62], C[20], C[41], 4); cp = b64_24(cp, 0,     0,     C[63], 2);
    *cp = '\0';
    return result;
}

/* --- system identity: uname() reports AIOS, NOT the host. A guest sees the AIOS kernel's identity
 *     (sysname "AIOS", release = the AIOS version), proving the program runs on AIOS, not Linux. Pure
 *     libaios (AIOS constants); no host call. struct utsname MUST match the shadow <sys/utsname.h>. --- */
struct utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
static void uts_set(char *d, const char *s) { size_t i = 0; for (; s[i] && i < 64; i++) d[i] = s[i]; d[i] = 0; }
int uname(struct utsname *u) {
    uts_set(u->sysname, "AIOS");
    uts_set(u->nodename, "aios");                      /* /etc/hostname overrides if present */
    { FILE *f = fopen("/etc/hostname", "r");
      if (f) { char b[65]; if (fgets(b, sizeof b, f)) {
          size_t n = strlen(b); while (n && (b[n-1] == '\n' || b[n-1] == '\r')) b[--n] = 0;
          if (b[0]) uts_set(u->nodename, b); } fclose(f); } }
    uts_set(u->release, AIOS_VERSION_STR);
    uts_set(u->version, AIOS_VERSION_LINE);
#if defined(__aarch64__)
    uts_set(u->machine, "aarch64");
#elif defined(__x86_64__)
    uts_set(u->machine, "x86_64");
#else
    uts_set(u->machine, "unknown");
#endif
    uts_set(u->domainname, "(none)");
    return 0;
}

/* ttyname: AIOS has no /dev/pts namespace, so a terminal fd reports the console device (a documented
 * simplification); a non-terminal fd is ENOTTY. `tty` uses this. */
char *ttyname(int fd) {
    static char dev[] = "/dev/console";
    if (isatty(fd)) return dev;
    errno = AIOS_ENOTTY; return 0;
}

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

/* --- signals: REGISTERED with the kernel, which DELIVERS them by running the handler (the kernel
 * saves/restores the guest's regs around it; the handler returns through __aios_sigtramp, which
 * issues AIOS_SYS_SIGRETURN). dash's trap/kill + interactive interrupts ride this. --- */
typedef void (*aios_sighandler)(int);
typedef struct { aios_sighandler sa_handler; unsigned long sa_mask; int sa_flags; } aios_sigaction;
extern void __aios_sigtramp(void);                 /* the sigreturn trampoline (asm, below) */
_Static_assert(AIOS_SYS_SIGRETURN == 0x101E, "the trampoline's hard-coded SIGRETURN nr must match");

aios_sighandler signal(int sig, aios_sighandler h) {
    long old = asys(AIOS_SYS_SIGACTION, sig, (long)h, (long)__aios_sigtramp);
    if (AIOS_IS_ERR(old)) { errno = (int)-old; return (aios_sighandler)(long)-1; }   /* SIG_ERR */
    return (aios_sighandler)old;
}
int sigaction(int sig, const aios_sigaction *act, aios_sigaction *old) {
    if (!act) { if (old) { old->sa_handler = 0; old->sa_mask = 0; old->sa_flags = 0; } return 0; }
    long prev = asys(AIOS_SYS_SIGACTION, sig, (long)act->sa_handler, (long)__aios_sigtramp);
    if (AIOS_IS_ERR(prev)) { errno = (int)-prev; return -1; }
    if (old) { old->sa_handler = (aios_sighandler)prev; old->sa_mask = 0; old->sa_flags = 0; }
    return 0;
}
int kill(int pid, int sig) { return (int)__ret(asys(AIOS_SYS_KILL, pid, sig, 0)); }
int killpg(int pgrp, int sig) { return kill(-pgrp, sig); }   /* signal the whole process group */
int raise(int sig)         { return kill(getpid(), sig); }
unsigned int alarm(unsigned int sec) { (void)sec; return 0; }
/* signal sets + masking are no-ops (no pending/blocked model yet). */
int sigemptyset(unsigned long *set) { if (set) *set = 0; return 0; }
int sigfillset(unsigned long *set)  { if (set) *set = ~0UL; return 0; }
int sigaddset(unsigned long *set, int s) { if (set) *set |= (1UL << (s & 63)); return 0; }
int sigdelset(unsigned long *set, int s) { if (set) *set &= ~(1UL << (s & 63)); return 0; }
int sigismember(const unsigned long *set, int s) { return set ? (int)((*set >> (s & 63)) & 1) : 0; }
int sigprocmask(int how, const unsigned long *set, unsigned long *old) {
    return (int)__ret(asys(AIOS_SYS_SIGPROCMASK, how, (long)set, (long)old));
}
int sigsuspend(const unsigned long *mask) { (void)mask; return -1; }   /* nothing to wait for */
char *strsignal(int sig) {
    static const char *n[] = { 0, "Hangup", "Interrupt", "Quit", "Illegal instruction",
        "Trace/breakpoint trap", "Aborted", "Bus error", "Floating point exception", "Killed",
        "User defined signal 1", "Segmentation fault", "User defined signal 2", "Broken pipe",
        "Alarm clock", "Terminated", "Stack fault", "Child exited", "Continued",
        "Stopped (signal)", "Stopped", "Stopped (tty input)", "Stopped (tty output)",
        "Urgent I/O condition", "CPU time limit exceeded", "File size limit exceeded",
        "Virtual timer expired", "Profiling timer expired", "Window changed", "I/O possible",
        "Power failure", "Bad system call" };  /* index 20 = SIGTSTP -> "Stopped" (dash's job notice) */
    if (sig > 0 && sig < (int)(sizeof n / sizeof n[0])) return (char *)n[sig];
    return "Unknown signal";
}

/* gettimeofday: a REALTIME reader over clock_gettime (microsecond precision). ioctl: no terminal-
 * geometry source yet -> failure (dash uses it for terminal width; it degrades gracefully). */
struct aios_timeval { long tv_sec; long tv_usec; };
int gettimeofday(struct aios_timeval *tv, void *tz) {
    (void)tz;
    long long ts[2] = { 0, 0 };
    clock_gettime(AIOS_CLOCK_REALTIME, ts);
    if (tv) { tv->tv_sec = (long)ts[0]; tv->tv_usec = (long)(ts[1] / 1000); }
    return 0;
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

/* The sigreturn trampoline: a signal handler "returns" here (the kernel set x30/lr to it), and it
 * issues AIOS_SYS_SIGRETURN so the kernel restores the pre-signal registers. It traps via the same
 * Linux/aarch64 GATEWAY convention as asys() above -- x8 = AIOS_GATEWAY (so seccomp traps it), the
 * real AIOS number (SIGRETURN) in x9 -- stringified from the macros so they cannot drift. It never
 * returns -- the kernel resumes the guest elsewhere. */
#define _GW_STR(x)  #x
#define _GW_XSTR(x) _GW_STR(x)
__asm__(
    ".global __aios_sigtramp\n"
    "__aios_sigtramp:\n"
    "  mov x8, #" _GW_XSTR(AIOS_GATEWAY) "\n"        /* gateway in x8 */
    "  mov x9, #" _GW_XSTR(AIOS_SYS_SIGRETURN) "\n"  /* real AIOS number in x9 */
    "  svc #0\n"
    "  b __aios_sigtramp\n"
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

/* ===== POSIX regular expressions: regcomp / regexec / regfree / regerror =====
 *
 * A small, self-contained BRE/ERE engine, added to libaios so vendored `grep` runs UNMODIFIED (grep
 * is the first regex-using utility; sbase is never patched -- the missing libc feature grows here).
 *
 * Design (correctness/termination is the soul of the project): parse the pattern to a small AST,
 * compile the AST to a Thompson NFA program (a flat instruction array), and MATCH by NFA SIMULATION
 * (a Pike-style two-thread-list sweep) -- LINEAR in the input length, with NO catastrophic
 * backtracking and a guaranteed halt: a per-step visited stamp dedups program counters, so even
 * empty-loop constructs like (a*)* cannot spin. The search is unanchored via an implicit leading
 * ".*"; ^ and $ stay real anchors because the assertions test the ABSOLUTE input position.
 *
 * Supported: literals, '.', bracket expressions ([abc], ranges, [^..] negation, POSIX [:class:]),
 * '^' '$' anchors, '\<' '\>' word boundaries, grouping, '|' alternation, the '*' '+' '?' quantifiers
 * and '{m,n}' intervals, and REG_ICASE. BRE vs ERE per REG_EXTENDED, with the GNU-ish leniencies
 * grep relies on (a quantifier with nothing to repeat is a literal; \< \> \+ \? \| work in BRE).
 *
 * Scope (honest): boolean match only. grep compiles REG_NOSUB and never inspects pmatch, and no
 * other vendored utility needs submatch capture yet, so regexec sets pmatch[*] = {-1,-1}. \1..\9
 * backreferences are rejected (REG_ESUBREG) -- a backtracking engine would be needed and nothing in
 * scope uses them. The day a util needs captures, this gains a Pike-VM save layer; the parser stays. */

/* libaios.c is its own translation unit and never includes the shadow <regex.h> (it is compiled
 * under two include regimes -- the -nostdinc libc class AND the plain prog class -- so it stays
 * self-contained, just like struct _IO_FILE above). These mirror lib/include/regex.h EXACTLY: the
 * program TU that calls regcomp passes a regex_t laid out by the shadow header, so the layouts MUST
 * agree. Guarded so an accidental shadow-header include would not redefine. */
#ifndef _REGEX_H
typedef long regoff_t;
typedef struct { size_t re_nsub; void *__impl; } regex_t;
typedef struct { regoff_t rm_so; regoff_t rm_eo; } regmatch_t;
#define REG_EXTENDED  1
#define REG_ICASE     2
#define REG_NOSUB     4
#define REG_NEWLINE   8
#define REG_NOTBOL    1
#define REG_NOTEOL    2
#define REG_NOMATCH   1
#define REG_BADPAT    2
#define REG_ECOLLATE  3
#define REG_ECTYPE    4
#define REG_EESCAPE   5
#define REG_ESUBREG   6
#define REG_EBRACK    7
#define REG_EPAREN    8
#define REG_EBRACE    9
#define REG_BADBR    10
#define REG_ERANGE   11
#define REG_ESPACE   12
#define REG_BADRPT   13
#endif

enum {                 /* AST node kinds */
    RXN_CHAR, RXN_ANY, RXN_SET, RXN_BOL, RXN_EOL, RXN_WSTART, RXN_WEND,
    RXN_CAT, RXN_ALT, RXN_STAR, RXN_PLUS, RXN_QUEST, RXN_REP, RXN_EMPTY
};
typedef struct rx_node {
    int kind;
    int ch;                 /* RXN_CHAR */
    unsigned char *set;     /* RXN_SET: 32-byte (256-bit) membership bitmap */
    struct rx_node *a, *b;  /* CAT/ALT children; unary ops use a */
    int min, max;           /* RXN_REP: max<0 == unbounded */
} rx_node;

enum {                 /* NFA instruction ops */
    RXI_CHAR, RXI_ANY, RXI_SET, RXI_MATCH, RXI_JMP, RXI_SPLIT,
    RXI_BOL, RXI_EOL, RXI_WSTART, RXI_WEND
};
typedef struct { int op; int c; unsigned char *set; int x, y; } rx_inst;

typedef struct {       /* the compiled program, hung off regex_t.__impl */
    rx_inst *prog;
    int      nprog;
    int      icase;
    int     *clist, *nlist;   /* reused thread scratch (free() is a no-op, so never alloc per line) */
    int     *seen;            /* per-step visited stamps */
    unsigned gen;             /* monotone stamp; seen[i]==gen means "added this step" */
} rx_prog;

typedef struct {       /* parser state */
    const char *p;
    int ere, icase, depth, ngroup, err;
} rx_parser;

typedef struct { rx_inst *prog; int n, cap, err; } rx_emit;

/* ---- AST ---- */
static rx_node *rxn_new(rx_parser *ps, int kind) {
    rx_node *n = malloc(sizeof *n);
    if (!n) { ps->err = REG_ESPACE; return 0; }
    memset(n, 0, sizeof *n);
    n->kind = kind;
    return n;
}
static rx_node *rxn_char(rx_parser *ps, int c) { rx_node *n = rxn_new(ps, RXN_CHAR); if (n) n->ch = c & 0xff; return n; }
static rx_node *rxn_unary(rx_parser *ps, int kind, rx_node *a) { rx_node *n = rxn_new(ps, kind); if (n) n->a = a; return n; }
static void rxn_free(rx_node *n) {
    if (!n) return;
    rxn_free(n->a); rxn_free(n->b);
    if (n->set) free(n->set);
    free(n);
}

/* ---- parser ---- */
static rx_node *rx_parse_alt(rx_parser *ps);

static int rx_branch_end(rx_parser *ps) {        /* current char ends a branch (top level or a group/alt) */
    if (*ps->p == '\0') return 1;
    /* '|' is always a separator (top-level alternation is valid); a ')' ends a branch only inside a
     * group -- a ')' with no open group is an ordinary literal (GNU-ish). */
    if (ps->ere) return *ps->p == '|' || (*ps->p == ')' && ps->depth > 0);
    if (ps->p[0] == '\\' && (ps->p[1] == '|' || (ps->p[1] == ')' && ps->depth > 0))) return 1;
    return 0;
}

static void rx_set_add(unsigned char *set, int c, int icase) {
    c &= 0xff;
    set[c >> 3] |= (unsigned char)(1u << (c & 7));
    if (icase) {
        int o = (c >= 'A' && c <= 'Z') ? c + 32 : (c >= 'a' && c <= 'z') ? c - 32 : c;
        if (o != c) set[o >> 3] |= (unsigned char)(1u << (o & 7));
    }
}

static int rx_class_fill(const char *name, unsigned char *set) {
    int (*fn)(int) = 0;
    if      (!strcmp(name, "alpha"))  fn = isalpha;
    else if (!strcmp(name, "digit"))  fn = isdigit;
    else if (!strcmp(name, "alnum"))  fn = isalnum;
    else if (!strcmp(name, "upper"))  fn = isupper;
    else if (!strcmp(name, "lower"))  fn = islower;
    else if (!strcmp(name, "space"))  fn = isspace;
    else if (!strcmp(name, "blank"))  fn = isblank;
    else if (!strcmp(name, "punct"))  fn = ispunct;
    else if (!strcmp(name, "cntrl"))  fn = iscntrl;
    else if (!strcmp(name, "print"))  fn = isprint;
    else if (!strcmp(name, "graph"))  fn = isgraph;
    else if (!strcmp(name, "xdigit")) fn = isxdigit;
    else return 0;
    for (int x = 0; x < 256; x++) if (fn(x)) rx_set_add(set, x, 0);
    return 1;
}

static rx_node *rx_parse_bracket(rx_parser *ps) {
    ps->p++;                                            /* past '[' */
    rx_node *node = rxn_new(ps, RXN_SET);
    if (!node) return 0;
    unsigned char *set = malloc(32);
    if (!set) { ps->err = REG_ESPACE; return 0; }
    memset(set, 0, 32);
    node->set = set;
    int neg = 0;
    if (*ps->p == '^') { neg = 1; ps->p++; }
    int first = 1;                                      /* a ']' as the first member is a literal ']' */
    for (;;) {
        int c = (unsigned char)*ps->p;
        if (c == '\0') { ps->err = REG_EBRACK; return 0; }
        if (c == ']' && !first) { ps->p++; break; }
        first = 0;
        if (c == '[' && ps->p[1] == ':') {             /* POSIX [:class:] */
            ps->p += 2;
            char name[16]; int ni = 0;
            while (*ps->p && *ps->p != ':' && ni < 15) name[ni++] = *ps->p++;
            name[ni] = '\0';
            if (!(ps->p[0] == ':' && ps->p[1] == ']')) { ps->err = REG_ECTYPE; return 0; }
            ps->p += 2;
            if (!rx_class_fill(name, set)) { ps->err = REG_ECTYPE; return 0; }
            continue;
        }
        if (c == '[' && (ps->p[1] == '.' || ps->p[1] == '=')) {  /* [.x.] / [=x=]: take the single char */
            char close = ps->p[1];
            ps->p += 2;
            int cc = (unsigned char)*ps->p;
            if (cc == '\0') { ps->err = REG_EBRACK; return 0; }
            ps->p++;
            if (!(ps->p[0] == close && ps->p[1] == ']')) { ps->err = REG_ECOLLATE; return 0; }
            ps->p += 2;
            rx_set_add(set, cc, ps->icase);
            continue;
        }
        ps->p++;
        if (*ps->p == '-' && ps->p[1] != ']' && ps->p[1] != '\0') {   /* a range c-hi */
            ps->p++;
            int hi = (unsigned char)*ps->p;
            ps->p++;
            if (hi < c) { ps->err = REG_ERANGE; return 0; }
            for (int x = c; x <= hi; x++) rx_set_add(set, x, ps->icase);
        } else {
            rx_set_add(set, c, ps->icase);
        }
    }
    if (neg) for (int i = 0; i < 32; i++) set[i] = (unsigned char)~set[i];
    return node;
}

static int rx_interval_ahead(rx_parser *ps) {
    const char *q = ps->p;
    if (ps->ere) { if (*q != '{') return 0; q++; }
    else { if (!(q[0] == '\\' && q[1] == '{')) return 0; q += 2; }
    return isdigit((unsigned char)*q) || *q == ',';
}

static rx_node *rx_parse_interval(rx_parser *ps, rx_node *atom) {
    if (ps->ere) ps->p++; else ps->p += 2;             /* past '{' or '\{' */
    int m = 0, n = 0, haveM = 0, haveComma = 0, haveN = 0;
    while (isdigit((unsigned char)*ps->p)) { m = m * 10 + (*ps->p++ - '0'); haveM = 1; if (m > 255) { ps->err = REG_BADBR; return 0; } }
    if (*ps->p == ',') { haveComma = 1; ps->p++;
        while (isdigit((unsigned char)*ps->p)) { n = n * 10 + (*ps->p++ - '0'); haveN = 1; if (n > 255) { ps->err = REG_BADBR; return 0; } }
    }
    if (ps->ere) { if (*ps->p != '}') { ps->err = REG_EBRACE; return 0; } ps->p++; }
    else { if (!(ps->p[0] == '\\' && ps->p[1] == '}')) { ps->err = REG_EBRACE; return 0; } ps->p += 2; }
    if (!haveM && !haveComma) { ps->err = REG_BADBR; return 0; }
    rx_node *rep = rxn_new(ps, RXN_REP);
    if (!rep) return 0;
    rep->a = atom;
    rep->min = haveM ? m : 0;
    rep->max = !haveComma ? rep->min : (!haveN ? -1 : n);
    if (rep->max >= 0 && rep->max < rep->min) { ps->err = REG_BADBR; return 0; }
    return rep;
}

static rx_node *rx_parse_escape(rx_parser *ps) {
    ps->p++;                                           /* past '\' */
    int c = (unsigned char)*ps->p;
    if (c == '\0') { ps->err = REG_EESCAPE; return 0; }
    ps->p++;
    if (!ps->ere) {                                    /* BRE-only escapes */
        if (c == '(') {
            ps->depth++;
            rx_node *in = rx_parse_alt(ps);
            if (!in) return 0;
            if (!(ps->p[0] == '\\' && ps->p[1] == ')')) { ps->err = REG_EPAREN; return 0; }
            ps->p += 2; ps->depth--; ps->ngroup++;
            return in;
        }
        if (c == ')') { ps->err = REG_EPAREN; return 0; }
        if (c >= '1' && c <= '9') { ps->err = REG_ESUBREG; return 0; }   /* backrefs unsupported */
    }
    if (c == '<') return rxn_new(ps, RXN_WSTART);
    if (c == '>') return rxn_new(ps, RXN_WEND);
    if (c == 'n') return rxn_char(ps, '\n');
    if (c == 't') return rxn_char(ps, '\t');
    if (c == 'r') return rxn_char(ps, '\r');
    return rxn_char(ps, c);                             /* \. \* \\ \( (ERE literal '(') ... */
}

static rx_node *rx_parse_atom(rx_parser *ps, int atstart) {
    int c = (unsigned char)*ps->p;
    if (c == '\\') return rx_parse_escape(ps);
    if (c == '.') { ps->p++; return rxn_new(ps, RXN_ANY); }
    if (c == '[') return rx_parse_bracket(ps);
    if (ps->ere) {
        if (c == '(') {
            ps->p++; ps->depth++;
            rx_node *in = rx_parse_alt(ps);
            if (!in) return 0;
            if (*ps->p != ')') { ps->err = REG_EPAREN; return 0; }
            ps->p++; ps->depth--; ps->ngroup++;
            return in;
        }
        if (c == '^') { ps->p++; return rxn_new(ps, RXN_BOL); }
        if (c == '$') { ps->p++; return rxn_new(ps, RXN_EOL); }
        ps->p++; return rxn_char(ps, c);               /* a stray ) * + ? { | here is a literal (GNU-ish) */
    }
    /* BRE: ^ anchors only at a branch start; $ only at a branch end; everything else (incl. a
     * leading *) is a literal -- a * that is a real quantifier is consumed by rx_parse_piece. */
    if (c == '^') { ps->p++; if (atstart) return rxn_new(ps, RXN_BOL); return rxn_char(ps, '^'); }
    if (c == '$') { ps->p++; if (rx_branch_end(ps)) return rxn_new(ps, RXN_EOL); return rxn_char(ps, '$'); }
    ps->p++; return rxn_char(ps, c);
}

static rx_node *rx_parse_piece(rx_parser *ps, int atstart) {
    rx_node *atom = rx_parse_atom(ps, atstart);
    if (!atom) return 0;
    int kind = -1;
    if (ps->ere) {
        if (*ps->p == '*') kind = RXN_STAR;
        else if (*ps->p == '+') kind = RXN_PLUS;
        else if (*ps->p == '?') kind = RXN_QUEST;
        else if (*ps->p == '{' && rx_interval_ahead(ps)) return rx_parse_interval(ps, atom);
        if (kind >= 0) ps->p++;
    } else {
        if (*ps->p == '*') { kind = RXN_STAR; ps->p++; }
        else if (ps->p[0] == '\\' && ps->p[1] == '+') { kind = RXN_PLUS; ps->p += 2; }   /* GNU BRE */
        else if (ps->p[0] == '\\' && ps->p[1] == '?') { kind = RXN_QUEST; ps->p += 2; }  /* GNU BRE */
        else if (ps->p[0] == '\\' && ps->p[1] == '{') return rx_parse_interval(ps, atom);
    }
    if (kind >= 0) return rxn_unary(ps, kind, atom);
    return atom;
}

static rx_node *rx_parse_cat(rx_parser *ps) {
    rx_node *left = 0;
    int first = 1;
    while (!rx_branch_end(ps)) {
        rx_node *pc = rx_parse_piece(ps, first);
        first = 0;
        if (!pc) return 0;
        if (!left) left = pc;
        else { rx_node *cat = rxn_new(ps, RXN_CAT); if (!cat) return 0; cat->a = left; cat->b = pc; left = cat; }
    }
    return left ? left : rxn_new(ps, RXN_EMPTY);
}

static rx_node *rx_parse_alt(rx_parser *ps) {
    rx_node *left = rx_parse_cat(ps);
    if (!left) return 0;
    for (;;) {
        if (ps->ere && *ps->p == '|') ps->p++;
        else if (!ps->ere && ps->p[0] == '\\' && ps->p[1] == '|') ps->p += 2;
        else break;
        rx_node *right = rx_parse_cat(ps);
        if (!right) return 0;
        rx_node *alt = rxn_new(ps, RXN_ALT);
        if (!alt) return 0;
        alt->a = left; alt->b = right; left = alt;
    }
    return left;
}

/* ---- emit AST -> NFA program ---- */
static int rx_emit_inst(rx_emit *e, int op, int c, unsigned char *set) {
    if (e->n >= e->cap) {
        int nc = e->cap ? e->cap * 2 : 32;
        rx_inst *np = realloc(e->prog, (size_t)nc * sizeof *np);
        if (!np) { e->err = REG_ESPACE; return -1; }
        e->prog = np; e->cap = nc;
    }
    int i = e->n++;
    e->prog[i].op = op; e->prog[i].c = c; e->prog[i].set = set; e->prog[i].x = e->prog[i].y = -1;
    return i;
}

static void rx_emit_node(rx_emit *e, rx_node *n) {
    if (e->err || !n) return;
    switch (n->kind) {
    case RXN_EMPTY: break;
    case RXN_CHAR: rx_emit_inst(e, RXI_CHAR, n->ch, 0); break;
    case RXN_ANY:  rx_emit_inst(e, RXI_ANY, 0, 0); break;
    case RXN_SET:  rx_emit_inst(e, RXI_SET, 0, n->set); n->set = 0; break;   /* ownership -> inst */
    case RXN_BOL:  rx_emit_inst(e, RXI_BOL, 0, 0); break;
    case RXN_EOL:  rx_emit_inst(e, RXI_EOL, 0, 0); break;
    case RXN_WSTART: rx_emit_inst(e, RXI_WSTART, 0, 0); break;
    case RXN_WEND:   rx_emit_inst(e, RXI_WEND, 0, 0); break;
    case RXN_CAT:  rx_emit_node(e, n->a); rx_emit_node(e, n->b); break;
    case RXN_ALT: {
        int sp = rx_emit_inst(e, RXI_SPLIT, 0, 0); if (sp < 0) return;
        e->prog[sp].x = e->n; rx_emit_node(e, n->a);
        int jmp = rx_emit_inst(e, RXI_JMP, 0, 0);
        e->prog[sp].y = e->n; rx_emit_node(e, n->b);
        if (jmp >= 0) e->prog[jmp].x = e->n;
        break;
    }
    case RXN_STAR: {
        int sp = rx_emit_inst(e, RXI_SPLIT, 0, 0); if (sp < 0) return;
        e->prog[sp].x = e->n; rx_emit_node(e, n->a);
        int jmp = rx_emit_inst(e, RXI_JMP, 0, 0);
        if (jmp >= 0) e->prog[jmp].x = sp;
        e->prog[sp].y = e->n;
        break;
    }
    case RXN_PLUS: {
        int start = e->n; rx_emit_node(e, n->a);
        int sp = rx_emit_inst(e, RXI_SPLIT, 0, 0); if (sp < 0) return;
        e->prog[sp].x = start; e->prog[sp].y = e->n;
        break;
    }
    case RXN_QUEST: {
        int sp = rx_emit_inst(e, RXI_SPLIT, 0, 0); if (sp < 0) return;
        e->prog[sp].x = e->n; rx_emit_node(e, n->a);
        e->prog[sp].y = e->n;
        break;
    }
    case RXN_REP: {
        for (int i = 0; i < n->min; i++) rx_emit_node(e, n->a);
        if (n->max < 0) {                              /* {min,} -> min copies then a star */
            int sp = rx_emit_inst(e, RXI_SPLIT, 0, 0); if (sp < 0) return;
            e->prog[sp].x = e->n; rx_emit_node(e, n->a);
            int jmp = rx_emit_inst(e, RXI_JMP, 0, 0);
            if (jmp >= 0) e->prog[jmp].x = sp;
            e->prog[sp].y = e->n;
        } else {                                       /* {min,max} -> (max-min) optional copies */
            int extra = n->max - n->min;
            int splits[256], ns = 0;
            for (int i = 0; i < extra && ns < 256; i++) {
                int sp = rx_emit_inst(e, RXI_SPLIT, 0, 0); if (sp < 0) return;
                e->prog[sp].x = e->n; splits[ns++] = sp;
                rx_emit_node(e, n->a);
            }
            int end = e->n;
            for (int i = 0; i < ns; i++) e->prog[splits[i]].y = end;
        }
        break;
    }
    }
}

/* ---- match: NFA simulation ---- */
static int rx_isword(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static void rx_addthread(rx_prog *rp, int *list, int *nlist, unsigned gen,
                         int pc, const char *s, int sp, int slen) {
    if (pc < 0 || rp->seen[pc] == (int)gen) return;
    rp->seen[pc] = (int)gen;
    rx_inst *in = &rp->prog[pc];
    switch (in->op) {
    case RXI_JMP:   rx_addthread(rp, list, nlist, gen, in->x, s, sp, slen); break;
    case RXI_SPLIT: rx_addthread(rp, list, nlist, gen, in->x, s, sp, slen);
                    rx_addthread(rp, list, nlist, gen, in->y, s, sp, slen); break;
    case RXI_BOL:   if (sp == 0)    rx_addthread(rp, list, nlist, gen, pc + 1, s, sp, slen); break;
    case RXI_EOL:   if (sp == slen) rx_addthread(rp, list, nlist, gen, pc + 1, s, sp, slen); break;
    case RXI_WSTART: {
        int before = sp > 0    ? rx_isword((unsigned char)s[sp - 1]) : 0;
        int here   = sp < slen ? rx_isword((unsigned char)s[sp])     : 0;
        if (!before && here) rx_addthread(rp, list, nlist, gen, pc + 1, s, sp, slen);
        break;
    }
    case RXI_WEND: {
        int before = sp > 0    ? rx_isword((unsigned char)s[sp - 1]) : 0;
        int here   = sp < slen ? rx_isword((unsigned char)s[sp])     : 0;
        if (before && !here) rx_addthread(rp, list, nlist, gen, pc + 1, s, sp, slen);
        break;
    }
    default: list[(*nlist)++] = pc; break;             /* CHAR/ANY/SET/MATCH: a real thread */
    }
}

static int rx_search(rx_prog *rp, const char *s, int slen) {
    int np = rp->nprog, cn = 0;
    int *cl = rp->clist, *nl = rp->nlist;
    unsigned gen = ++rp->gen;
    if (gen == 0) { for (int i = 0; i < np; i++) rp->seen[i] = 0; gen = ++rp->gen; }  /* wrap guard */
    rx_addthread(rp, cl, &cn, gen, 0, s, 0, slen);
    for (int sp = 0; ; sp++) {
        for (int i = 0; i < cn; i++) if (rp->prog[cl[i]].op == RXI_MATCH) return 1;
        if (sp >= slen) return 0;
        int c = (unsigned char)s[sp], nn = 0;
        gen = ++rp->gen;
        if (gen == 0) { for (int i = 0; i < np; i++) rp->seen[i] = 0; gen = ++rp->gen; }
        for (int i = 0; i < cn; i++) {
            rx_inst *in = &rp->prog[cl[i]];
            int adv = 0;
            switch (in->op) {
            case RXI_CHAR: adv = (c == in->c) || (rp->icase && tolower(c) == tolower(in->c)); break;
            case RXI_ANY:  adv = 1; break;
            case RXI_SET:  adv = (in->set[c >> 3] >> (c & 7)) & 1; break;
            default: break;
            }
            if (adv) rx_addthread(rp, nl, &nn, gen, cl[i] + 1, s, sp + 1, slen);
        }
        int *t = cl; cl = nl; nl = t; cn = nn;
    }
}

/* ---- public API ---- */
int regcomp(regex_t *preg, const char *pattern, int cflags) {
    rx_parser ps = { pattern, (cflags & REG_EXTENDED) ? 1 : 0, (cflags & REG_ICASE) ? 1 : 0, 0, 0, 0 };
    preg->__impl = 0; preg->re_nsub = 0;
    rx_node *root = rx_parse_alt(&ps);
    if (!ps.err && *ps.p != '\0') ps.err = REG_EPAREN;     /* unconsumed input -> unbalanced */
    if (ps.err) { rxn_free(root); return ps.err; }

    rx_emit e = { 0, 0, 0, 0 };
    int sp = rx_emit_inst(&e, RXI_SPLIT, 0, 0);            /* implicit leading ".*" (unanchored search) */
    int any = rx_emit_inst(&e, RXI_ANY, 0, 0);
    int jmp = rx_emit_inst(&e, RXI_JMP, 0, 0);
    if (e.err) { rxn_free(root); free(e.prog); return e.err; }
    e.prog[sp].x = any; e.prog[jmp].x = sp; e.prog[sp].y = e.n;
    rx_emit_node(&e, root);
    rx_emit_inst(&e, RXI_MATCH, 0, 0);
    rxn_free(root);
    if (e.err) {
        for (int i = 0; i < e.n; i++) if (e.prog[i].op == RXI_SET && e.prog[i].set) free(e.prog[i].set);
        free(e.prog);
        return e.err;
    }

    rx_prog *rp = malloc(sizeof *rp);
    if (rp) { rp->clist = malloc((size_t)e.n * sizeof(int));
              rp->nlist = malloc((size_t)e.n * sizeof(int));
              rp->seen  = malloc((size_t)e.n * sizeof(int)); }
    if (!rp || !rp->clist || !rp->nlist || !rp->seen) {
        for (int i = 0; i < e.n; i++) if (e.prog[i].op == RXI_SET && e.prog[i].set) free(e.prog[i].set);
        free(e.prog);
        if (rp) { free(rp->clist); free(rp->nlist); free(rp->seen); free(rp); }
        return REG_ESPACE;
    }
    for (int i = 0; i < e.n; i++) rp->seen[i] = 0;   /* 0 = the permanent "unvisited" sentinel; gen is always >=1 (never 0), so a stamp never collides with it -- the dedup is collision-free across all searches */
    rp->prog = e.prog; rp->nprog = e.n; rp->icase = ps.icase; rp->gen = 0;
    preg->re_nsub = (size_t)ps.ngroup;
    preg->__impl = rp;
    return 0;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags) {
    (void)eflags;                                          /* REG_NOTBOL/REG_NOTEOL: no in-scope caller sets them */
    rx_prog *rp = preg->__impl;
    for (size_t i = 0; i < nmatch; i++) { pmatch[i].rm_so = -1; pmatch[i].rm_eo = -1; }  /* no submatch capture */
    if (!rp) return REG_NOMATCH;
    return rx_search(rp, string, (int)strlen(string)) > 0 ? 0 : REG_NOMATCH;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size) {
    (void)preg;
    const char *msg;
    switch (errcode) {
    case 0:            msg = "Success"; break;
    case REG_NOMATCH:  msg = "No match"; break;
    case REG_BADPAT:   msg = "Invalid regular expression"; break;
    case REG_ECOLLATE: msg = "Invalid collating element"; break;
    case REG_ECTYPE:   msg = "Invalid character class"; break;
    case REG_EESCAPE:  msg = "Trailing backslash"; break;
    case REG_ESUBREG:  msg = "Invalid back reference"; break;
    case REG_EBRACK:   msg = "Unmatched [, [^, [:, [. or [="; break;
    case REG_EPAREN:   msg = "Unmatched ( or \\("; break;
    case REG_EBRACE:   msg = "Unmatched \\{"; break;
    case REG_BADBR:    msg = "Invalid content of \\{\\}"; break;
    case REG_ERANGE:   msg = "Invalid range end"; break;
    case REG_ESPACE:   msg = "Out of memory"; break;
    case REG_BADRPT:   msg = "Invalid preceding regular expression"; break;
    default:           msg = "Unknown regex error"; break;
    }
    size_t len = strlen(msg);
    if (errbuf_size) {
        size_t n = len < errbuf_size - 1 ? len : errbuf_size - 1;
        memcpy(errbuf, msg, n);
        errbuf[n] = '\0';
    }
    return len + 1;
}

void regfree(regex_t *preg) {
    rx_prog *rp = preg->__impl;
    if (!rp) return;
    for (int i = 0; i < rp->nprog; i++) if (rp->prog[i].op == RXI_SET && rp->prog[i].set) free(rp->prog[i].set);
    free(rp->prog); free(rp->clist); free(rp->nlist); free(rp->seen); free(rp);
    preg->__impl = 0;
}
