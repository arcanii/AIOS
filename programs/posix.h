/*
 * AIOS POSIX compatibility layer for sandbox programs
 *
 * Maps standard POSIX function signatures to the aios_syscalls_t
 * function pointer table. Include this AFTER aios.h.
 *
 * Usage:
 *   #include "aios.h"
 *   #include "posix.h"
 *   ...
 *   int fd = open("hello.txt", O_RDONLY);
 *   read(fd, buf, sizeof(buf));
 *   write(STDOUT_FILENO, buf, n);
 *   close(fd);
 */
#ifndef AIOS_POSIX_H
#define AIOS_POSIX_H

/* ── errno ───────────────────────────────────────────── */
static int _posix_errno = 0;
#define errno _posix_errno

#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define ENXIO        6
#define EBADF        9
#define ECHILD      10
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EEXIST      17
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENFILE      23
#define EMFILE      24
#define ENOSPC      28
#define EPIPE       32
#define ERANGE      34
#define ENOSYS      38
#define ENOTEMPTY   39
#define ENOTSOCK    88
#define ECONNREFUSED 111


#include "aios.h"

/* ── Types ───────────────────────────────────────────── */
typedef long          ssize_t;
typedef long          off_t;
typedef unsigned long ino_t;
typedef int           pid_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;

/* ── Signal constants ────────────────────────────────── */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGABRT    6
#define SIGFPE     8
#define SIGKILL    9
#define SIGSEGV   11
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
typedef unsigned int  mode_t;

/* ── File descriptors ────────────────────────────────── */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/* ── Open flags ──────────────────────────────────────── */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040
#define O_TRUNC    0x0200
#define O_APPEND   0x0400

/* ── Seek ────────────────────────────────────────────── */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* ── Stat ────────────────────────────────────────────── */
#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)

struct stat {
    ino_t    st_ino;
    mode_t   st_mode;
    off_t    st_size;
    unsigned st_uid;
    unsigned st_gid;
    unsigned st_mtime;
    unsigned st_nlink;
};

/* ── Directory ───────────────────────────────────────── */
#define NAME_MAX 255

struct dirent {
    ino_t  d_ino;
    unsigned char d_type;
    char   d_name[NAME_MAX + 1];
    unsigned long d_size;      /* AIOS extension: file size */
};

/* ── Program arguments ────────────────────────────────── */
#define posix_args()  (sys->args)

/* ── Console fd tracking ─────────────────────────────── */
/* fd 0-2 are console, fd 3+ are filesystem */
#define _FD_IS_CONSOLE(fd) ((fd) >= 0 && (fd) <= 2)

/* ── POSIX open() ────────────────────────────────────── */
static inline int open(const char *path, int flags, ...) {
    int fd = sys->open_flags(path, flags);
    if (fd < 0) return -1;
    /* Offset by 3 to reserve 0,1,2 for console */
    return fd + 3;
}

/* ── POSIX read() ────────────────────────────────────── */
static inline ssize_t read(int fd, void *buf, size_t count) {
    if (fd == STDIN_FILENO) {
        /* Console input: read one character at a time */
        int c = sys->getc();
        if (c < 0) { errno = EIO; return 0; }
        ((char *)buf)[0] = (char)c;
        return 1;
    }
    if (_FD_IS_CONSOLE(fd)) return 0;
    return (ssize_t)sys->read(fd - 3, buf, (unsigned long)count);
}

/* ── POSIX write() ───────────────────────────────────── */
static inline ssize_t write(int fd, const void *buf, size_t count) {
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        const char *s = (const char *)buf;
        for (size_t i = 0; i < count; i++) {
            sys->putc_direct(s[i]);
        }
        return (ssize_t)count;
    }
    if (_FD_IS_CONSOLE(fd)) return 0;
    return (ssize_t)sys->write_file(fd - 3, buf, (unsigned long)count);
}

/* ── POSIX close() ───────────────────────────────────── */
static inline int close(int fd) {
    if (_FD_IS_CONSOLE(fd)) return 0;
    return sys->close(fd - 3);
}

/* ── POSIX unlink() / remove() ───────────────────────── */
static inline int unlink(const char *path) {
    errno = 0;
    return sys->unlink(path);
}

static inline int mkdir(const char *path, mode_t mode) {
    errno = 0;
    (void)mode;
    return sys->mkdir(path);
}

static inline int rmdir(const char *path) {
    return sys->rmdir(path);
}

static inline int rename(const char *oldpath, const char *newpath) {
    return sys->rename(oldpath, newpath);
}

static inline int remove(const char *path) {
    return unlink(path);
}

/* ── POSIX stat() ────────────────────────────────────── */
static inline int stat(const char *path, struct stat *buf) {
    unsigned long sz = 0;
    int r = sys->stat_file(path, &sz);
    if (r < 0) { errno = ENOENT; return -1; }
    if (r == 0 && buf) {
        buf->st_size = (off_t)sz;
        buf->st_ino = 0;
        buf->st_nlink = 1;
        unsigned int u = 0, g = 0, m = 0;
        unsigned int mt = 0;
        sys->stat_ex(&u, &g, &m, &mt);
        buf->st_uid = u;
        buf->st_gid = g;
        buf->st_mode = (mode_t)m;
        buf->st_mtime = mt;
    }
    return r;
}

