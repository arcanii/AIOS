#ifndef POSIX_INTERNAL_H
#define POSIX_INTERNAL_H
/*
 * AIOS POSIX Shim -- internal header shared by all posix_*.c modules
 * v0.4.58: modularization of aios_posix.c
 */
#include "aios_posix.h"
#include <sel4/sel4.h>
#include <sel4runtime.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <bits/syscall.h>
#include "aios/aios_signal.h"

/* Syscall numbers not always defined on AArch64 */
#ifndef __NR_times
#define __NR_times 153
#endif
#ifndef __NR_getdents64
#define __NR_getdents64 61
#endif
#ifndef __NR_rt_sigaction
#define __NR_rt_sigaction 134
#endif
#ifndef __NR_rt_sigprocmask
#define __NR_rt_sigprocmask 135
#endif
#ifndef __NR_rt_sigpending
#define __NR_rt_sigpending 136
#endif
#ifndef __NR_kill
#define __NR_kill 129
#endif
#ifndef __NR_tgkill
#define __NR_tgkill 131
#endif

/* Pipe IPC protocol labels */
#define PIPE_FORK         65
#define PIPE_GETPID       66
#define PIPE_WAIT         67
#define PIPE_EXIT         68
#define PIPE_EXEC         69
#define PIPE_CLOSE_WRITE  70
#define PIPE_CLOSE_READ   73
#define PIPE_SIGNAL       75
#define PIPE_SIG_FETCH    76

/* fd table constants */
#define AIOS_MAX_FDS 32
#define AIOS_FD_BASE 10

typedef struct {
    int active;
    int is_dir;
    int is_pipe;
    int pipe_id;
    int pipe_read;
    char path[128];
    char data[4096];
    int size;
    int pos;
} aios_fd_t;

/* ---- Shared globals (defined in aios_posix.c) ---- */
extern seL4_CPtr ser_ep;
extern seL4_CPtr fs_ep_cap;
extern seL4_CPtr thread_ep;
extern seL4_CPtr auth_ep;
extern seL4_CPtr pipe_ep;

extern char aios_cwd[256];
extern uint32_t aios_uid;
extern uint32_t aios_gid;
extern int stdout_pipe_id;
extern int stdin_pipe_id;

extern aios_sigstate_t sigstate;
extern int sig_dispatching;

extern aios_fd_t aios_fds[AIOS_MAX_FDS];
extern int aios_pid;

/* ---- Shared helpers (defined in aios_posix.c) ---- */
static inline int str_len(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static inline void str_copy(char *d, const char *s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

void resolve_path(const char *pathname, char *out, int outsz);
int  aios_fd_alloc(void);
int  fetch_file(const char *path, char *buf, int bufsz);
int  fetch_stat(const char *path, uint32_t *mode, uint32_t *size);
int  fetch_dir_as_getdents(const char *path, char *buf, int bufsz);
size_t aios_stdio_write(void *data, size_t count);
int  aios_getchar(void);
int  aios_sig_check(void);

/* ---- Syscall handlers: posix_file.c ---- */
long aios_sys_open(va_list ap);
long aios_sys_openat(va_list ap);
long aios_sys_read(va_list ap);
long aios_sys_write(va_list ap);
long aios_sys_close(va_list ap);
long aios_sys_lseek(va_list ap);
long aios_sys_writev(va_list ap);
long aios_sys_readv(va_list ap);
long aios_sys_ftruncate(va_list ap);

/* ---- Syscall handlers: posix_stat.c ---- */
long aios_sys_fstat(va_list ap);
long aios_sys_fstatat(va_list ap);
long aios_sys_access(va_list ap);
long aios_sys_faccessat(va_list ap);

/* ---- Syscall handlers: posix_dir.c ---- */
long aios_sys_mkdirat(va_list ap);
long aios_sys_unlinkat(va_list ap);
long aios_sys_chdir(va_list ap);
long aios_sys_getcwd(va_list ap);
long aios_sys_getdents64(va_list ap);
long aios_sys_renameat(va_list ap);

/* ---- Syscall handlers: posix_proc.c ---- */
long aios_sys_exit(va_list ap);
long aios_sys_exit_group(va_list ap);
long aios_sys_getpid(va_list ap);
long aios_sys_getppid(va_list ap);
long aios_sys_getuid(va_list ap);
long aios_sys_geteuid(va_list ap);
long aios_sys_getgid(va_list ap);
long aios_sys_getegid(va_list ap);
long aios_sys_wait4(va_list ap);
long aios_sys_execve(va_list ap);
long aios_sys_clone(va_list ap);
long aios_sys_rt_sigaction(va_list ap);
long aios_sys_rt_sigprocmask(va_list ap);
long aios_sys_kill(va_list ap);
long aios_sys_tgkill(va_list ap);
long aios_sys_rt_sigpending(va_list ap);

/* ---- Syscall handlers: posix_time.c ---- */
long aios_sys_clock_gettime(va_list ap);
long aios_sys_gettimeofday(va_list ap);
long aios_sys_nanosleep(va_list ap);
long aios_sys_times(va_list ap);

/* ---- Syscall handlers: posix_misc.c ---- */
long aios_sys_utimensat(va_list ap);
long aios_sys_umask(va_list ap);
long aios_sys_uname(va_list ap);
long aios_sys_ioctl(va_list ap);
long aios_sys_fcntl(va_list ap);
long aios_sys_dup(va_list ap);
long aios_sys_dup3(va_list ap);
long aios_sys_pipe2(va_list ap);

#endif
