/*
 * AIOS POSIX Shim — routes libc I/O through seL4 IPC
 *
 * Overrides open/read/close/write syscalls to work with AIOS services.
 * After AIOS_INIT(), standard POSIX file I/O works:
 *   fd = open("/etc/hostname", O_RDONLY);
 *   n = read(fd, buf, sizeof(buf));
 *   write(1, buf, n);
 *   close(fd);
 */
#include "aios_posix.h"
#include <arch_stdio.h>
#include <sel4/sel4.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <bits/syscall.h>
#include <muslcsys/vsyscall.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <time.h>

static seL4_CPtr ser_ep = 0;
static seL4_CPtr fs_ep_cap = 0;

seL4_CPtr aios_get_serial_ep(void) { return ser_ep; }
seL4_CPtr aios_get_fs_ep(void) { return fs_ep_cap; }

/* ── Simple fd table for ext2 files ── */
#define AIOS_MAX_FDS 32
#define AIOS_FD_BASE 10  /* start above stdin/stdout/stderr and CPIO range */

typedef struct {
    int active;
    char path[128];
    char data[4096];
    int size;
    int pos;
} aios_fd_t;

static aios_fd_t aios_fds[AIOS_MAX_FDS];

static int aios_fd_alloc(void) {
    for (int i = 0; i < AIOS_MAX_FDS; i++) {
        if (!aios_fds[i].active) return i;
    }
    return -1;
}

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void str_copy(char *d, const char *s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

/* ── Fetch file content from fs_thread via IPC ── */
static int fetch_file(const char *path, char *buf, int bufsz) {
    if (!fs_ep_cap) return -1;
    int pl = str_len(path);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap,
        seL4_MessageInfo_new(AIOS_FS_CAT, 0, 0, mr));
    seL4_Word total = seL4_GetMR(0);
    if (total == 0) return -1;
    int rmrs = (int)seL4_MessageInfo_get_length(reply) - 1;
    int got = 0;
    for (int i = 0; i < rmrs && got < bufsz - 1; i++) {
        seL4_Word rw = seL4_GetMR(i + 1);
        for (int j = 0; j < 8 && got < (int)total && got < bufsz - 1; j++)
            buf[got++] = (char)((rw >> (j * 8)) & 0xFF);
    }
    buf[got] = '\0';
    return got;
}

/* ── stdio write via IPC ── */
static size_t aios_stdio_write(void *data, size_t count) {
    if (!ser_ep) return count;
    char *buf = (char *)data;
    for (size_t i = 0; i < count; i++) {
        seL4_SetMR(0, (seL4_Word)(uint8_t)buf[i]);
        seL4_Call(ser_ep, seL4_MessageInfo_new(AIOS_SER_PUTC, 0, 0, 1));
    }
    return count;
}

int aios_getchar(void) {
    if (!ser_ep) return -1;
    seL4_MessageInfo_t reply = seL4_Call(ser_ep,
        seL4_MessageInfo_new(AIOS_SER_GETC, 0, 0, 0));
    return (int)(long)seL4_GetMR(0);
}

/* ── Syscall overrides ── */

static long aios_sys_open(va_list ap) {
    const char *pathname = va_arg(ap, const char *);
    int flags = va_arg(ap, int);
    va_arg(ap, int); /* mode — unused */

    if (!fs_ep_cap) return -ENOENT;
    if (flags != O_RDONLY && flags != (O_RDONLY | O_LARGEFILE))
        return -EINVAL;

    int idx = aios_fd_alloc();
    if (idx < 0) return -EMFILE;

    aios_fd_t *f = &aios_fds[idx];
    int n = fetch_file(pathname, f->data, sizeof(f->data));
    if (n < 0) return -ENOENT;

    f->active = 1;
    f->size = n;
    f->pos = 0;
    str_copy(f->path, pathname, sizeof(f->path));
    return AIOS_FD_BASE + idx;
}

