/* sys/wait.h -- AIOS shadow header (see sys/types.h). wait/waitpid over the kernel's process model.
 * Status encodes a normal exit as (code & 0xff) << 8 (a POSIX-shaped subset; signals come later). */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#include <sys/types.h>

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)    ((s) & 0x7f)
#define WNOHANG 1

#endif /* _SYS_WAIT_H */
