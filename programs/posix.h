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
    unsigned st_nlink;
};

/* ── Directory ───────────────────────────────────────── */
#define NAME_MAX 12

struct dirent {
    ino_t  d_ino;
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

static inline int remove(const char *path) {
    return unlink(path);
}

/* ── POSIX stat() ────────────────────────────────────── */
static inline int stat(const char *path, struct stat *buf) {
    unsigned long size = 0;
    int r = sys->stat_file(path, &size);
    if (r < 0) return -1;
    if (buf) {
        for (int i = 0; i < (int)sizeof(struct stat); i++)
            ((char *)buf)[i] = 0;
        buf->st_size = (off_t)size;
        buf->st_mode = S_IFREG | 0644;
        buf->st_ino = 1;
        buf->st_nlink = 1;


    }
    return 0;
}

/* ── Directory operations ────────────────────────────── */

/* Simple directory listing using readdir syscall */
typedef struct {
    struct dirent entries[64];
    int count;
    int pos;
} DIR;

static DIR _posix_dir;

static inline DIR *opendir(const char *name) {
    (void)name;
    /* Use the raw 16-byte entry format from sandbox readdir */
    unsigned char raw[64 * 16];
    int count = sys->readdir(raw, 64);
    if (count < 0) return NULL;
    _posix_dir.count = count;
    _posix_dir.pos = 0;
    for (int i = 0; i < count; i++) {
        unsigned char *e = raw + i * 16;
        int j;
        for (j = 0; j < 12 && e[j]; j++)
            _posix_dir.entries[i].d_name[j] = (char)e[j];
        _posix_dir.entries[i].d_name[j] = '\0';
        _posix_dir.entries[i].d_ino = (ino_t)(i + 1);
        /* Bytes 12-15: file size (little-endian) */
        _posix_dir.entries[i].d_size =
            (unsigned long)e[12] | ((unsigned long)e[13] << 8) |
            ((unsigned long)e[14] << 16) | ((unsigned long)e[15] << 24);
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

static inline int mkdir(const char *path, mode_t mode) {
    (void)path; (void)mode;
    return -1;  /* not yet supported */
}

static inline int rmdir(const char *path) {
    (void)path;
    return -1;  /* not yet supported */
}

static inline char *getcwd(char *buf, size_t size) {
    sys->getcwd(buf, (unsigned long)size);
    return buf;
}

static inline int isatty(int fd) {
    return _FD_IS_CONSOLE(fd) ? 1 : 0;
}

static inline pid_t getpid(void) { return (pid_t)sys->getpid(); }

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

#endif /* AIOS_POSIX_H */