/* ── Directory operations ────────────────────────────── */

/* Simple directory listing using readdir syscall */
typedef struct {
    struct dirent entries[64];
    int count;
    int pos;
} DIR;

static DIR _posix_dir;

/* Forward declarations for opendir */
static inline int chdir(const char *path);
static inline char *getcwd(char *buf, size_t size);

static inline DIR *opendir(const char *path) {
    /* If path given and not ".", chdir there, readdir, chdir back */
    char _od_prev[256];
    int _od_need_restore = 0;
    if (path && path[0] && !(path[0] == '.' && path[1] == '\0')) {
        getcwd(_od_prev, sizeof(_od_prev));
        if (chdir(path) == 0) {
            _od_need_restore = 1;
        } else {
            return (void *)0;
        }
    }
    /* Raw buffer for variable-length entries */
    static unsigned char raw[3072];
    int count = sys->readdir(raw, sizeof(raw));
    if (count < 0) return (void *)0;
    if (count == 0) {
        _posix_dir.count = 0;
        _posix_dir.pos = 0;
        return &_posix_dir;
    }
    _posix_dir.count = 0;
    _posix_dir.pos = 0;
    /* Parse variable-length entries: [2B entry_len][1B name_len][1B type][4B size][name][NUL] */
    int off = 0;
    for (int i = 0; i < count && i < 64; i++) {
        unsigned short elen = raw[off] | (raw[off+1] << 8);
        if (elen == 0) break;
        unsigned char nlen = raw[off + 2];
        unsigned char dtype = raw[off + 3];
        /* skip type at off+3 */
        unsigned long fsize = raw[off+4] | (raw[off+5]<<8) | (raw[off+6]<<16) | (raw[off+7]<<24);
        _posix_dir.entries[i].d_size = fsize;
        int cplen = nlen < NAME_MAX ? nlen : NAME_MAX;
        for (int j = 0; j < cplen; j++)
            _posix_dir.entries[i].d_name[j] = (char)raw[off + 8 + j];
        _posix_dir.entries[i].d_name[cplen] = '\0';
        _posix_dir.entries[i].d_type = dtype;
        _posix_dir.count++;
        off += elen;
    }
    if (_od_need_restore) chdir(_od_prev);
    return &_posix_dir;
}

static inline struct dirent *readdir(DIR *dirp) {
    if (!dirp || dirp->pos >= dirp->count) return NULL;
    return &dirp->entries[dirp->pos++];
}

static inline int closedir(DIR *dirp) {
    if (dirp) dirp->pos = dirp->count = 0;
    return 0;
}

/* ── Stubs ───────────────────────────────────────────── */

static inline off_t lseek(int fd, off_t offset, int whence) {
    if (_FD_IS_CONSOLE(fd)) return -1;
    return (off_t)sys->lseek(fd - 3, (long)offset, whence);
}

static inline int aios_exec(const char *path, const char *args) {
    return sys->exec(path, args);
}

static inline int chdir(const char *path) {
    errno = 0;
    return sys->chdir(path);
}

static inline char *getcwd(char *buf, size_t size) {
    sys->getcwd(buf, (unsigned long)size);
    return buf;
}

static inline int isatty(int fd) {
    return _FD_IS_CONSOLE(fd) ? 1 : 0;
}

static inline pid_t getpid(void) { return (pid_t)sys->getpid(); }

static inline int kill(pid_t pid, int sig) {
    int (*fn)(int, int) = sys->kill_proc;
    return fn((int)pid, sig);
}

static inline int chmod(const char *path, mode_t mode) {
    return sys->chmod(path, (unsigned int)mode);
}

static inline int chown(const char *path, int owner, int group) {
    return sys->chown(path, (unsigned int)owner, (unsigned int)group);
}

static inline int ftruncate(int fd, off_t length) {
    return sys->ftruncate(fd, (unsigned long)length);
}

/* fcntl constants */
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define FD_CLOEXEC 1

static inline int fcntl(int fd, int cmd, ...) {
    return sys->fcntl(fd, cmd, 0);
}

/* ── printf family (minimal) ─────────────────────────── */
/* Programs already have puts/putc from aios.h; for POSIX
 * compatibility, printf goes through write(STDOUT_FILENO,...) */

static inline int posix_puts(const char *s) {
    int len = 0;
    while (s[len]) len++;
    write(STDOUT_FILENO, s, (size_t)len);
    write(STDOUT_FILENO, "\n", 1);
    return len + 1;
}



/* ── FILE stream I/O ─────────────────────────────────── */
typedef struct _posix_file {
    int fd;
    int eof;
    int err;
    int append;
} FILE;

#define _POSIX_FOPEN_MAX 8
static FILE _posix_files[_POSIX_FOPEN_MAX];
static int  _posix_files_used[_POSIX_FOPEN_MAX];
static FILE _posix_stdin  = { .fd = 0, .eof = 0, .err = 0, .append = 0 };
static FILE _posix_stdout = { .fd = 1, .eof = 0, .err = 0, .append = 0 };
static FILE _posix_stderr = { .fd = 2, .eof = 0, .err = 0, .append = 0 };
#define stdin  (&_posix_stdin)
#define stdout (&_posix_stdout)
#define stderr (&_posix_stderr)
#define EOF (-1)

