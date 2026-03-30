#ifndef _UNISTD_H
#define _UNISTD_H
#include <stddef.h>
#include <sys/types.h>

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);
int     unlink(const char *path);
int     rmdir(const char *path);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
pid_t   getpid(void);
int     isatty(int fd);
unsigned int sleep(unsigned int seconds);
int     usleep(unsigned long usec);

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

uid_t   getuid(void);
uid_t   geteuid(void);
gid_t   getgid(void);
gid_t   getegid(void);
pid_t   getppid(void);
int     access(const char *path, int amode);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);
int     pipe(int pipefd[2]);
int     execvp(const char *file, char *const argv[]);
pid_t   fork(void);
int     link(const char *oldpath, const char *newpath);
int     rename(const char *oldpath, const char *newpath);

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

#endif