static long aios_sys_openat(va_list ap) {
    int dirfd = va_arg(ap, int);
    const char *pathname = va_arg(ap, const char *);
    int flags = va_arg(ap, int);
    int mode = va_arg(ap, int);
    (void)dirfd; (void)mode;

    if (!fs_ep_cap) return -ENOENT;
    if (flags != O_RDONLY && flags != (O_RDONLY | O_LARGEFILE))
        return -EINVAL;

    int idx = aios_fd_alloc();
    if (idx < 0) return -EMFILE;

    aios_fd_t *f = &aios_fds[idx];
    int n = fetch_file(pathname, f->data, sizeof(f->data));
    if (n < 0) return -ENOENT;

    f->active = 1;
    f->size = n;
    f->pos = 0;
    str_copy(f->path, pathname, sizeof(f->path));
    return AIOS_FD_BASE + idx;
}

static long aios_sys_read(va_list ap) {
    int fd = va_arg(ap, int);
    void *buf = va_arg(ap, void *);
    size_t count = va_arg(ap, size_t);

    /* stdin */
    if (fd == 0) {
        char *cbuf = (char *)buf;
        for (size_t i = 0; i < count; i++) {
            int c = aios_getchar();
            if (c < 0) return (long)i;
            cbuf[i] = (char)c;
            if (c == '\n') return (long)(i + 1);
        }
        return (long)count;
    }

    /* aios fd */
    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        if (!f->active) return -EBADF;
        int avail = f->size - f->pos;
        if (avail <= 0) return 0;
        int n = (int)count < avail ? (int)count : avail;
        char *dst = (char *)buf;
        for (int i = 0; i < n; i++) dst[i] = f->data[f->pos + i];
        f->pos += n;
        return n;
    }

    /* fallback — return error for unknown fds */
    return -EBADF;
}

static long aios_sys_write(va_list ap) {
    int fd = va_arg(ap, int);
    const void *buf = va_arg(ap, const void *);
    size_t count = va_arg(ap, size_t);

    if (fd == 1 || fd == 2) {
        return (long)aios_stdio_write((void *)buf, count);
    }
    return -EBADF;
}

static long aios_sys_close(va_list ap) {
    int fd = va_arg(ap, int);
    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        f->active = 0;
        return 0;
    }
    /* let stdin/stdout/stderr close silently */
    if (fd < 3) return 0;
    return -EBADF;
}

static long aios_sys_lseek(va_list ap) {
    int fd = va_arg(ap, int);
    long offset = va_arg(ap, long);
    int whence = va_arg(ap, int);

    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        if (!f->active) return -EBADF;
        int newpos;
        if (whence == 0) newpos = (int)offset;        /* SEEK_SET */
        else if (whence == 1) newpos = f->pos + (int)offset; /* SEEK_CUR */
        else if (whence == 2) newpos = f->size + (int)offset; /* SEEK_END */
        else return -EINVAL;
        if (newpos < 0 || newpos > f->size) return -EINVAL;
        f->pos = newpos;
        return newpos;
    }
    return -EBADF;
}

static long aios_sys_writev(va_list ap) {
    int fd = va_arg(ap, int);
    struct iovec { void *iov_base; size_t iov_len; };
    struct iovec *iov = va_arg(ap, struct iovec *);
    int iovcnt = va_arg(ap, int);

    if (fd == 1 || fd == 2) {
        long total = 0;
        for (int i = 0; i < iovcnt; i++) {
            total += (long)aios_stdio_write(iov[i].iov_base, iov[i].iov_len);
        }
        return total;
    }
    return -EBADF;
}