static inline FILE *fopen(const char *path, const char *mode) {
    int flags = 0;
    if (mode[0] == 'r') {
        flags = O_RDONLY;
        if (mode[1] == '+') flags = O_RDWR;
    } else if (mode[0] == 'w') {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (mode[1] == '+') flags = O_RDWR | O_CREAT | O_TRUNC;
    } else if (mode[0] == 'a') {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        if (mode[1] == '+') flags = O_RDWR | O_CREAT | O_APPEND;
    } else {
        return (FILE *)0;
    }
    int fd = open(path, flags);
    if (fd < 0) { /* errno set by open */ return (FILE *)0; }
    for (int i = 0; i < _POSIX_FOPEN_MAX; i++) {
        if (!_posix_files_used[i]) {
            _posix_files_used[i] = 1;
            _posix_files[i].fd = fd;
            _posix_files[i].eof = 0;
            _posix_files[i].err = 0;
            _posix_files[i].append = (mode[0] == 'a') ? 1 : 0;
            return &_posix_files[i];
        }
    }
    close(fd);
    return (FILE *)0;
}

static inline int fclose(FILE *fp) {
    if (!fp) return -1;
    int rc = close(fp->fd);
    fp->fd = -1;
    fp->eof = 1;
    for (int i = 0; i < _POSIX_FOPEN_MAX; i++) {
        if (&_posix_files[i] == fp) { _posix_files_used[i] = 0; break; }
    }
    return rc;
}

static inline size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    ssize_t n = read(fp->fd, ptr, total);
    if (n <= 0) { fp->eof = 1; return 0; }
    return (size_t)n / size;
}

static inline size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    ssize_t n = write(fp->fd, ptr, total);
    if (n < 0) { fp->err = 1; return 0; }
    return (size_t)n / size;
}

static inline int fseek(FILE *fp, long offset, int whence) {
    if (!fp) return -1;
    off_t r = lseek(fp->fd, (off_t)offset, whence);
    if (r < 0) return -1;
    fp->eof = 0;
    return 0;
}

static inline long ftell(FILE *fp) {
    if (!fp) return -1;
    return (long)lseek(fp->fd, 0, SEEK_CUR);
}

static inline int feof(FILE *fp) { return fp ? fp->eof : 1; }
static inline int ferror(FILE *fp) { return fp ? fp->err : 1; }
static inline int fflush(FILE *fp) { (void)fp; return 0; }
static inline int fileno(FILE *fp) { return fp ? fp->fd : -1; }

static inline int fputc(int c, FILE *fp) {
    char ch = (char)c;
    if (fwrite(&ch, 1, 1, fp) == 1) return (unsigned char)c;
    return EOF;
}

static inline int fputs(const char *s, FILE *fp) {
    while (*s) { if (fputc(*s++, fp) == EOF) return EOF; }
    return 0;
}

static inline int fgetc(FILE *fp) {
    unsigned char c;
    if (fread(&c, 1, 1, fp) == 1) return (int)c;
    return EOF;
}

