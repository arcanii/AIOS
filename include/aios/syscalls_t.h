#ifndef AIOS_SYSCALLS_T_H
#define AIOS_SYSCALLS_T_H

/*
 * aios_syscalls_t — shared between sandbox.c and user programs
 * Single source of truth for the syscall function pointer table.
 */
typedef struct {
    /* Console I/O */
    void  (*puts)(const char *s);
    void  (*putc)(char c);
    void (*put_dec)(unsigned int n);

    /* File I/O */
    int   (*open)(const char *path);
    int   (*open_flags)(const char *path, int flags);
    int   (*read)(int fd, void *buf, unsigned long len);
    int   (*write_file)(int fd, const void *buf, unsigned long len);
    int   (*close)(int fd);
    int   (*unlink)(const char *path);
    int   (*mkdir)(const char *path);
    int   (*rmdir)(const char *path);
    int   (*rename)(const char *oldpath, const char *newpath);
    int   (*exec)(const char *path, const char *args);
    int   (*readdir)(void *buf, unsigned long max_entries);
    int   (*filesize)(void);

    /* Extended POSIX */
    int   (*stat_file)(const char *path, unsigned long *size_out);
    int   (*stat_ex)(unsigned int *uid, unsigned int *gid, unsigned int *mode, unsigned int *mtime);
    int   (*lseek)(int fd, long offset, int whence);
    int   (*getcwd)(char *buf, unsigned long size);
    int   (*chdir)(const char *path);
    int   (*getpid)(void);

    /* Args */
    const char *args;

    /* Interactive I/O */
    int   (*getc)(void);
    void  (*puts_direct)(const char *s);
    void  (*putc_direct)(char c);
    int   (*sleep)(unsigned int seconds);

    /* POSIX extensions */
    int   (*getuid)(void);
    int   (*getgid)(void);
    int   (*geteuid)(void);
    int   (*getegid)(void);
    int   (*getppid)(void);
    int   (*access)(const char *path, int amode);
    int   (*umask)(int mask);
    int   (*dup)(int oldfd);
    int   (*dup2)(int oldfd, int newfd);
    int   (*pipe)(int pipefd[2]);
    long  (*time)(void);

    /* Process management */
    int   (*spawn)(const char *path, const char *args);
    int   (*waitpid)(int pid, int *status);
    int   (*fork)(void);

    int   (*chmod)(const char *path, unsigned int mode);
    int   (*chown)(const char *path, unsigned int uid, unsigned int gid);
    int   (*ftruncate)(int fd, unsigned long length);
    int   (*fcntl)(int fd, int cmd, int arg);
    int   (*kill_proc)(int pid, int sig);
    int   (*getprocs)(void *buf, int max_entries);
    unsigned long long (*timer_freq)(void);

    /* Sockets */
    int   (*socket)(int domain, int type, int protocol);
    int   (*connect)(int sockfd, const void *addr, int addrlen);
    int   (*bind)(int sockfd, const void *addr, int addrlen);
    int   (*listen)(int sockfd, int backlog);
    int   (*accept)(int sockfd, void *addr, int *addrlen);
    int   (*send)(int sockfd, const void *buf, unsigned long len, int flags);
    int   (*recv)(int sockfd, void *buf, unsigned long len, int flags);

    /* POSIX Threads */
    int   (*pthread_create)(unsigned long *thread, const void *attr,
                            void *(*start_routine)(void *), void *arg);
    int   (*pthread_join)(unsigned long thread, void **retval);
    int   (*pthread_detach)(unsigned long thread);
    void  (*pthread_exit)(void *retval);
    int   (*pthread_mutex_init)(void *mutex, const void *attr);
    int   (*pthread_mutex_lock)(void *mutex);
    int   (*pthread_mutex_unlock)(void *mutex);
    int   (*pthread_mutex_destroy)(void *mutex);
    int   (*pthread_cond_init)(void *cond, const void *attr);
    int   (*pthread_cond_wait)(void *cond, void *mutex);
    int   (*pthread_cond_signal)(void *cond);
    int   (*pthread_cond_broadcast)(void *cond);
    int   (*pthread_rwlock_init)(void *rwlock, const void *attr);
    int   (*pthread_rwlock_rdlock)(void *rwlock);
    int   (*pthread_rwlock_wrlock)(void *rwlock);
    int   (*pthread_rwlock_unlock)(void *rwlock);
    int   (*pthread_key_create)(unsigned int *key, void (*destructor)(void *));
    int   (*pthread_setspecific)(unsigned int key, const void *value);
    void *(*pthread_getspecific)(unsigned int key);
    void  (*sched_yield)(void);
} aios_syscalls_t;

#endif /* AIOS_SYSCALLS_T_H */