static long aios_sys_readv(va_list ap) {
    int fd = va_arg(ap, int);
    struct iovec { void *iov_base; size_t iov_len; };
    struct iovec *iov = va_arg(ap, struct iovec *);
    int iovcnt = va_arg(ap, int);

    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        /* Rebuild va_list for aios_sys_read — just call directly */
        if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
            aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
            if (!f->active) return -EBADF;
            int avail = f->size - f->pos;
            int n = (int)iov[i].iov_len < avail ? (int)iov[i].iov_len : avail;
            char *dst = (char *)iov[i].iov_base;
            for (int j = 0; j < n; j++) dst[j] = f->data[f->pos + j];
            f->pos += n;
            total += n;
            if (n < (int)iov[i].iov_len) break;
        } else if (fd == 0) {
            /* stdin */
            char *dst = (char *)iov[i].iov_base;
            for (size_t j = 0; j < iov[i].iov_len; j++) {
                int c = aios_getchar();
                if (c < 0) return total + (long)j;
                dst[j] = (char)c;
                total++;
                if (c == '\n') return total;
            }
        } else {
            return -EBADF;
        }
    }
    return total;
}

/* ── stat/fstat via IPC ── */
static long aios_sys_fstat(va_list ap) {
    int fd = va_arg(ap, int);
    struct stat *st = va_arg(ap, struct stat *);

    /* Zero the struct */
    char *p = (char *)st;
    for (int i = 0; i < (int)sizeof(struct stat); i++) p[i] = 0;

    /* stdout/stderr/stdin */
    if (fd < 3) {
        st->st_mode = 0020666; /* char device */
        st->st_blksize = 4096;
        return 0;
    }

    /* aios fds */
    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        if (!f->active) return -EBADF;
        st->st_mode = 0100644; /* regular file */
        st->st_size = f->size;
        st->st_blksize = 4096;
        return 0;
    }
    return -EBADF;
}

static int fetch_stat(const char *path, uint32_t *mode, uint32_t *size) {
    if (!fs_ep_cap) return -1;
    int pl = str_len(path);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap,
        seL4_MessageInfo_new(12 /* FS_STAT */, 0, 0, mr));
    if (seL4_GetMR(0) == 0) return -1;
    *mode = (uint32_t)seL4_GetMR(1);
    *size = (uint32_t)seL4_GetMR(2);
    return 0;
}

static long aios_sys_fstatat(va_list ap) {
    int dirfd = va_arg(ap, int);
    const char *pathname = va_arg(ap, const char *);
    struct stat *st = va_arg(ap, struct stat *);
    int flags = va_arg(ap, int);
    (void)dirfd; (void)flags;

    char *p = (char *)st;
    for (int i = 0; i < (int)sizeof(struct stat); i++) p[i] = 0;

    uint32_t mode, size;
    if (fetch_stat(pathname, &mode, &size) != 0) return -ENOENT;

    st->st_mode = mode;
    st->st_size = size;
    st->st_blksize = 4096;
    st->st_nlink = 1;
    return 0;
}

/* ── Easy POSIX stubs ── */

static char aios_cwd[256] = "/";
static char *aios_env[] = {
    "HOME=/",
    "PATH=/bin",
    "USER=root",
    "SHELL=/bin/sh",
    "TERM=vt100",
    NULL
};

/* Set CWD (called by shell before exec if needed) */
void aios_set_cwd(const char *path) {
    int i = 0;
    while (path[i] && i < 255) { aios_cwd[i] = path[i]; i++; }
    aios_cwd[i] = '\0';
}

static long aios_sys_getcwd(va_list ap) {
    char *buf = va_arg(ap, char *);
    size_t size = va_arg(ap, size_t);
    int len = str_len(aios_cwd);
    if ((int)size <= len) return -ERANGE;
    for (int i = 0; i <= len; i++) buf[i] = aios_cwd[i];
    return (long)buf;
}

static long aios_sys_getpid(va_list ap) {
    (void)ap;
    return 1; /* TODO: get real PID from proc_table */
}

static long aios_sys_getppid(va_list ap) {
    (void)ap;
    return 0;
}