static inline char *fgets(char *s, int size, FILE *fp) {
    if (!s || size <= 0 || !fp) return (char *)0;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(fp);
        if (c == EOF) { if (i == 0) return (char *)0; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

static inline int fprintf(FILE *fp, const char *fmt, ...) {
    /* Minimal fprintf: supports %s, %d, %c, %x, %% */
    char buf[512];
    int pos = 0;
    const char *p = fmt;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    while (*p && pos < 510) {
        if (*p == '%') {
            p++;
            if (*p == 's') {
                const char *s = __builtin_va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && pos < 510) buf[pos++] = *s++;
            } else if (*p == 'd') {
                int v = __builtin_va_arg(ap, int);
                char tmp[12]; int ti = 0;
                if (v < 0) { buf[pos++] = '-'; v = -v; }
                if (v == 0) tmp[ti++] = '0';
                while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
                while (ti > 0 && pos < 510) buf[pos++] = tmp[--ti];
            } else if (*p == 'c') {
                int v = __builtin_va_arg(ap, int);
                buf[pos++] = (char)v;
            } else if (*p == 'x') {
                unsigned int v = __builtin_va_arg(ap, unsigned int);
                char tmp[9]; int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                while (v > 0) { tmp[ti++] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
                while (ti > 0 && pos < 510) buf[pos++] = tmp[--ti];
            } else if (*p == '%') {
                buf[pos++] = '%';
            }
            p++;
        } else {
            buf[pos++] = *p++;
        }
    }
    __builtin_va_end(ap);
    buf[pos] = '\0';
    return (int)fwrite(buf, 1, (size_t)pos, fp);
}

static inline int printf(const char *fmt, ...) {
    /* Route through fprintf to stdout */
    char buf[512];
    int pos = 0;
    const char *p = fmt;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    while (*p && pos < 510) {
        if (*p == '%') {
            p++;
            if (*p == 's') {
                const char *s = __builtin_va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && pos < 510) buf[pos++] = *s++;
            } else if (*p == 'd') {
                int v = __builtin_va_arg(ap, int);
                char tmp[12]; int ti = 0;
                if (v < 0) { buf[pos++] = '-'; v = -v; }
                if (v == 0) tmp[ti++] = '0';
                while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
                while (ti > 0 && pos < 510) buf[pos++] = tmp[--ti];
            } else if (*p == 'c') {
                int v = __builtin_va_arg(ap, int);
                buf[pos++] = (char)v;
            } else if (*p == 'x') {
                unsigned int v = __builtin_va_arg(ap, unsigned int);
                char tmp[9]; int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                while (v > 0) { tmp[ti++] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
                while (ti > 0 && pos < 510) buf[pos++] = tmp[--ti];
            } else if (*p == '%') {
                buf[pos++] = '%';
            }
            p++;
        } else {
            buf[pos++] = *p++;
        }
    }
    __builtin_va_end(ap);
    buf[pos] = '\0';
    write(STDOUT_FILENO, buf, (size_t)pos);
    return pos;
}

/* ── String utilities ────────────────────────────────── */
static inline char *strdup(const char *s) {
    if (!s) return (char *)0;
    int len = 0;
    while (s[len]) len++;
    void *(*mfn)(unsigned long) = sys->malloc;
    char *d = (char *)mfn((unsigned long)(len + 1));
    if (d) { for (int i = 0; i <= len; i++) d[i] = s[i]; }
    return d;
}

static inline char *strerror(int errnum) {
    switch (errnum) {
    case 0:  return "Success";
    case 1:  return "Operation not permitted";
    case 2:  return "No such file or directory";
    case 5:  return "I/O error";
    case 9:  return "Bad file descriptor";
    case 12: return "Out of memory";
    case 13: return "Permission denied";
    case 17: return "File exists";
    case 20: return "Not a directory";
    case 21: return "Is a directory";
    case 22: return "Invalid argument";
    case 28: return "No space left on device";
    case 38: return "Function not implemented";
    case 39: return "Directory not empty";
    default: return "Unknown error";
    }
}


/* ── POSIX wrappers (avoiding aios.h macro collision) ── */
static inline unsigned int posix_sleep(unsigned int seconds) {
    int (*fn)(unsigned int) = sys->sleep;
    return (unsigned int)fn(seconds);
}

static inline int posix_dup(int oldfd) {
    int (*fn)(int) = sys->dup;
    return fn(oldfd);
}

static inline int posix_dup2(int oldfd, int newfd) {
    int (*fn)(int, int) = sys->dup2;
    return fn(oldfd, newfd);
}

static inline int posix_pipe(int pipefd[2]) {
    int (*fn)(int *) = sys->pipe;
    return fn(pipefd);
}

static inline int posix_access(const char *path, int amode) {
    int (*fn)(const char *, int) = sys->access;
    return fn(path, amode);
}

static inline mode_t posix_umask(mode_t mask) {
    int (*fn)(int) = sys->umask;
    return (mode_t)fn((int)mask);
}

static inline uid_t posix_getuid(void) {
    int (*fn)(void) = sys->getuid;
    return (uid_t)fn();
}

static inline gid_t posix_getgid(void) {
    int (*fn)(void) = sys->getgid;
    return (gid_t)fn();
}

static inline uid_t posix_geteuid(void) {
    int (*fn)(void) = sys->geteuid;
    return (uid_t)fn();
}

static inline gid_t posix_getegid(void) {
    int (*fn)(void) = sys->getegid;
    return (gid_t)fn();
}

static inline pid_t posix_getppid(void) {
    int (*fn)(void) = sys->getppid;
    return (pid_t)fn();
}

static inline pid_t posix_spawn(const char *path, const char *args) {
    int (*fn)(const char *, const char *) = sys->spawn;
    return (pid_t)fn(path, args);
}

static inline pid_t posix_waitpid(pid_t pid, int *status) {
    int (*fn)(int, int *) = sys->waitpid;
    return (pid_t)fn((int)pid, status);
}

static inline long posix_time(void) {
    long (*fn)(void) = sys->time;
    return fn();
}

/* ── Signal handling (stub) ──────────────────────────── */
typedef void (*sighandler_t)(int);
static inline sighandler_t signal(int signum, sighandler_t handler) {
    (void)signum; (void)handler;
    return SIG_DFL; /* stub — signals delivered via Ctrl-C only */
}


/* ── Environment variables ───────────────────────────── */
#define _ENV_MAX 32
#define _ENV_BUF_SIZE 2048
static char _env_buf[_ENV_BUF_SIZE];
static int  _env_buf_pos = 0;
static char *_posix_environ[_ENV_MAX + 1];
static int  _env_count = 0;
#define environ _posix_environ

static inline void _env_init(void) {
    /* Set default environment variables */
    static int inited = 0;
    if (inited) return;
    inited = 1;
    _env_count = 0;
    /* Defaults */
    char *defaults[] = {
        "HOME=/",
        "PATH=/",
        "SHELL=/shell.bin",
        "USER=root",
        "TERM=vt100",
        (char *)0
    };
    for (int i = 0; defaults[i]; i++) {
        int len = 0;
        while (defaults[i][len]) len++;
        if (_env_buf_pos + len + 1 < _ENV_BUF_SIZE && _env_count < _ENV_MAX) {
            char *dst = _env_buf + _env_buf_pos;
            for (int j = 0; j <= len; j++) dst[j] = defaults[i][j];
            _posix_environ[_env_count++] = dst;
            _env_buf_pos += len + 1;
        }
    }
    _posix_environ[_env_count] = (char *)0;
}

static inline char *getenv(const char *name) {
    _env_init();
    int nlen = 0;
    while (name[nlen]) nlen++;
    for (int i = 0; i < _env_count; i++) {
        char *e = _posix_environ[i];
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            if (e[j] != name[j]) { match = 0; break; }
        }
        if (match && e[nlen] == '=') return e + nlen + 1;
    }
    return (char *)0;
}

static inline int setenv(const char *name, const char *value, int overwrite) {
    _env_init();
    int nlen = 0; while (name[nlen]) nlen++;
    int vlen = 0; while (value[vlen]) vlen++;

    /* Check if already exists */
    for (int i = 0; i < _env_count; i++) {
        char *e = _posix_environ[i];
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            if (e[j] != name[j]) { match = 0; break; }
        }
        if (match && e[nlen] == '=') {
            if (!overwrite) return 0;
            /* Replace in-place if it fits, else append */
            int total = nlen + 1 + vlen + 1;
            if (_env_buf_pos + total < _ENV_BUF_SIZE) {
                char *dst = _env_buf + _env_buf_pos;
                for (int j = 0; j < nlen; j++) dst[j] = name[j];
                dst[nlen] = '=';
                for (int j = 0; j <= vlen; j++) dst[nlen + 1 + j] = value[j];
                _posix_environ[i] = dst;
                _env_buf_pos += total;
                return 0;
            }
            errno = ENOMEM;
            return -1;
        }
    }

    /* New entry */
    if (_env_count >= _ENV_MAX) { errno = ENOMEM; return -1; }
    int total = nlen + 1 + vlen + 1;
    if (_env_buf_pos + total >= _ENV_BUF_SIZE) { errno = ENOMEM; return -1; }
    char *dst = _env_buf + _env_buf_pos;
    for (int j = 0; j < nlen; j++) dst[j] = name[j];
    dst[nlen] = '=';
    for (int j = 0; j <= vlen; j++) dst[nlen + 1 + j] = value[j];
    _posix_environ[_env_count++] = dst;
    _posix_environ[_env_count] = (char *)0;
    _env_buf_pos += total;
    return 0;
}

