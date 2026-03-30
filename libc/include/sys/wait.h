#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>

#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WTERMSIG(s)    ((s) & 0x7F)
#define WIFEXITED(s)   (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (WTERMSIG(s) != 0)
#define WIFSTOPPED(s)  (((s) & 0xFF) == 0x7F)
#define WSTOPSIG(s)    WEXITSTATUS(s)

#define WNOHANG   1
#define WUNTRACED 2

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#endif