static long aios_sys_getuid(va_list ap) { (void)ap; return 0; }
static long aios_sys_geteuid(va_list ap) { (void)ap; return 0; }
static long aios_sys_getgid(va_list ap) { (void)ap; return 0; }
static long aios_sys_getegid(va_list ap) { (void)ap; return 0; }

static long aios_sys_uname(va_list ap) {
    struct utsname *buf = va_arg(ap, struct utsname *);
    /* Zero then fill */
    char *p = (char *)buf;
    for (int i = 0; i < (int)sizeof(struct utsname); i++) p[i] = 0;

    const char *s = "AIOS";
    for (int i = 0; s[i]; i++) buf->sysname[i] = s[i];
    s = "aios";
    for (int i = 0; s[i]; i++) buf->nodename[i] = s[i];
    s = "0.4.17";
    for (int i = 0; s[i]; i++) buf->release[i] = s[i];
    s = "seL4 15.0.0 SMP";
    for (int i = 0; s[i]; i++) buf->version[i] = s[i];
    s = "aarch64";
    for (int i = 0; s[i]; i++) buf->machine[i] = s[i];
    return 0;
}

static long aios_sys_ioctl(va_list ap) {
    int fd = va_arg(ap, int);
    int req = va_arg(ap, int);
    (void)fd; (void)req;
    /* Stub: return 0 for stdout ioctls (TIOCGWINSZ etc.) */
    if (fd <= 2) return 0;
    return -ENOTTY;
}

static long aios_sys_fcntl(va_list ap) {
    int fd = va_arg(ap, int);
    int cmd = va_arg(ap, int);
    (void)fd;
    if (cmd == 1) return 0;  /* F_GETFD */
    if (cmd == 2) return 0;  /* F_SETFD */
    if (cmd == 3) return 2;  /* F_GETFL: O_RDWR */
    if (cmd == 4) return 0;  /* F_SETFL */
    return -EINVAL;
}

static long aios_sys_dup(va_list ap) {
    int oldfd = va_arg(ap, int);
    if (oldfd < 3) {
        /* dup stdin/stdout/stderr — allocate new aios fd pointing to same */
        int idx = aios_fd_alloc();
        if (idx < 0) return -EMFILE;
        aios_fds[idx].active = 1;
        aios_fds[idx].size = 0;
        aios_fds[idx].pos = 0;
        return AIOS_FD_BASE + idx;
    }
    if (oldfd >= AIOS_FD_BASE && oldfd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *src = &aios_fds[oldfd - AIOS_FD_BASE];
        if (!src->active) return -EBADF;
        int idx = aios_fd_alloc();
        if (idx < 0) return -EMFILE;
        aios_fd_t *dst = &aios_fds[idx];
        *dst = *src;
        return AIOS_FD_BASE + idx;
    }
    return -EBADF;
}

static long aios_sys_dup3(va_list ap) {
    int oldfd = va_arg(ap, int);
    int newfd = va_arg(ap, int);
    int flags = va_arg(ap, int);
    (void)oldfd; (void)newfd; (void)flags;
    /* Minimal stub */
    return -EINVAL;
}

static long aios_sys_access(va_list ap) {
    const char *pathname = va_arg(ap, const char *);
    int mode = va_arg(ap, int);
    (void)mode;
    uint32_t m, s;
    if (fetch_stat(pathname, &m, &s) == 0) return 0;
    return -ENOENT;
}

static long aios_sys_faccessat(va_list ap) {
    int dirfd = va_arg(ap, int);
    const char *pathname = va_arg(ap, const char *);
    int mode = va_arg(ap, int);
    (void)dirfd; (void)mode;
    uint32_t m, s;
    if (fetch_stat(pathname, &m, &s) == 0) return 0;
    return -ENOENT;
}

static long aios_sys_clock_gettime(va_list ap) {
    int clk_id = va_arg(ap, int);
    struct timespec *tp = va_arg(ap, struct timespec *);
    (void)clk_id;
    /* Read ARM counter */
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    tp->tv_sec = (long)(cnt / freq);
    tp->tv_nsec = (long)((cnt % freq) * 1000000000ULL / freq);
    return 0;
}