static inline int unsetenv(const char *name) {
    _env_init();
    int nlen = 0; while (name[nlen]) nlen++;
    for (int i = 0; i < _env_count; i++) {
        char *e = _posix_environ[i];
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            if (e[j] != name[j]) { match = 0; break; }
        }
        if (match && e[nlen] == '=') {
            for (int j = i; j < _env_count - 1; j++)
                _posix_environ[j] = _posix_environ[j + 1];
            _env_count--;
            _posix_environ[_env_count] = (char *)0;
            return 0;
        }
    }
    return 0; /* not found is OK */
}


/* ── BSD Socket API ──────────────────────────────────── */
#define AF_INET      2
#define AF_INET6    10
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define IPPROTO_TCP  6
#define IPPROTO_UDP 17
#define INADDR_ANY  0

typedef unsigned int socklen_t;
typedef unsigned int in_addr_t;
typedef unsigned short in_port_t;
typedef unsigned short sa_family_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t   sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

static inline unsigned short htons(unsigned short x) {
    return (unsigned short)((x >> 8) | (x << 8));
}
static inline unsigned short ntohs(unsigned short x) { return htons(x); }
static inline unsigned int htonl(unsigned int x) {
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
           ((x >> 8) & 0xFF00) | ((x >> 24) & 0xFF);
}
static inline unsigned int ntohl(unsigned int x) { return htonl(x); }

static inline int socket(int domain, int type, int protocol) {
    int (*fn)(int, int, int) = sys->socket;
    return fn(domain, type, protocol);
}

static inline int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int (*fn)(int, const void *, int) = sys->connect;
    return fn(sockfd, (const void *)addr, (int)addrlen);
}

static inline int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int (*fn)(int, const void *, int) = sys->bind;
    return fn(sockfd, (const void *)addr, (int)addrlen);
}

static inline int listen(int sockfd, int backlog) {
    int (*fn)(int, int) = sys->listen;
    return fn(sockfd, backlog);
}

static inline int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int (*fn)(int, void *, int *) = sys->accept;
    return fn(sockfd, (void *)addr, (int *)addrlen);
}

static inline ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    int (*fn)(int, const void *, unsigned long, int) = sys->send;
    return (ssize_t)fn(sockfd, buf, (unsigned long)len, flags);
}

static inline ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    int (*fn)(int, void *, unsigned long, int) = sys->recv;
    return (ssize_t)fn(sockfd, buf, (unsigned long)len, flags);
}


