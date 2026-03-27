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
#endif
