#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)
#define BUFSIZ 1024

typedef struct _FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *fp);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int   fseek(FILE *fp, long offset, int whence);
long  ftell(FILE *fp);
int   feof(FILE *fp);
int   fflush(FILE *fp);
char *fgets(char *s, int size, FILE *fp);
int   fputs(const char *s, FILE *fp);
int   fgetc(FILE *fp);
int   fputc(int c, FILE *fp);

int   printf(const char *fmt, ...);
int   fprintf(FILE *fp, const char *fmt, ...);
int   sprintf(char *str, const char *fmt, ...);
int   snprintf(char *str, size_t size, const char *fmt, ...);
int   vprintf(const char *fmt, va_list ap);
int   vfprintf(FILE *fp, const char *fmt, va_list ap);
int   vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

int   puts(const char *s);
int   putchar(int c);
int   getchar(void);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Needed for va_list — compiler builtin */
#ifndef _STDARG_H
#define _STDARG_H
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_copy(dst, src)  __builtin_va_copy(dst, src)
#endif

#endif
