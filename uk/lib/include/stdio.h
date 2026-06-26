/* stdio.h -- AIOS shadow header (see sys/types.h). Buffered FILE* stdio, implemented in libaios:
 * stdout line-buffered, stderr unbuffered, opened files fully buffered; flushed on exit. */
#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>   /* ssize_t (getline/getdelim) */

#ifndef NULL
#define NULL ((void *)0)
#endif
#define EOF (-1)
#define BUFSIZ 1024
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

typedef struct _IO_FILE FILE;          /* opaque -- programs only ever use FILE* */
extern FILE *stdin, *stdout, *stderr;

FILE  *fopen(const char *path, const char *mode);
FILE  *fmemopen(void *buf, size_t size, const char *mode);   /* read-mode mem stream (grep -e/-f) */
int    fclose(FILE *f);
int    fflush(FILE *f);
size_t fread(void *ptr, size_t sz, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t sz, size_t nmemb, FILE *f);
int    fgetc(FILE *f);
int    getchar(void);
char  *fgets(char *s, int n, FILE *f);
ssize_t getline(char **lineptr, size_t *n, FILE *f);
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *f);
int    fputc(int c, FILE *f);
int    putchar(int c);
int    fputs(const char *s, FILE *f);
int    puts(const char *s);
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);
int    fileno(FILE *f);
void   perror(const char *s);
int    rename(const char *oldpath, const char *newpath);
int    remove(const char *path);

int    printf(const char *fmt, ...);
int    fprintf(FILE *f, const char *fmt, ...);
int    sprintf(char *buf, const char *fmt, ...);
int    snprintf(char *buf, size_t size, const char *fmt, ...);
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#define getc(f)    fgetc(f)
#define putc(c, f) fputc((c), (f))

#endif /* _STDIO_H */