static long aios_sys_gettimeofday(va_list ap) {
    struct timeval *tv = va_arg(ap, struct timeval *);
    void *tz = va_arg(ap, void *);
    (void)tz;
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    tv->tv_sec = (long)(cnt / freq);
    tv->tv_usec = (long)((cnt % freq) * 1000000ULL / freq);
    return 0;
}

static long aios_sys_nanosleep(va_list ap) {
    const struct timespec *req = va_arg(ap, const struct timespec *);
    struct timespec *rem = va_arg(ap, struct timespec *);
    /* Busy-wait using ARM counter */
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    uint64_t target;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(target));
    target += (uint64_t)req->tv_sec * freq + (uint64_t)req->tv_nsec * freq / 1000000000ULL;
    uint64_t now;
    do {
        seL4_Yield();
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    } while (now < target);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

/* ── Init ── */
void aios_init(seL4_CPtr serial_ep, seL4_CPtr fs_endpoint) {
    ser_ep = serial_ep;
    fs_ep_cap = fs_endpoint;

    /* Clear fd table */
    for (int i = 0; i < AIOS_MAX_FDS; i++) aios_fds[i].active = 0;

    /* Register stdio write hook */
    sel4muslcsys_register_stdio_write_fn(aios_stdio_write);

    /* Override syscalls */
    muslcsys_install_syscall(__NR_write, aios_sys_write);
    muslcsys_install_syscall(__NR_read, aios_sys_read);
    muslcsys_install_syscall(__NR_close, aios_sys_close);
    muslcsys_install_syscall(__NR_writev, aios_sys_writev);
    muslcsys_install_syscall(__NR_readv, aios_sys_readv);
#ifdef __NR_open
    muslcsys_install_syscall(__NR_open, aios_sys_open);
#endif
#ifdef __NR_openat
    muslcsys_install_syscall(__NR_openat, aios_sys_openat);
#endif
#ifdef __NR_lseek
    muslcsys_install_syscall(__NR_lseek, aios_sys_lseek);
#endif
#ifdef __NR_fstat
    muslcsys_install_syscall(__NR_fstat, aios_sys_fstat);
#endif
#ifdef __NR_newfstatat
    muslcsys_install_syscall(__NR_newfstatat, aios_sys_fstatat);
#endif

    /* Easy POSIX stubs */
    muslcsys_install_syscall(__NR_getcwd, aios_sys_getcwd);
    muslcsys_install_syscall(__NR_getpid, aios_sys_getpid);
    muslcsys_install_syscall(__NR_getppid, aios_sys_getppid);
    muslcsys_install_syscall(__NR_getuid, aios_sys_getuid);
    muslcsys_install_syscall(__NR_geteuid, aios_sys_geteuid);
    muslcsys_install_syscall(__NR_getgid, aios_sys_getgid);
    muslcsys_install_syscall(__NR_getegid, aios_sys_getegid);
    muslcsys_install_syscall(__NR_uname, aios_sys_uname);
    muslcsys_install_syscall(__NR_ioctl, aios_sys_ioctl);
    muslcsys_install_syscall(__NR_fcntl, aios_sys_fcntl);
#ifdef __NR_dup
    muslcsys_install_syscall(__NR_dup, aios_sys_dup);
#endif
    muslcsys_install_syscall(__NR_dup3, aios_sys_dup3);
#ifdef __NR_access
    muslcsys_install_syscall(__NR_access, aios_sys_access);
#endif
    muslcsys_install_syscall(__NR_faccessat, aios_sys_faccessat);
    muslcsys_install_syscall(__NR_clock_gettime, aios_sys_clock_gettime);
    muslcsys_install_syscall(__NR_gettimeofday, aios_sys_gettimeofday);
    muslcsys_install_syscall(__NR_nanosleep, aios_sys_nanosleep);
}
