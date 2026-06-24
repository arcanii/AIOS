/* stdio.h -- AIOS shadow header (see sys/types.h). MINIMAL for now: printf family + the basics.
 * The FILE* stream layer (fopen/fgets/fread/fwrite/fprintf with buffering) lands in the next step;
 * until then programs use unistd.h read/write for I/O and printf for formatted stdout. */
#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>

#ifndef NULL
#define NULL ((void *)0)
#endif
#define EOF (-1)

int printf(const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);

#endif /* _STDIO_H */
