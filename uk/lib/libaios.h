/*
 * libaios.h -- a minimal C runtime for AIOS-ABI programs.
 *
 * Programs written as ordinary C (main + printf + malloc + argv) link against libaios and run on
 * the AIOS userspace kernel. libaios implements the C surface entirely on the AIOS ABI
 * (aios_abi.h) -- it NEVER calls the host kernel. This is the seed of the full libc that
 * recompiling sbase/dash will need (the AIOS-ABI retarget of the seL4 libaios_posix).
 *
 * Freestanding: no system headers except the compiler's own stddef/stdint/stdarg.
 */
#ifndef LIBAIOS_H
#define LIBAIOS_H

#include <stddef.h>
#include <stdint.h>

/* --- raw file I/O over the AIOS ABI (fd-based, like POSIX) --- */
long  aios_open(const char *path, int flags, int mode);
long  aios_read(int fd, void *buf, unsigned long len);
long  aios_write(int fd, const void *buf, unsigned long len);
int   aios_close(int fd);
void  aios_exit(int code) __attribute__((noreturn));

/* --- minimal libc --- */
size_t strlen(const char *s);
void  *memcpy(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
void  *malloc(size_t n);
void   free(void *p);                 /* bump allocator: free is a no-op for now */

int    puts(const char *s);           /* writes s + '\n' to stdout */
int    printf(const char *fmt, ...);  /* supports %s %d %u %x %c %% */

#endif /* LIBAIOS_H */
