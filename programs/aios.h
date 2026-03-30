/* aios.h — AIOS sandbox program interface */
#ifndef AIOS_H
#define AIOS_H

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef unsigned long size_t;

typedef struct {
    /* Output (buffered until program exit) */
    void (*puts)(const char *s);
    void (*putc)(char c);
    void (*put_dec)(unsigned int n);
    void (*put_hex)(unsigned int n);
    /* Memory */
    void *(*malloc)(size_t size);
    void  (*free)(void *ptr);
    void *(*memcpy)(void *dst, const void *src, size_t n);
    void *(*memset)(void *dst, int c, size_t n);
    /* String */
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
    int   (*lseek)(int fd, long offset, int whence);
    int   (*getcwd)(char *buf, unsigned long size);
    int   (*chdir)(const char *path);
    int   (*getpid)(void);
    /* Args */
    const char *args;
    /* Interactive I/O (immediate, not buffered) */
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
    int   (*kill_proc)(int pid, int sig);
} aios_syscalls_t;

/* Global syscall pointer — set by _start */
static aios_syscalls_t *sys;

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
#define kill_proc(p, s)   sys->kill_proc(p, s)

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

#endif /* AIOS_H */
