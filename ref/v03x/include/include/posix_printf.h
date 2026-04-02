#ifndef POSIX_PRINTF_H
#define POSIX_PRINTF_H

/* Printf family extracted from posix.h for single-inclusion. */
/* This avoids code duplication when posix.h is included by */
/* multiple translation units.                               */

static inline int fprintf(FILE *fp, const char *fmt, ...) {
    /* Minimal fprintf: supports %s, %d, %c, %x, %% */
    char buf[512];
    int pos = 0;
    const char *p = fmt;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    while (*p && pos < 510) {
        if (*p == '%') {
            p++;
            if (*p == 's') {
                const char *s = __builtin_va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && pos < 510) buf[pos++] = *s++;
            } else if (*p == 'd') {
                int v = __builtin_va_arg(ap, int);
                char tmp[12]; int ti = 0;
                if (v < 0) { buf[pos++] = '-'; v = -v; }
                if (v == 0) tmp[ti++] = '0';
                while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
                while (ti > 0 && pos < 510) buf[pos++] = tmp[--ti];
            } else if (*p == 'c') {
                int v = __builtin_va_arg(ap, int);
                buf[pos++] = (char)v;
            } else if (*p == 'x') {
                unsigned int v = __builtin_va_arg(ap, unsigned int);
                char tmp[9]; int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                while (v > 0) { tmp[ti++] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
                while (ti > 0 && pos < 510) buf[pos++] = tmp[--ti];
            } else if (*p == '%') {
                buf[pos++] = '%';
            }
            p++;
        } else {
            buf[pos++] = *p++;
        }
    }
    __builtin_va_end(ap);
    buf[pos] = '\0';
    return (int)fwrite(buf, 1, (size_t)pos, fp);
}

static inline int printf(const char *fmt, ...) {
    /* Route through fprintf to stdout */
    char buf[512];
    int pos = 0;
    const char *p = fmt;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    while (*p && pos < 510) {
        if (*p == '%') {
            p++;
            if (*p == 's') {
                const char *s = __builtin_va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && pos < 510) buf[pos++] = *s++;
            } else if (*p == 'd') {
                int v = __builtin_va_arg(ap, int);
                char tmp[12]; int ti = 0;
                if (v < 0) { buf[pos++] = '-'; v = -v; }
                if (v == 0) tmp[ti++] = '0';
                while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
                while (ti > 0 && pos < 510) buf[pos++] = tmp[--ti];
            } else if (*p == 'c') {
                int v = __builtin_va_arg(ap, int);
                buf[pos++] = (char)v;
            } else if (*p == 'x') {
                unsigned int v = __builtin_va_arg(ap, unsigned int);
                char tmp[9]; int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                while (v > 0) { tmp[ti++] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
                while (ti > 0 && pos < 510) buf[pos++] = tmp[--ti];
            } else if (*p == '%') {
                buf[pos++] = '%';
            }
            p++;
        } else {
            buf[pos++] = *p++;
        }
    }
    __builtin_va_end(ap);
    buf[pos] = '\0';
    write(STDOUT_FILENO, buf, (size_t)pos);
    return pos;
}

#endif /* POSIX_PRINTF_H */
