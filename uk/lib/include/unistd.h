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
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
void    _exit(int code) __attribute__((noreturn));

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif /* _UNISTD_H */
