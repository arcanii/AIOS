/* aios.h — AIOS sandbox program interface */
#ifndef AIOS_H
#define AIOS_H

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef unsigned long size_t;

typedef struct {
    /* Console I/O */
    void (*puts)(const char *s);
    void (*putc)(char c);
    void (*put_dec)(unsigned int n);
    void (*put_hex)(unsigned int n);

    /* Memory */
    void *(*malloc)(size_t size);
    void  (*free)(void *ptr);
    void *(*memcpy)(void *dst, const void *src, size_t n);
    void *(*memset)(void *dst, int c, size_t n);

    /* Strings */
    int   (*strlen)(const char *s);
    int   (*strcmp)(const char *a, const char *b);
    char *(*strcpy)(char *dst, const char *src);
    char *(*strncpy)(char *dst, const char *src, size_t n);

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
    void  (*exit_proc)(int status);  /* terminate current process */
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

    /* POSIX Threads (pthreads) */
    int   (*pthread_create)(unsigned long *thread, const void *attr,
                            void *(*start_routine)(void *), void *arg);
    int   (*pthread_join)(unsigned long thread, void **retval);
    int   (*pthread_detach)(unsigned long thread);
    void  (*pthread_exit)(void *retval);

    /* Mutex */
    int   (*pthread_mutex_init)(void *mutex, const void *attr);
    int   (*pthread_mutex_lock)(void *mutex);
    int   (*pthread_mutex_unlock)(void *mutex);
    int   (*pthread_mutex_destroy)(void *mutex);

    /* Condition variables */
    int   (*pthread_cond_init)(void *cond, const void *attr);
    int   (*pthread_cond_wait)(void *cond, void *mutex);
    int   (*pthread_cond_signal)(void *cond);
    int   (*pthread_cond_broadcast)(void *cond);

    /* Read-write locks */
    int   (*pthread_rwlock_init)(void *rwlock, const void *attr);
    int   (*pthread_rwlock_rdlock)(void *rwlock);
    int   (*pthread_rwlock_wrlock)(void *rwlock);
    int   (*pthread_rwlock_unlock)(void *rwlock);

    /* Thread-local storage */
    int   (*pthread_key_create)(unsigned int *key, void (*destructor)(void *));
    int   (*pthread_setspecific)(unsigned int key, const void *value);
    void *(*pthread_getspecific)(unsigned int key);

    /* Yield */
    void  (*sched_yield)(void);

    /* Privileged operations */
    int   (*shutdown)(int flags);
    int   (*sync)(void);

    /* Signal handling */
    unsigned long (*signal_handler)(int signum, unsigned long handler);
} aios_syscalls_t;

/* Global syscall pointer — set by _start */
#ifndef AIOS_NO_SYS_GLOBAL
static aios_syscalls_t *sys;
#endif

/* Buffered output macros */
/* POSIX identity macros */
#define getuid()       sys->getuid()
#define getgid()       sys->getgid()
#define geteuid()      sys->geteuid()
#define getegid()      sys->getegid()
#define getppid()      sys->getppid()
#define access(p,m)    sys->access(p,m)
#define umask(m)       sys->umask(m)
#define dup(fd)        sys->dup(fd)
#define dup2(a,b)      sys->dup2(a,b)
#define pipe(fds)      sys->pipe(fds)
#define time()         sys->time()
#define spawn(p,a)     sys->spawn(p,a)
#define waitpid(p,s)   sys->waitpid(p,s)
#define fork()         sys->fork()
#define kill_proc(p, s)   sys->kill_proc(p, s)

/* Process table entry (returned by getprocs) */
typedef struct {
    int pid;
    int parent_pid;
    int state;          /* 0=free,1=queued,2=ready,3=running,4=blocked,5=zombie */
    int uid;
    int slot;           /* sandbox slot, -1 if swapped */
    unsigned char foreground;
    unsigned char _reserved[3];
    char name[32];
    unsigned long long cpu_time;  /* arch timer ticks */
} proc_info_t;