/* ═══════════════════════════════════════════════════════════
 *  Tier 1 POSIX extensions — stubs and wrappers
 * ═══════════════════════════════════════════════════════════ */

/* ── uname ────────────────────────────────────────────── */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

static inline int uname(struct utsname *buf) {
    if (!buf) { errno = EFAULT; return -1; }
    /* sysname */
    const char *s = "AIOS";
    int i = 0; while (s[i] && i < 64) { buf->sysname[i] = s[i]; i++; } buf->sysname[i] = 0;
    /* nodename */
    s = "aios"; i = 0; while (s[i] && i < 64) { buf->nodename[i] = s[i]; i++; } buf->nodename[i] = 0;
    /* release */
    s = "0.2.23"; i = 0; while (s[i] && i < 64) { buf->release[i] = s[i]; i++; } buf->release[i] = 0;
    /* version */
    s = "seL4 14.0.0 Microkit 2.1.0"; i = 0; while (s[i] && i < 64) { buf->version[i] = s[i]; i++; } buf->version[i] = 0;
    /* machine */
    s = "aarch64"; i = 0; while (s[i] && i < 64) { buf->machine[i] = s[i]; i++; } buf->machine[i] = 0;
    return 0;
}

static inline int gethostname(char *name, size_t len) {
    const char *h = "aios";
    size_t i = 0;
    while (h[i] && i < len - 1) { name[i] = h[i]; i++; }
    name[i] = 0;
    return 0;
}

static inline int sethostname(const char *name, size_t len) {
    (void)name; (void)len;
    errno = EPERM;
    return -1;
}

/* ── Time extensions ──────────────────────────────────── */
struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timespec {
    long tv_sec;
    long tv_nsec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#endif

static inline int gettimeofday(struct timeval *tv, struct timezone *tz) {
    if (tv) {
        long (*tfn)(void) = sys->time;
        long t = tfn();
        tv->tv_sec = t;
        tv->tv_usec = 0;
    }
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}

static inline int clock_gettime(int clk_id, struct timespec *tp) {
    (void)clk_id;
    if (!tp) { errno = EFAULT; return -1; }
    long (*tfn)(void) = sys->time;
    long t = tfn();
    tp->tv_sec = t;
    tp->tv_nsec = 0;
    return 0;
}

static inline int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req) { errno = EFAULT; return -1; }
    unsigned int secs = (unsigned int)req->tv_sec;
    if (req->tv_nsec > 0 && secs == 0) secs = 1; /* at least 1s for sub-second */
    if (secs > 0) {
        int (*sfn)(unsigned int) = sys->sleep;
        sfn(secs);
    }
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

/* ── Password/group database ─────────────────────────── */
struct passwd {
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};

struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

static struct passwd _pw_entry;
static char _pw_name[32], _pw_dir[64], _pw_shell[64];

static inline struct passwd *getpwuid(uid_t uid) {
    _pw_entry.pw_uid = uid;
    if (uid == 0) {
        const char *s;
        s = "root"; int i = 0; while (s[i]) { _pw_name[i] = s[i]; i++; } _pw_name[i] = 0;
        s = "/"; i = 0; while (s[i]) { _pw_dir[i] = s[i]; i++; } _pw_dir[i] = 0;
        s = "/bin/shell.bin"; i = 0; while (s[i]) { _pw_shell[i] = s[i]; i++; } _pw_shell[i] = 0;
    } else {
        const char *s;
        s = "user"; int i = 0; while (s[i]) { _pw_name[i] = s[i]; i++; } _pw_name[i] = 0;
        s = "/home"; i = 0; while (s[i]) { _pw_dir[i] = s[i]; i++; } _pw_dir[i] = 0;
        s = "/bin/shell.bin"; i = 0; while (s[i]) { _pw_shell[i] = s[i]; i++; } _pw_shell[i] = 0;
    }
    _pw_entry.pw_name = _pw_name;
    _pw_entry.pw_passwd = "x";
    _pw_entry.pw_gid = (gid_t)uid;
    _pw_entry.pw_gecos = _pw_name;
    _pw_entry.pw_dir = _pw_dir;
    _pw_entry.pw_shell = _pw_shell;
    return &_pw_entry;
}

static inline struct passwd *getpwnam(const char *name) {
    /* Simple lookup: root=0, user=1000, guest=1001 */
    if (name[0] == 'r' && name[1] == 'o' && name[2] == 'o' && name[3] == 't' && name[4] == 0)
        return getpwuid(0);
    if (name[0] == 'u' && name[1] == 's' && name[2] == 'e' && name[3] == 'r' && name[4] == 0)
        return getpwuid(1000);
    if (name[0] == 'g' && name[1] == 'u' && name[2] == 'e' && name[3] == 's' && name[4] == 't' && name[5] == 0)
        return getpwuid(1001);
    return (struct passwd *)0;
}

static struct group _gr_entry;
static char _gr_name[32];

static inline struct group *getgrgid(gid_t gid) {
    _gr_entry.gr_gid = gid;
    if (gid == 0) {
        const char *s = "root"; int i = 0; while (s[i]) { _gr_name[i] = s[i]; i++; } _gr_name[i] = 0;
    } else {
        const char *s = "users"; int i = 0; while (s[i]) { _gr_name[i] = s[i]; i++; } _gr_name[i] = 0;
    }
    _gr_entry.gr_name = _gr_name;
    _gr_entry.gr_passwd = "x";
    _gr_entry.gr_mem = (char **)0;
    return &_gr_entry;
}

