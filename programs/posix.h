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

#include "aios.h"

/* ── Types ───────────────────────────────────────────── */
typedef long          ssize_t;
typedef long          off_t;
typedef unsigned long ino_t;
typedef int           pid_t;

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
        if (c < 0) return 0;
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
    return sys->unlink(path);
}

static inline int mkdir(const char *path, mode_t mode) {
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

static inline DIR *opendir(const char *path) {
    (void)path;
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
    if (fd < 0) return (FILE *)0;
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

#endif /* AIOS_POSIX_H */