#define PROC_STATE_FREE    0
#define PROC_STATE_QUEUED  1
#define PROC_STATE_READY   2
#define PROC_STATE_RUNNING 3
#define PROC_STATE_BLOCKED 4
#define PROC_STATE_ZOMBIE  5

#define getprocs(b, m)    sys->getprocs(b, m)
#define get_timer_freq()  sys->timer_freq()

/* Access mode constants */
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

/* Buffered output macros */
#define puts(s)         sys->puts(s)
#define putc(c)         sys->putc(c)
#define put_dec(n)      sys->put_dec(n)
#define sleep(s)        sys->sleep(s)
#define put_hex(n)      sys->put_hex(n)

/* Memory macros */
#define aios_malloc(n)  sys->malloc(n)
#define aios_free(p)    sys->free(p)
#define aios_memcpy     sys->memcpy
#define aios_memset     sys->memset
#define aios_strlen     sys->strlen
#define aios_strcmp      sys->strcmp
#define aios_strcpy     sys->strcpy
#define aios_strncpy    sys->strncpy

/* Backward-compatible short macros */
#define malloc(sz)      sys->malloc(sz)
#define free(p)         sys->free(p)
#define memcpy(d,s,n)   sys->memcpy(d,s,n)
#define memset(d,c,n)   sys->memset(d,c,n)
#define strlen(s)       sys->strlen(s)
#define strcmp(a,b)      sys->strcmp(a,b)
#define strcpy(d,s)     sys->strcpy(d,s)
#define strncpy(d,s,n)  sys->strncpy(d,s,n)

/* File I/O macros */
#define aios_open(path)          sys->open(path)
#define aios_read(fd, buf, len)  sys->read(fd, buf, len)
#define aios_write(fd, buf, len) sys->write_file(fd, buf, len)
#define aios_close(fd)           sys->close(fd)
#define aios_unlink(path)        sys->unlink(path)
#define aios_readdir(buf, max)   sys->readdir(buf, max)
#define aios_filesize()          sys->filesize()

/* Program arguments */
#define aios_args()              sys->args

/* Interactive I/O macros */
#define aios_getc()              sys->getc()
#define aios_puts_direct(s)      sys->puts_direct(s)
#define aios_putc_direct(c)      sys->putc_direct(c)

/* Entry point wrapper */
#define AIOS_ENTRY __attribute__((section(".text.zmain"))) int aios_main(void); __attribute__((section(".text._start"))) int _start(aios_syscalls_t *_sys) { sys = _sys; return aios_main(); } \
                   __attribute__((section(".text.zmain"))) int aios_main(void)


/* ---- POSIX Threads macros ---- */
#define aios_pthread_create(t,a,f,arg) sys->pthread_create(t,a,f,arg)
#define aios_pthread_join(t,r)         sys->pthread_join(t,r)
#define aios_pthread_detach(t)         sys->pthread_detach(t)
#define aios_pthread_exit(r)           sys->pthread_exit(r)
#define aios_mutex_init(m,a)           sys->pthread_mutex_init(m,a)
#define aios_mutex_lock(m)             sys->pthread_mutex_lock(m)
#define aios_mutex_unlock(m)           sys->pthread_mutex_unlock(m)
#define aios_mutex_destroy(m)          sys->pthread_mutex_destroy(m)
#define aios_cond_init(c,a)            sys->pthread_cond_init(c,a)
#define aios_cond_wait(c,m)            sys->pthread_cond_wait(c,m)
#define aios_cond_signal(c)            sys->pthread_cond_signal(c)
#define aios_cond_broadcast(c)         sys->pthread_cond_broadcast(c)
#define aios_sched_yield()             sys->sched_yield()

/* ---- System control ---- */
#define aios_shutdown(f)  sys->shutdown(f)
#define aios_sync()       sys->sync()

#endif /* AIOS_H */