static inline struct group *getgrnam(const char *name) {
    if (name[0] == 'r' && name[1] == 'o' && name[2] == 'o' && name[3] == 't' && name[4] == 0)
        return getgrgid(0);
    return getgrgid(1000);
}

/* ── Signal extensions ────────────────────────────────── */
typedef void (*sighandler_t)(int);

struct sigaction {
    sighandler_t sa_handler;
    unsigned long sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

#ifndef SA_RESTART
#define SA_RESTART  0x10000000
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO   4
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif

typedef unsigned long sigset_t;

static inline int sigemptyset(sigset_t *set) { if (set) *set = 0; return 0; }
static inline int sigfillset(sigset_t *set) { if (set) *set = ~0UL; return 0; }
static inline int sigaddset(sigset_t *set, int signo) { if (set) *set |= (1UL << signo); return 0; }
static inline int sigdelset(sigset_t *set, int signo) { if (set) *set &= ~(1UL << signo); return 0; }
static inline int sigismember(const sigset_t *set, int signo) { return set ? (*set >> signo) & 1 : 0; }

static inline int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    (void)signum; (void)oldact;
    if (act && act->sa_handler) {
        /* Register via posix_signal wrapper */
        signal(signum, act->sa_handler);
    }
    return 0;
}

static inline int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    (void)how; (void)set;
    if (oldset) *oldset = 0;
    return 0;
}

static inline int sigsuspend(const sigset_t *mask) {
    (void)mask;
    errno = EINTR;
    return -1;
}

static inline unsigned int alarm(unsigned int seconds) {
    (void)seconds;
    return 0; /* no previous alarm */
}

/* ── Socket extensions ────────────────────────────────── */
static inline int shutdown(int sockfd, int how) {
    (void)how;
    close(sockfd);
    return 0;
}

static inline int setsockopt(int sockfd, int level, int optname, const void *optval, unsigned int optlen) {
    (void)sockfd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0; /* silently succeed */
}

static inline int getsockopt(int sockfd, int level, int optname, void *optval, unsigned int *optlen) {
    (void)sockfd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

/* ── Symlink stubs ────────────────────────────────────── */
static inline ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    (void)path; (void)buf; (void)bufsiz;
    errno = ENOSYS;
    return -1;
}

static inline int symlink(const char *target, const char *linkpath) {
    (void)target; (void)linkpath;
    errno = ENOSYS;
    return -1;
}



/* ═══════════════════════════════════════════════════════════
 *  Tier 2 POSIX extensions — wrappers and stubs
 * ═══════════════════════════════════════════════════════════ */

/* ── *at variants (use path-based equivalents) ────────── */
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200
#endif

static inline int openat(int dirfd, const char *pathname, int flags, ...) {
    (void)dirfd; /* ignore dirfd, use path as-is */
    return open(pathname, flags);
}

static inline int mkdirat(int dirfd, const char *pathname, mode_t mode) {
    (void)dirfd;
    return mkdir(pathname, mode);
}

static inline int unlinkat(int dirfd, const char *pathname, int flags) {
    (void)dirfd;
    if (flags & AT_REMOVEDIR) return rmdir(pathname);
    return unlink(pathname);
}

static inline int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    (void)olddirfd; (void)newdirfd;
    return rename(oldpath, newpath);
}

static inline int fstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
    (void)dirfd; (void)flags;
    return stat(pathname, buf);
}

/* ── fd-based permission changes ──────────────────────── */
static inline int fchmod(int fd, mode_t mode) {
    (void)fd; (void)mode;
    errno = ENOSYS;
    return -1; /* would need fd-to-path mapping */
}

static inline int fchown(int fd, uid_t owner, gid_t group) {
    (void)fd; (void)owner; (void)group;
    errno = ENOSYS;
    return -1;
}

/* ── Identity switching (stubs — single-user kernel) ──── */
static inline int setuid(uid_t uid) { (void)uid; return 0; }
static inline int setgid(gid_t gid) { (void)gid; return 0; }
static inline int seteuid(uid_t uid) { (void)uid; return 0; }
static inline int setegid(gid_t gid) { (void)gid; return 0; }

/* ── Positioned I/O ───────────────────────────────────── */
static inline ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    off_t prev = lseek(fd, 0, SEEK_CUR);
    lseek(fd, offset, SEEK_SET);
    ssize_t n = read(fd, buf, count);
    lseek(fd, prev, SEEK_SET);
    return n;
}

static inline ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    off_t prev = lseek(fd, 0, SEEK_CUR);
    lseek(fd, offset, SEEK_SET);
    ssize_t n = write(fd, buf, count);
    lseek(fd, prev, SEEK_SET);
    return n;
}

/* ── Scatter/gather I/O ───────────────────────────────── */
struct iovec {
    void *iov_base;
    size_t iov_len;
};

static inline ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t n = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return n;
        total += n;
        if ((size_t)n < iov[i].iov_len) break;
    }
    return total;
}

