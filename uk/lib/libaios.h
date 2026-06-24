/*
 * libaios.h -- a minimal C runtime for AIOS-ABI programs.
 *
 * Programs written as ordinary C (main + printf + malloc + argv + the usual string/ctype helpers)
 * link against libaios and run on the AIOS userspace kernel. libaios implements the C surface
 * entirely on the AIOS ABI (aios_abi.h) -- it NEVER calls the host kernel. This is the seed of the
 * full libc that recompiling sbase/dash will need (the AIOS-ABI retarget of the seL4 libaios_posix).
 *
 * Freestanding: no system headers except the compiler's own stddef/stdint/stdarg.
 */
#ifndef LIBAIOS_H
#define LIBAIOS_H

#include <stddef.h>
#include <stdint.h>
#include "aios_abi.h"   /* underlying ABI values (open flags, std fd numbers) */

/* POSIX-flavoured names so programs read like ordinary C (mapped onto the AIOS ABI). */
#define O_RDONLY AIOS_O_RDONLY
#define O_WRONLY AIOS_O_WRONLY
#define O_RDWR   AIOS_O_RDWR
#define O_CREAT  AIOS_O_CREAT
#define O_TRUNC  AIOS_O_TRUNC
#define O_APPEND AIOS_O_APPEND
#define STDIN_FILENO  AIOS_FD_STDIN
#define STDOUT_FILENO AIOS_FD_STDOUT
#define STDERR_FILENO AIOS_FD_STDERR

/* --- raw file I/O over the AIOS ABI (fd-based, like POSIX) --- */
long  aios_open(const char *path, int flags, int mode);
long  aios_read(int fd, void *buf, unsigned long len);
long  aios_write(int fd, const void *buf, unsigned long len);
int   aios_close(int fd);
long  aios_lseek(int fd, long off, int whence);
int   aios_fstat(int fd, struct aios_stat *st);
void  aios_exit(int code) __attribute__((noreturn));

/* --- process: replace the current image (returns only on failure, like POSIX exec) --- */
long  aios_execve(const char *path, char *const argv[], char *const envp[]);
long  aios_exec(const char *path, char *const argv[]);   /* execve with the inherited environment */
extern char **environ;                                    /* POSIX env, captured at _start */

/* --- process: fork / wait (the shell core: fork, child exec, parent wait) --- */
long  aios_fork(void);                       /* -> child pid (parent), 0 (child), -1 (error)      */
long  aios_wait(int *status);                /* wait for ANY child  -> reaped pid, or -1          */
long  aios_waitpid(long pid, int *status, int flags);   /* wait for `pid` (or -1/0 = any)         */
#define WEXITSTATUS(s) AIOS_WEXITSTATUS(s)   /* decode a wait status (normal-exit subset)         */

/* --- string / memory --- */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strcpy(char *d, const char *s);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
int    atoi(const char *s);

/* --- ctype --- */
int    isspace(int c);
int    isdigit(int c);

/* --- allocation (bump allocator for now; free is a no-op) --- */
void  *malloc(size_t n);
void   free(void *p);

/* --- output --- */
int    puts(const char *s);                 /* s + '\n' to stdout                       */
void   fdputs(int fd, const char *s);        /* s to an arbitrary fd                     */
int    printf(const char *fmt, ...);         /* to stdout: %s %d %u %x %c %%             */

#endif /* LIBAIOS_H */
