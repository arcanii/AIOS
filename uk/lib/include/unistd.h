/* unistd.h -- AIOS shadow header (see sys/types.h). The POSIX process + I/O surface, implemented
 * by libaios on the AIOS ABI (fork/exec/wait/pipe/dup2 ride the kernel's process model). */
#ifndef _UNISTD_H
#define _UNISTD_H
#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
int     pipe(int fds[2]);
int     dup2(int oldfd, int newfd);
pid_t   fork(void);
int     execv(const char *path, char *const argv[]);
int     execvp(const char *file, char *const argv[]);
pid_t   getpid(void);
int     isatty(int fd);
int     unlink(const char *path);
int     unlinkat(int dirfd, const char *path, int flags);
int     rmdir(const char *path);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
int     access(const char *path, int amode);
int     faccessat(int dirfd, const char *path, int amode, int flags);
void    _exit(int code) __attribute__((noreturn));

/* access() / faccessat() modes (AIOS-owned; match AIOS_?_OK in aios_abi.h) */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

/* getopt + its standard globals (POSIX option parsing). */
extern char *optarg;
extern int   optind, opterr, optopt;
int     getopt(int argc, char *const argv[], const char *optstring);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif /* _UNISTD_H */