static inline ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return n;
        total += n;
        if ((size_t)n < iov[i].iov_len) break;
    }
    return total;
}

/* ── Extended pipe/dup ────────────────────────────────── */
static inline int pipe2(int pipefd[2], int flags) {
    (void)flags; /* ignore O_CLOEXEC etc */
    return pipe(pipefd);
}

static inline int dup3(int oldfd, int newfd, int flags) {
    (void)flags;
    return dup2(oldfd, newfd);
}

/* ── Socket extensions ────────────────────────────────── */
static inline ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
                             const struct sockaddr *dest_addr, unsigned int addrlen) {
    (void)flags; (void)dest_addr; (void)addrlen;
    return send(sockfd, buf, len, 0);
}

static inline ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                               struct sockaddr *src_addr, unsigned int *addrlen) {
    (void)flags; (void)src_addr; (void)addrlen;
    return recv(sockfd, buf, len, 0);
}

/* ── Timer stubs ──────────────────────────────────────── */
struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};

#ifndef ITIMER_REAL
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2
#endif

static inline int setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value) {
    (void)which; (void)new_value;
    if (old_value) {
        old_value->it_interval.tv_sec = 0; old_value->it_interval.tv_usec = 0;
        old_value->it_value.tv_sec = 0; old_value->it_value.tv_usec = 0;
    }
    return 0;
}

static inline int getitimer(int which, struct itimerval *curr_value) {
    (void)which;
    if (curr_value) {
        curr_value->it_interval.tv_sec = 0; curr_value->it_interval.tv_usec = 0;
        curr_value->it_value.tv_sec = 0; curr_value->it_value.tv_usec = 0;
    }
    return 0;
}

/* ── I/O multiplexing stubs ───────────────────────────── */
typedef struct { unsigned long fds_bits[16]; } fd_set;

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#define FD_ZERO(s)   do { for (int _i=0;_i<16;_i++) (s)->fds_bits[_i]=0; } while(0)
#define FD_SET(f,s)  ((s)->fds_bits[(f)/64] |= (1UL << ((f)%64)))
#define FD_CLR(f,s)  ((s)->fds_bits[(f)/64] &= ~(1UL << ((f)%64)))
#define FD_ISSET(f,s) (((s)->fds_bits[(f)/64] >> ((f)%64)) & 1)
#endif

static inline int select(int nfds, fd_set *readfds, fd_set *writefds,
                         fd_set *exceptfds, struct timeval *timeout) {
    (void)exceptfds;
    /* Simple stub: if timeout, sleep; mark all writable fds as ready */
    if (timeout && timeout->tv_sec > 0) {
        int (*sfn)(unsigned int) = sys->sleep;
        sfn((unsigned int)timeout->tv_sec);
    }
    int ready = 0;
    if (writefds) {
        for (int i = 0; i < nfds; i++) {
            if (FD_ISSET(i, writefds)) ready++;
        }
    }
    if (readfds) {
        /* Console (fd 0) is always readable */
        if (nfds > 0 && FD_ISSET(0, readfds)) ready++;
    }
    return ready > 0 ? ready : 0;
}

/* ── Memory stubs ─────────────────────────────────────── */
static inline void *sbrk(long increment) {
    (void)increment;
    errno = ENOMEM;
    return (void *)-1;
}

/* ── Process stubs (spawn model, no fork) ─────────────── */
static inline pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}

static inline int execve(const char *pathname, char *const argv[], char *const envp[]) {
    (void)argv; (void)envp;
    return aios_exec(pathname, "");
}

static inline int execvp(const char *file, char *const argv[]) {
    (void)argv;
    return aios_exec(file, "");
}



/* ── Memory mapping stubs (no MMU access from userland) ── */
#ifndef MAP_FAILED
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON    MAP_ANONYMOUS
#define MAP_FAILED  ((void *)-1)
#endif

static inline void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
    /* For MAP_ANONYMOUS, use malloc */
    if (flags & MAP_ANONYMOUS) {
        void *(*mfn)(unsigned long) = sys->malloc;
        return mfn((unsigned long)length);
    }
    errno = ENOSYS;
    return MAP_FAILED;
}

static inline int munmap(void *addr, size_t length) {
    (void)length;
    void (*ffn)(void *) = sys->free;
    ffn(addr);
    return 0;
}

static inline int mprotect(void *addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    return 0; /* silently succeed */
}

/* ── poll stub ────────────────────────────────────────── */
struct pollfd {
    int fd;
    short events;
    short revents;
};

#ifndef POLLIN
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
#endif

static inline int poll(struct pollfd *fds, unsigned long nfds, int timeout) {
    if (timeout > 0) {
        int (*sfn)(unsigned int) = sys->sleep;
        sfn((unsigned int)((timeout + 999) / 1000));
    }
    int ready = 0;
    for (unsigned long i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].events & POLLOUT) { fds[i].revents |= POLLOUT; ready++; }
        if (fds[i].events & POLLIN && fds[i].fd == 0) { fds[i].revents |= POLLIN; ready++; }
    }
    return ready;
}


#endif /* AIOS_POSIX_H */
