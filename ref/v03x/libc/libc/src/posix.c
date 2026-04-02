/*
 * AIOS POSIX function implementations
 *
 * Bridges standard POSIX signatures to the AIOS syscall transport.
 * Uses __aios_syscall (PPC to orchestrator) and shared memory for
 * passing paths and data buffers.
 */
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>

/* From syscall.c */
extern long __aios_syscall(long num, long a0, long a1, long a2);
extern uintptr_t sandbox_io;

#define SHM_PATH      0x200
#define SHM_DATA      0x400
#define SHM_DATA_MAX  3072

/* ── Helpers ─────────────────────────────────────────── */

static int shm_copy_path(const char *path) {
    volatile char *dst = (volatile char *)(sandbox_io + SHM_PATH);
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    return i;
}

static void shm_write(const void *src, int len) {
    const uint8_t *s = (const uint8_t *)src;
    volatile uint8_t *d = (volatile uint8_t *)(sandbox_io + SHM_DATA);
    for (int i = 0; i < len; i++) d[i] = s[i];
}

static void shm_read(void *dst, int len) {
    uint8_t *d = (uint8_t *)dst;
    volatile uint8_t *s = (volatile uint8_t *)(sandbox_io + SHM_DATA);
    for (int i = 0; i < len; i++) d[i] = s[i];
}

/* ── File operations ─────────────────────────────────── */

int open(const char *path, int flags, ...) {
    shm_copy_path(path);
    long r = __aios_syscall(SYS_OPEN, flags, 0, 0);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

int creat(const char *path, int mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

ssize_t read(int fd, void *buf, size_t count) {
    if (count == 0) return 0;
    if (count > SHM_DATA_MAX) count = SHM_DATA_MAX;

    /* fd 0 = stdin: use console getc */
    if (fd == STDIN_FILENO) {
        long c = __aios_syscall(SYS_GETC, 0, 0, 0);
        if (c < 0) return 0;
        ((char *)buf)[0] = (char)c;
        return 1;
    }

    long r = __aios_syscall(SYS_READ, fd, 0, (long)count);
    if (r < 0) { errno = (int)(-r); return -1; }
    if (r > 0) shm_read(buf, (int)r);
    return (ssize_t)r;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (count == 0) return 0;

    /* fd 1/2 = stdout/stderr: use console putc */
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        const char *s = (const char *)buf;
        for (size_t i = 0; i < count; i++) {
            __aios_syscall(SYS_PUTC, (long)s[i], 0, 0);
        }
        return (ssize_t)count;
    }

    if (count > SHM_DATA_MAX) count = SHM_DATA_MAX;
    shm_write(buf, (int)count);
    long r = __aios_syscall(SYS_WRITE, fd, (long)count, 0);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (ssize_t)r;
}

int close(int fd) {
    /* Don't close stdin/stdout/stderr */
    if (fd <= STDERR_FILENO) return 0;
    long r = __aios_syscall(SYS_CLOSE, fd, 0, 0);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

off_t lseek(int fd, off_t offset, int whence) {
    long r = __aios_syscall(SYS_LSEEK, fd, (long)offset, whence);
    if (r < 0) { errno = (int)(-r); return (off_t)-1; }
    return (off_t)r;
}

int unlink(const char *path) {
    shm_copy_path(path);
    long r = __aios_syscall(SYS_UNLINK, 0, 0, 0);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

/* ── stat ────────────────────────────────────────────── */

int stat(const char *path, struct stat *buf) {
    shm_copy_path(path);
    long r = __aios_syscall(SYS_STAT, 0, 0, 0);
    if (r < 0) { errno = ENOENT; return -1; }

    /* Orchestrator returns: MR0=0, MR1=size, MR2=mode */
    if (buf) {
        for (int i = 0; i < (int)sizeof(struct stat); i++)
            ((char *)buf)[i] = 0;
        buf->st_size = (off_t)__aios_syscall(SYS_STAT, 1, 0, 0);
        buf->st_mode = S_IFREG | 0644;  /* default regular file */
        buf->st_nlink = 1;
        buf->st_blksize = 512;
        buf->st_blocks = (buf->st_size + 511) / 512;
        buf->st_ino = 1;
    }
    return 0;
}

int fstat(int fd, struct stat *buf) {
    /* Minimal: just return file size from the VFS */
    long r = __aios_syscall(SYS_FSTAT, fd, 0, 0);
    if (r < 0) { errno = EBADF; return -1; }
    if (buf) {
        for (int i = 0; i < (int)sizeof(struct stat); i++)
            ((char *)buf)[i] = 0;
        buf->st_size = (off_t)r;
        buf->st_mode = (fd <= STDERR_FILENO) ? (S_IFREG | 0666) : (S_IFREG | 0644);
        buf->st_nlink = 1;
        buf->st_blksize = 512;
    }
    return 0;
}

/* ── Directory operations ────────────────────────────── */

/* Static DIR and dirent — single-directory-at-a-time */
static DIR _the_dir;
static struct dirent _the_dirent;

/* Directory entry buffer: up to 64 entries, 16 bytes each
 * (from readdir syscall: 12-byte name + 4-byte size) */
#define MAX_DIR_ENTRIES 64
static uint8_t _dir_buf[MAX_DIR_ENTRIES * 16];
static int _dir_count;

DIR *opendir(const char *name) {
    (void)name; /* Only root directory for now */
    long r = __aios_syscall(SYS_READDIR, 0, 0, 0);
    if (r < 0) { errno = ENOENT; return NULL; }
    _dir_count = (int)r;
    if (_dir_count > MAX_DIR_ENTRIES) _dir_count = MAX_DIR_ENTRIES;
    if (_dir_count > 0) shm_read(_dir_buf, _dir_count * 16);
    _the_dir._fd = 0;
    _the_dir._pos = 0;
    return &_the_dir;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp || dirp->_pos >= _dir_count) return NULL;
    uint8_t *entry = _dir_buf + dirp->_pos * 16;

    /* Entry format: 12 bytes filename (null-padded) + 4 bytes size */
    int i;
    for (i = 0; i < 12 && entry[i]; i++)
        _the_dirent.d_name[i] = (char)entry[i];
    _the_dirent.d_name[i] = '\0';
    _the_dirent.d_ino = (ino_t)(dirp->_pos + 1);

    dirp->_pos++;
    return &_the_dirent;
}

int closedir(DIR *dirp) {
    if (dirp) { dirp->_fd = -1; dirp->_pos = 0; }
    return 0;
}

/* ── mkdir / rmdir ───────────────────────────────────── */

int mkdir(const char *path, mode_t mode) {
    (void)mode;
    shm_copy_path(path);
    long r = __aios_syscall(SYS_MKDIR, 0, 0, 0);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

int rmdir(const char *path) {
    shm_copy_path(path);
    long r = __aios_syscall(SYS_RMDIR, 0, 0, 0);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

/* ── Process-like stubs ──────────────────────────────── */

char *getcwd(char *buf, size_t size) {
    /* Single root directory for now */
    if (buf && size >= 2) { buf[0] = '/'; buf[1] = '\0'; }
    return buf;
}

int chdir(const char *path) {
    (void)path;
    return 0;  /* no-op, single directory */
}

pid_t getpid(void) {
    return (pid_t)__aios_syscall(SYS_GETPID, 0, 0, 0);
}

int isatty(int fd) {
    return (fd <= STDERR_FILENO) ? 1 : 0;
}

unsigned int sleep(unsigned int seconds) {
    __aios_syscall(SYS_SLEEP, (long)seconds, 0, 0);
    return 0;
}

int usleep(unsigned long usec) {
    (void)usec;
    return 0;
}
