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
#ifndef __NR_statx
#define __NR_statx 291
#endif
#ifndef __NR_tgkill
#define __NR_tgkill 131
#endif

#ifndef __NR_pread64
#define __NR_pread64 67
#endif
#ifndef __NR_pwrite64
#define __NR_pwrite64 68
#endif
#ifndef __NR_fchmod
#define __NR_fchmod 52
#endif
#ifndef __NR_fchmodat
#define __NR_fchmodat 53
#endif
#ifndef __NR_fchown
#define __NR_fchown 55
#endif
#ifndef __NR_fchownat
#define __NR_fchownat 54
#endif
#ifndef __NR_linkat
#define __NR_linkat 37
#endif
#ifndef __NR_symlinkat
#define __NR_symlinkat 36
#endif
#ifndef __NR_readlinkat
#define __NR_readlinkat 78
#endif
#ifndef __NR_setuid
#define __NR_setuid 146
#endif
#ifndef __NR_setgid
#define __NR_setgid 144
#endif
#ifndef __NR_setsid
#define __NR_setsid 157
#endif
#ifndef __NR_getpgid
#define __NR_getpgid 155
#endif
#ifndef __NR_rt_sigreturn
#define __NR_rt_sigreturn 139
#endif
#ifndef __NR_sigaltstack
#define __NR_sigaltstack 132
#endif
#ifndef __NR_clock_nanosleep
#define __NR_clock_nanosleep 115
#endif
#ifndef __NR_mprotect
#define __NR_mprotect 226
#endif

#ifndef __NR_setpgid
#define __NR_setpgid 154
#endif

#ifndef __NR_futex
#define __NR_futex 98
#endif

/* M3: Socket syscall numbers (AArch64 musl) */
#ifndef __NR_listen
#define __NR_listen 201
#endif
#ifndef __NR_accept4
#define __NR_accept4 202
#endif
#ifndef __NR_socket
#define __NR_socket 198
#endif
#ifndef __NR_bind
#define __NR_bind 200
#endif
#ifndef __NR_sendto
#define __NR_sendto 206
#endif
#ifndef __NR_recvfrom
#define __NR_recvfrom 207
#endif
#ifndef __NR_setsockopt
#define __NR_setsockopt 208
#endif
#ifndef __NR_shutdown_sock
#define __NR_shutdown_sock 210
#endif
#ifndef __NR_connect
#define __NR_connect 203
#endif
#ifndef __NR_getsockname
#define __NR_getsockname 204
#endif
#ifndef __NR_getpeername
#define __NR_getpeername 205
#endif
#ifndef __NR_getsockopt
#define __NR_getsockopt 209
#endif

/* Net IPC labels (client side) */
#define NET_SOCKET_L      90
#define NET_BIND_L        91
#define NET_SENDTO_L      95
#define NET_RECVFROM_L    96
#define NET_LISTEN_L      92
#define NET_ACCEPT_L      93
#define NET_CONNECT_L     94
#define NET_CLOSE_SOCK_L  97


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
#define PIPE_SHUTDOWN     77
#define PIPE_DUP_REFS    82

/* fd table constants */
#define AIOS_MAX_FDS 32
#define AIOS_FD_BASE 10

typedef struct {
    int active;
    int is_dir;
    int is_pipe;
    int pipe_id;
    int pipe_read;
    int is_devnull;
    int is_append;
    int is_nonblock;          /* v0.4.79: O_NONBLOCK for pipes */
    int is_tty;               /* REDIR_FIX_V072: marks fd as terminal copy */
    int is_socket;            /* M3: fd is a network socket */
    int socket_id;            /* M3: index in net_server socket table (0-7) */
    char *shm_vaddr;              /* v0.4.66: SHM xfer page in this VSpace */
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
extern seL4_CPtr net_ep;
extern seL4_CPtr disp_ep;
extern seL4_CPtr crypto_ep;

extern char aios_cwd[256];
extern char aios_progpath[128];
extern uint32_t aios_uid;
extern uint32_t aios_gid;
extern int stdout_pipe_id;
extern int stdin_pipe_id;
extern int stdout_redir_idx;
extern int stderr_redir_idx;
extern aios_fd_t stdout_redir_copy;  /* REDIR_COPY_V072 */
extern aios_fd_t stderr_redir_copy;

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
int  fetch_pread(const char *path, int offset, char *buf, int count);
int  fetch_pwrite(const char *path, int offset, const char *data, int len);
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
long aios_sys_statx(va_list ap);

/* ---- Syscall handlers: posix_dir.c ---- */
long aios_sys_mkdirat(va_list ap);
long aios_sys_unlinkat(va_list ap);
long aios_sys_chdir(va_list ap);
long aios_sys_getcwd(va_list ap);
long aios_sys_getdents64(va_list ap);
long aios_sys_renameat(va_list ap);

/* ---- Syscall handlers: posix_net.c ---- */
long aios_sys_socket(va_list ap);
long aios_sys_bind(va_list ap);
long aios_sys_sendto(va_list ap);
long aios_sys_recvfrom(va_list ap);
long aios_sys_setsockopt(va_list ap);
long aios_sys_listen(va_list ap);
long aios_sys_accept4(va_list ap);
long aios_sys_shutdown_sock(va_list ap);
long aios_sys_connect(va_list ap);
long aios_sys_getsockname(va_list ap);
long aios_sys_getpeername(va_list ap);
long aios_sys_getsockopt(va_list ap);

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


/* ---- v0.4.62 extended syscall handlers ---- */

/* posix_file.c */
long aios_sys_pread64(va_list ap);
long aios_sys_pwrite64(va_list ap);

/* posix_stat.c */
long aios_sys_fchmod(va_list ap);
long aios_sys_fchmodat(va_list ap);
long aios_sys_fchown(va_list ap);
long aios_sys_fchownat(va_list ap);
long aios_sys_linkat(va_list ap);
long aios_sys_symlinkat(va_list ap);
long aios_sys_readlinkat(va_list ap);

/* posix_proc.c */
long aios_sys_setuid(va_list ap);
long aios_sys_setgid(va_list ap);
long aios_sys_setsid(va_list ap);
long aios_sys_getpgid(va_list ap);
long aios_sys_rt_sigreturn(va_list ap);
long aios_sys_sigaltstack(va_list ap);

/* posix_time.c */
long aios_sys_clock_nanosleep(va_list ap);

/* posix_misc.c */
long aios_sys_mprotect(va_list ap);

/* v0.4.64: dash prerequisites */
long aios_sys_setpgid(va_list ap);


/* ---- Syscall handlers: posix_compat.c (v0.4.78 Linux compat) ---- */
long aios_sys_ppoll(va_list ap);
long aios_sys_pselect6(va_list ap);
long aios_sys_getrandom(va_list ap);
long aios_sys_mremap(va_list ap);
long aios_sys_prlimit64(va_list ap);
long aios_sys_prctl(va_list ap);
long aios_sys_getrlimit(va_list ap);
long aios_sys_setrlimit(va_list ap);
long aios_sys_sysinfo(va_list ap);
long aios_sys_getrusage(va_list ap);
long aios_sys_membarrier(va_list ap);
long aios_sys_futex(va_list ap);

#endif
