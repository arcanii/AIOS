#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Minimal FILE structure */
struct _FILE {
    int fd;
    int eof;
    int error;
};

static struct _FILE _stdin  = { .fd = 0, .eof = 0, .error = 0 };
static struct _FILE _stdout = { .fd = 1, .eof = 0, .error = 0 };
static struct _FILE _stderr = { .fd = 2, .eof = 0, .error = 0 };

FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

int errno;

/* Forward declarations for syscall layer */
extern long __aios_syscall(long num, long a0, long a1, long a2);
extern ssize_t write(int fd, const void *buf, size_t count);
extern ssize_t read(int fd, void *buf, size_t count);

/* ── vsnprintf (the core formatter) ─────────────────── */
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;

    #define PUTC(c) do { if (pos < size - 1) str[pos] = (c); pos++; } while(0)

    while (*fmt) {
        if (*fmt != '%') { PUTC(*fmt++); continue; }
        fmt++;

        /* Flags */
        int zero_pad = 0, left = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; }
        if (*fmt == '-') { left = 1; fmt++; }

        /* Width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        /* Length modifier */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') { is_long = 2; fmt++; } }

        /* Conversion */
        switch (*fmt) {
        case 'd': case 'i': {
            long val = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
            char buf[24]; int bi = 0;
            if (val < 0) { PUTC('-'); val = -val; width--; }
            if (val == 0) buf[bi++] = '0';
            else while (val > 0) { buf[bi++] = '0' + val % 10; val /= 10; }
            int pad = width - bi;
            if (!left) while (pad-- > 0) PUTC(zero_pad ? '0' : ' ');
            while (bi > 0) PUTC(buf[--bi]);
            if (left) while (pad-- > 0) PUTC(' ');
            break;
        }
        case 'u': {
            unsigned long val = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            char buf[24]; int bi = 0;
            if (val == 0) buf[bi++] = '0';
            else while (val > 0) { buf[bi++] = '0' + val % 10; val /= 10; }
            int pad = width - bi;
            if (!left) while (pad-- > 0) PUTC(zero_pad ? '0' : ' ');
            while (bi > 0) PUTC(buf[--bi]);
            if (left) while (pad-- > 0) PUTC(' ');
            break;
        }
        case 'x': case 'X': {
            unsigned long val = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            const char *hex = (*fmt == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
            char buf[20]; int bi = 0;
            if (val == 0) buf[bi++] = '0';
            else while (val > 0) { buf[bi++] = hex[val & 0xF]; val >>= 4; }
            int pad = width - bi;
            if (!left) while (pad-- > 0) PUTC(zero_pad ? '0' : ' ');
            while (bi > 0) PUTC(buf[--bi]);
            if (left) while (pad-- > 0) PUTC(' ');
            break;
        }
        case 'p': {
            unsigned long val = (unsigned long)va_arg(ap, void *);
            PUTC('0'); PUTC('x');
            char buf[20]; int bi = 0;
            if (val == 0) buf[bi++] = '0';
            else while (val > 0) { buf[bi++] = "0123456789abcdef"[val & 0xF]; val >>= 4; }
            while (bi > 0) PUTC(buf[--bi]);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = strlen(s);
            int pad = width - slen;
            if (!left) while (pad-- > 0) PUTC(' ');
            while (*s) PUTC(*s++);
            if (left) while (pad-- > 0) PUTC(' ');
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            PUTC(c);
            break;
        }
        case '%': PUTC('%'); break;
        default: PUTC('%'); PUTC(*fmt); break;
        }
        fmt++;
    }

    if (size > 0) str[pos < size ? pos : size - 1] = '\0';
    return (int)pos;
    #undef PUTC
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(str, 4096, fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) write(fp->fd, buf, n > (int)sizeof(buf) ? (int)sizeof(buf) : n);
    return n;
}

int fprintf(FILE *fp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(fp, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char *s) {
    int n = write(STDOUT_FILENO, s, strlen(s));
    write(STDOUT_FILENO, "\n", 1);
    return n + 1;
}

int putchar(int c) {
    char ch = (char)c;
    write(STDOUT_FILENO, &ch, 1);
    return c;
}

int fputc(int c, FILE *fp) {
    char ch = (char)c;
    write(fp->fd, &ch, 1);
    return c;
}

int fputs(const char *s, FILE *fp) {
    return (int)write(fp->fd, s, strlen(s));
}

int getchar(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) <= 0) return EOF;
    return (unsigned char)c;
}

int fgetc(FILE *fp) {
    char c;
    if (read(fp->fd, &c, 1) <= 0) { fp->eof = 1; return EOF; }
    return (unsigned char)c;
}

char *fgets(char *s, int size, FILE *fp) {
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(fp);
        if (c == EOF) { if (i == 0) return (void *)0; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

int feof(FILE *fp) { return fp->eof; }
int fflush(FILE *fp) { (void)fp; return 0; }


/* ── FILE stream I/O ─────────────────────────────────── */
extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern off_t lseek(int fd, off_t offset, int whence);

#define _FOPEN_MAX 16
static struct _FILE _file_pool[_FOPEN_MAX];
static int _file_pool_used[_FOPEN_MAX];

FILE *fopen(const char *path, const char *mode) {
    int flags = 0;
    if (mode[0] == 'r') {
        flags = 0x0000; /* O_RDONLY */
        if (mode[1] == '+') flags = 0x0002; /* O_RDWR */
    } else if (mode[0] == 'w') {
        flags = 0x0001 | 0x0040 | 0x0200; /* O_WRONLY | O_CREAT | O_TRUNC */
        if (mode[1] == '+') flags = 0x0002 | 0x0040 | 0x0200; /* O_RDWR | O_CREAT | O_TRUNC */
    } else if (mode[0] == 'a') {
        flags = 0x0001 | 0x0040 | 0x0400; /* O_WRONLY | O_CREAT | O_APPEND */
        if (mode[1] == '+') flags = 0x0002 | 0x0040 | 0x0400; /* O_RDWR | O_CREAT | O_APPEND */
    } else {
        return (FILE *)0;
    }

    int fd = open(path, flags);
    if (fd < 0) return (FILE *)0;

    /* Find a free slot */
    for (int i = 0; i < _FOPEN_MAX; i++) {
        if (!_file_pool_used[i]) {
            _file_pool_used[i] = 1;
            _file_pool[i].fd = fd;
            _file_pool[i].eof = 0;
            _file_pool[i].error = 0;
            return &_file_pool[i];
        }
    }
    close(fd);
    return (FILE *)0;
}

int fclose(FILE *fp) {
    if (!fp) return -1;
    int rc = close(fp->fd);
    fp->fd = -1;
    fp->eof = 1;
    /* Return to pool */
    for (int i = 0; i < _FOPEN_MAX; i++) {
        if (&_file_pool[i] == fp) {
            _file_pool_used[i] = 0;
            break;
        }
    }
    return rc;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    ssize_t n = read(fp->fd, ptr, total);
    if (n <= 0) {
        fp->eof = 1;
        return 0;
    }
    return (size_t)n / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    ssize_t n = write(fp->fd, ptr, total);
    if (n < 0) {
        fp->error = 1;
        return 0;
    }
    return (size_t)n / size;
}

int fseek(FILE *fp, long offset, int whence) {
    if (!fp) return -1;
    off_t r = lseek(fp->fd, (off_t)offset, whence);
    if (r < 0) return -1;
    fp->eof = 0;
    return 0;
}

long ftell(FILE *fp) {
    if (!fp) return -1;
    return (long)lseek(fp->fd, 0, 1); /* SEEK_CUR */
}
