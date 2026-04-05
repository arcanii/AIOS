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
#include <sel4runtime.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <bits/syscall.h>
#ifndef __NR_getdents64
#define __NR_getdents64 61
#endif
#define PIPE_FORK 65
#define PIPE_GETPID 66
#define PIPE_WAIT 67
#define PIPE_EXIT 68
#include <muslcsys/vsyscall.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>

static seL4_CPtr ser_ep = 0;
static seL4_CPtr fs_ep_cap = 0;
static seL4_CPtr thread_ep = 0;
static seL4_CPtr auth_ep = 0;
seL4_CPtr pipe_ep = 0;
static int fetch_stat(const char *path, uint32_t *mode, uint32_t *size);

/* Resolve relative path against CWD */

static char aios_cwd[256] = "/";
static uint32_t aios_uid = 0;
static uint32_t aios_gid = 0;
static int stdout_pipe_id = -1;   /* if >= 0, stdout goes to pipe */
static int stdin_pipe_id = -1;    /* if >= 0, stdin comes from pipe */

static void resolve_path(const char *pathname, char *out, int outsz) {
    if (pathname[0] == '/') {
        int i = 0;
        while (pathname[i] && i < outsz - 1) { out[i] = pathname[i]; i++; }
        out[i] = 0;
        return;
    }
    int ci = 0;
    while (aios_cwd[ci] && ci < outsz - 2) { out[ci] = aios_cwd[ci]; ci++; }
    if (pathname[0] == '.' && pathname[1] == 0) {
        out[ci] = 0;
        return;
    }
    if (pathname[0] == '.' && pathname[1] == '/') {
        pathname += 2;
    }
    if (ci > 1) out[ci++] = '/';
    while (*pathname && ci < outsz - 1) out[ci++] = *pathname++;
    out[ci] = 0;
}


seL4_CPtr aios_get_serial_ep(void) { return ser_ep; }
seL4_CPtr aios_get_fs_ep(void) { return fs_ep_cap; }
seL4_CPtr aios_get_auth_ep(void) { return auth_ep; }
int aios_nb_getchar(void) {
    if (!ser_ep) return -1;
    seL4_SetMR(0, 0);
    seL4_Call(ser_ep, seL4_MessageInfo_new(AIOS_SER_GETC, 0, 0, 0));
    return (int)(long)seL4_GetMR(0);
}

/* ── Simple fd table for ext2 files ── */
#define AIOS_MAX_FDS 32
#define AIOS_FD_BASE 10  /* start above stdin/stdout/stderr and CPIO range */

typedef struct {
    int active;
    int is_dir;
    int is_pipe;
    int pipe_id;      /* pipe server pipe index */
    int pipe_read;    /* 1=read end, 0=write end */
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
    /* Check for pipe redirect (stdout_pipe_id set by __wrap_main) */
    if (stdout_pipe_id >= 0 && pipe_ep) {
        const char *src = (const char *)data;
        size_t sent = 0;
        while (sent < count) {
            int chunk = (int)(count - sent);
            if (chunk > 900) chunk = 900;
            seL4_SetMR(0, (seL4_Word)stdout_pipe_id);
            seL4_SetMR(1, (seL4_Word)chunk);
            int mr = 2;
            seL4_Word w = 0;
            for (int i = 0; i < chunk; i++) {
                w |= ((seL4_Word)(uint8_t)src[sent + i]) << ((i % 8) * 8);
                if (i % 8 == 7 || i == chunk - 1) { seL4_SetMR(mr++, w); w = 0; }
            }
            seL4_Call(pipe_ep, seL4_MessageInfo_new(61, 0, 0, mr));
            sent += chunk;
        }
        return count;
    }
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

/* Fetch directory listing from fs_thread, format as getdents buffer */
static int fetch_dir_as_getdents(const char *path, char *buf, int bufsz) {
    if (!fs_ep_cap) return -1;
    /* Multi-round: fetch raw listing in chunks via offset */
    char raw[4096];
    int pl = str_len(path);
    int got = 0;
    int total = 0;
    int offset = 0;

    do {
        seL4_SetMR(0, (seL4_Word)pl);
        seL4_SetMR(1, (seL4_Word)offset);  /* chunk offset */
        int mr = 2;  /* path starts at MR2 */
        seL4_Word w = 0;
        for (int i = 0; i < pl; i++) {
            w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
            if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
        }
        seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap,
            seL4_MessageInfo_new(10 /* FS_LS */, 0, 0, mr));
        total = (int)seL4_GetMR(0);
        if (total == 0) return -1;

        int rmrs = (int)seL4_MessageInfo_get_length(reply) - 1;
        for (int i = 0; i < rmrs && got < 4095; i++) {
            seL4_Word rw = seL4_GetMR(i + 1);
            for (int j = 0; j < 8 && got < total && got < 4095; j++)
                raw[got++] = (char)((rw >> (j * 8)) & 0xFF);
        }
        offset = got;
    } while (got < total && got < 4095);

    raw[got] = '\0';

    /* Parse "d name\n" or "- name\n" lines into struct dirent format */
    /* struct dirent: d_ino(8) d_off(8) d_reclen(2) d_type(1) d_name[256] */
    int pos = 0;  /* position in raw */
    int out = 0;  /* position in buf */
    uint64_t fake_ino = 100;
    uint64_t d_off_counter = 0;

    while (pos < got && out < bufsz - 280) {
        char type_ch = raw[pos];
        if (type_ch != 'd' && type_ch != '-') break;
        pos += 2; /* skip "d " or "- " */

        /* Extract name */
        char name[256];
        int nl = 0;
        while (pos < got && raw[pos] != '\n' && nl < 255) {
            name[nl++] = raw[pos++];
        }
        name[nl] = '\0';
        if (pos < got && raw[pos] == '\n') pos++;

        /* Build dirent */
        int reclen = (8 + 8 + 2 + 1 + nl + 1 + 7) & ~7; /* align to 8 */
        if (reclen < 24) reclen = 24;
        if (out + reclen > bufsz) break;

        d_off_counter += reclen;
        uint8_t d_type = (type_ch == 'd') ? 4 : 8; /* DT_DIR=4, DT_REG=8 */

        /* d_ino (8 bytes) */
        uint64_t ino = fake_ino++;
        for (int i = 0; i < 8; i++) buf[out + i] = (char)((ino >> (i*8)) & 0xFF);
        /* d_off (8 bytes) */
        for (int i = 0; i < 8; i++) buf[out + 8 + i] = (char)((d_off_counter >> (i*8)) & 0xFF);
        /* d_reclen (2 bytes) */
        buf[out + 16] = (char)(reclen & 0xFF);
        buf[out + 17] = (char)((reclen >> 8) & 0xFF);
        /* d_type (1 byte) */
        buf[out + 18] = (char)d_type;
        /* d_name */
        for (int i = 0; i < nl; i++) buf[out + 19 + i] = name[i];
        buf[out + 19 + nl] = '\0';
        /* Zero padding */
        for (int i = 19 + nl + 1; i < reclen; i++) buf[out + i] = 0;

        out += reclen;
    }
    return out;
}

static long aios_sys_open(va_list ap) {
    const char *pathname = va_arg(ap, const char *);
    int flags = va_arg(ap, int);
    va_arg(ap, int); /* mode */

    if (!fs_ep_cap) return -ENOENT;

    int is_dir = (flags & 040000) != 0; /* O_DIRECTORY = 0200000 */
    int rdonly = ((flags & 3) == 0); /* O_RDONLY */
    flags &= ~(040000 | 02000000 | 0100000); /* strip O_DIRECTORY, O_CLOEXEC, O_LARGEFILE */

    if (!rdonly && !is_dir) return -EINVAL;

    int idx = aios_fd_alloc();
    if (idx < 0) return -EMFILE;

    aios_fd_t *f = &aios_fds[idx];
    f->is_dir = 0;

    if (is_dir) {
        char res_path[256];
        resolve_path(pathname, res_path, sizeof(res_path));
        int n = fetch_dir_as_getdents(res_path, f->data, sizeof(f->data));
        if (n < 0) return -ENOENT;
        f->active = 1;
        f->is_dir = 1;
        f->size = n;
        f->pos = 0;
        str_copy(f->path, pathname, sizeof(f->path));
    } else {
        char res_path[256];
        resolve_path(pathname, res_path, sizeof(res_path));
        int n = fetch_file(res_path, f->data, sizeof(f->data));
        if (n < 0) return -ENOENT;
        f->active = 1;
        f->size = n;
        f->pos = 0;
        str_copy(f->path, res_path, sizeof(f->path));
    }
    return AIOS_FD_BASE + idx;
}

static long aios_sys_openat(va_list ap) {
    int dirfd = va_arg(ap, int);
    const char *pathname = va_arg(ap, const char *);
    int flags = va_arg(ap, int);
    int mode = va_arg(ap, int);
    (void)dirfd; (void)mode;

    if (!fs_ep_cap) return -ENOENT;

    int is_dir = (flags & 040000) != 0;
    int is_creat = (flags & 0100) != 0;
    int is_wronly = ((flags & 3) == 1);
    int is_rdwr = ((flags & 3) == 2);
    flags &= ~(040000 | 02000000 | 0100000 | 0100 | 01000 | 02000);

    /* O_CREAT — create file via IPC if it doesn't exist */
    if (is_creat && fs_ep_cap) {
        /* Resolve path */
        char resolved[256];
        const char *p = pathname;
        if (p[0] != '/') {
            int ci = 0;
            while (aios_cwd[ci] && ci < 250) { resolved[ci] = aios_cwd[ci]; ci++; }
            if (ci > 1) resolved[ci++] = '/';
            while (*p && ci < 255) resolved[ci++] = *p++;
            resolved[ci] = 0;
            p = resolved;
        }
        /* Check if file exists */
        uint32_t m, s;
        if (fetch_stat(p, &m, &s) != 0) {
            /* Doesn't exist — create via IPC */
            int pl = str_len(p);
            seL4_SetMR(0, (seL4_Word)pl);
            int mr = 1;
            seL4_Word w = 0;
            for (int i = 0; i < pl; i++) {
                w |= ((seL4_Word)(uint8_t)p[i]) << ((i % 8) * 8);
                if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
            }
            seL4_SetMR(mr, 0); /* data_len = 0 */
            seL4_Call(fs_ep_cap,
                seL4_MessageInfo_new(15 /* FS_WRITE_FILE */, 0, 0, mr + 1));
        }
        /* If write-only, return a dummy fd */
        if (is_wronly || is_rdwr) {
            int idx = aios_fd_alloc();
            if (idx < 0) return -EMFILE;
            aios_fd_t *f = &aios_fds[idx];
            f->active = 1;
            f->is_dir = 0;
            f->size = 0;
            f->pos = 0;
            str_copy(f->path, pathname, sizeof(f->path));
            return AIOS_FD_BASE + idx;
        }
    }

    int idx = aios_fd_alloc();
    if (idx < 0) return -EMFILE;

    aios_fd_t *f = &aios_fds[idx];
    f->is_dir = 0;

    char res_path[256];
    resolve_path(pathname, res_path, sizeof(res_path));
    if (is_dir) {
        int n = fetch_dir_as_getdents(res_path, f->data, sizeof(f->data));
        if (n < 0) return -ENOENT;
        f->active = 1;
        f->is_dir = 1;
        f->size = n;
        f->pos = 0;
        str_copy(f->path, res_path, sizeof(f->path));
    } else {
        int n = fetch_file(res_path, f->data, sizeof(f->data));
        if (n < 0) return -ENOENT;
        f->active = 1;
        f->size = n;
        f->pos = 0;
        str_copy(f->path, res_path, sizeof(f->path));
    }
    return AIOS_FD_BASE + idx;
}

static long aios_sys_read(va_list ap) {
    int fd = va_arg(ap, int);
    void *buf = va_arg(ap, void *);
    size_t count = va_arg(ap, size_t);

    /* stdin — check for pipe redirect */
    if (fd == 0) {
        if (stdin_pipe_id >= 0 && pipe_ep) {
            /* Read from pipe instead of serial */
            char *cbuf = (char *)buf;
            int want = (int)count;
            if (want > 900) want = 900;
            seL4_SetMR(0, (seL4_Word)stdin_pipe_id);
            seL4_SetMR(1, (seL4_Word)want);
            seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
                seL4_MessageInfo_new(62, 0, 0, 2));
            int got = (int)(long)seL4_GetMR(0);
            int mr = 1;
            for (int i = 0; i < got; i++) {
                if (i % 8 == 0 && i > 0) mr++;
                cbuf[i] = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
            }
            return (long)got;
        }
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
        /* Pipe read */
        if (f->is_pipe && f->pipe_read && pipe_ep) {
            char *cbuf = (char *)buf;
            int want = (int)count;
            if (want > 900) want = 900;
            seL4_SetMR(0, (seL4_Word)f->pipe_id);
            seL4_SetMR(1, (seL4_Word)want);
            seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
                seL4_MessageInfo_new(62, 0, 0, 2));
            int got = (int)(long)seL4_GetMR(0);
            int mr = 1;
            for (int i = 0; i < got; i++) {
                if (i % 8 == 0 && i > 0) mr++;
                cbuf[i] = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
            }
            return (long)got;
        }
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

    /* stdout/stderr — check for pipe redirect */
    if (fd == 1 || fd == 2) {
        if (stdout_pipe_id >= 0 && pipe_ep) {
            /* Write to pipe instead of serial */
            const char *src = (const char *)buf;
            size_t sent = 0;
            while (sent < count) {
                int chunk = (int)(count - sent);
                if (chunk > 900) chunk = 900;  /* MR limit */
                seL4_SetMR(0, (seL4_Word)stdout_pipe_id);
                seL4_SetMR(1, (seL4_Word)chunk);
                int mr = 2;
                seL4_Word w = 0;
                for (int i = 0; i < chunk; i++) {
                    w |= ((seL4_Word)(uint8_t)src[sent + i]) << ((i % 8) * 8);
                    if (i % 8 == 7 || i == chunk - 1) { seL4_SetMR(mr++, w); w = 0; }
                }
                seL4_Call(pipe_ep, seL4_MessageInfo_new(61, 0, 0, mr));
                sent += chunk;
            }
            return (long)count;
        }
        return (long)aios_stdio_write((void *)buf, count);
    }

    /* File or pipe fd */
    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        if (!f->active) return -EBADF;
        /* Regular file write via FS_WRITE_FILE */
        if (!f->is_pipe && !f->is_dir && f->path[0] && fs_ep_cap) {
            const char *src = (const char *)buf;
            int plen = 0; while (f->path[plen]) plen++;
            seL4_SetMR(0, (seL4_Word)plen);
            int mr = 1;
            seL4_Word w = 0;
            for (int i = 0; i < plen; i++) {
                w |= ((seL4_Word)(uint8_t)f->path[i]) << ((i % 8) * 8);
                if (i % 8 == 7 || i == plen - 1) { seL4_SetMR(mr++, w); w = 0; }
            }
            int wlen = (int)count;
            if (wlen > 3000) wlen = 3000;  /* MR limit */
            seL4_SetMR(mr++, (seL4_Word)wlen);
            w = 0;
            for (int i = 0; i < wlen; i++) {
                w |= ((seL4_Word)(uint8_t)src[i]) << ((i % 8) * 8);
                if (i % 8 == 7 || i == wlen - 1) { seL4_SetMR(mr++, w); w = 0; }
            }
            seL4_Call(fs_ep_cap, seL4_MessageInfo_new(15 /* FS_WRITE_FILE */, 0, 0, mr));
            return (long)wlen;
        }
        if (f->is_pipe && !f->pipe_read && pipe_ep) {
            const char *src = (const char *)buf;
            size_t sent = 0;
            while (sent < count) {
                int chunk = (int)(count - sent);
                if (chunk > 900) chunk = 900;
                seL4_SetMR(0, (seL4_Word)f->pipe_id);
                seL4_SetMR(1, (seL4_Word)chunk);
                int mr = 2;
                seL4_Word w = 0;
                for (int i = 0; i < chunk; i++) {
                    w |= ((seL4_Word)(uint8_t)src[sent + i]) << ((i % 8) * 8);
                    if (i % 8 == 7 || i == chunk - 1) { seL4_SetMR(mr++, w); w = 0; }
                }
                seL4_Call(pipe_ep, seL4_MessageInfo_new(61, 0, 0, mr));
                sent += chunk;
            }
            return (long)count;
        }
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
            /* stdin — check pipe redirect */
            if (stdin_pipe_id >= 0 && pipe_ep) {
                char *dst = (char *)iov[i].iov_base;
                int want = (int)iov[i].iov_len;
                if (want > 900) want = 900;
                seL4_SetMR(0, (seL4_Word)stdin_pipe_id);
                seL4_SetMR(1, (seL4_Word)want);
                seL4_MessageInfo_t rpl = seL4_Call(pipe_ep,
                    seL4_MessageInfo_new(62, 0, 0, 2));
                int got = (int)(long)seL4_GetMR(0);
                int mr = 1;
                for (int k = 0; k < got; k++) {
                    if (k % 8 == 0 && k > 0) mr++;
                    dst[k] = (char)((seL4_GetMR(mr) >> ((k % 8) * 8)) & 0xFF);
                }
                total += got;
                if (got < (int)iov[i].iov_len) break;
            } else {
            char *dst = (char *)iov[i].iov_base;
            for (size_t j = 0; j < iov[i].iov_len; j++) {
                int c = aios_getchar();
                if (c < 0) return total + (long)j;
                dst[j] = (char)c;
                total++;
                if (c == '\n') return total;
            }
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

    /* Resolve relative paths using CWD */
    char resolved[256];
    const char *lookup = pathname;
    if (pathname[0] != '/') {
        /* Relative path — prepend CWD */
        int ci = 0;
        while (aios_cwd[ci] && ci < 250) { resolved[ci] = aios_cwd[ci]; ci++; }
        if (ci > 1) resolved[ci++] = '/';
        if (pathname[0] == '.' && pathname[1] == 0) {
            /* just "." — use CWD as-is */
            resolved[ci] = 0;
            if (ci > 1) resolved[ci-1] = 0; /* strip trailing / */
        } else if (pathname[0] == '.' && pathname[1] == '/') {
            const char *s = pathname + 2;
            while (*s && ci < 255) resolved[ci++] = *s++;
            resolved[ci] = 0;
        } else {
            const char *s = pathname;
            while (*s && ci < 255) resolved[ci++] = *s++;
            resolved[ci] = 0;
        }
        lookup = resolved;
    }

    uint32_t mode, size;
    if (fetch_stat(lookup, &mode, &size) != 0) return -ENOENT;

    st->st_mode = mode;
    st->st_size = size;
    st->st_blksize = 4096;
    st->st_nlink = 1;
    return 0;
}

/* ── Easy POSIX stubs ── */
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


/* Exit: trigger a page fault so exec_thread catches it cleanly */
static long aios_sys_exit(va_list ap) {
    int status = va_arg(ap, int);
    (void)status;
    /* Trigger VM fault — exec_thread Recv's on fault ep */
    volatile int *null = (volatile int *)0;
    *null = 0;
    __builtin_unreachable();
    return 0;
}

static long aios_sys_exit_group(va_list ap) {
    int status = va_arg(ap, int);
    /* Send exit code to pipe_server before dying */
    if (pipe_ep) {
        seL4_SetMR(0, (seL4_Word)status);
        seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_EXIT, 0, 0, 1));
    }
    /* Fault to trigger reaper (NULL deref) */
    volatile int *null_ptr = (volatile int *)0;
    *null_ptr = 0;
    __builtin_unreachable();
    return 0;
}


/* mkdir via IPC to fs_thread */
static long aios_sys_mkdirat(va_list ap) {
    int dirfd = va_arg(ap, int);
    const char *pathname = va_arg(ap, const char *);
    int mode = va_arg(ap, int);
    (void)dirfd; (void)mode;
    if (!fs_ep_cap) return -ENOSYS;

    /* Resolve relative path */
    char resolved[256];
    const char *p = pathname;
    if (p[0] != '/') {
        int ci = 0;
        while (aios_cwd[ci] && ci < 250) { resolved[ci] = aios_cwd[ci]; ci++; }
        if (ci > 1) resolved[ci++] = '/';
        while (*p && ci < 255) resolved[ci++] = *p++;
        resolved[ci] = 0;
        p = resolved;
    }

    int pl = str_len(p);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)p[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap,
        seL4_MessageInfo_new(14 /* FS_MKDIR */, 0, 0, mr));
    return ((int)(long)seL4_GetMR(0) == 0) ? 0 : -EIO;
}

static long aios_sys_utimensat(va_list ap) {
    /* Stub — ignore timestamps for now */
    (void)ap;
    return 0;
}

static long aios_sys_umask(va_list ap) {
    int mask = va_arg(ap, int);
    (void)mask;
    return 022; /* return old umask */
}

/* unlinkat — rm uses this */
static long aios_sys_unlinkat(va_list ap) {
    int dirfd = va_arg(ap, int);
    const char *pathname = va_arg(ap, const char *);
    int flags = va_arg(ap, int);
    (void)dirfd; (void)flags;
    if (!fs_ep_cap) return -ENOSYS;

    char resolved[256];
    const char *p = pathname;
    if (p[0] != '/') {
        int ci = 0;
        while (aios_cwd[ci] && ci < 250) { resolved[ci] = aios_cwd[ci]; ci++; }
        if (ci > 1) resolved[ci++] = '/';
        while (*p && ci < 255) resolved[ci++] = *p++;
        resolved[ci] = 0;
        p = resolved;
    }

    int pl = str_len(p);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)p[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap,
        seL4_MessageInfo_new(16 /* FS_UNLINK */, 0, 0, mr));
    return ((int)(long)seL4_GetMR(0) == 0) ? 0 : -EIO;
}

/* creat — cp uses open with O_CREAT */
static long aios_sys_openat_creat(const char *pathname, int flags, int mode) {
    if (!fs_ep_cap) return -ENOSYS;

    /* If creating, write empty file first */
    if (flags & 0100) { /* O_CREAT = 0100 */
        char resolved[256];
        const char *p = pathname;
        if (p[0] != '/') {
            int ci = 0;
            while (aios_cwd[ci] && ci < 250) { resolved[ci] = aios_cwd[ci]; ci++; }
            if (ci > 1) resolved[ci++] = '/';
            while (*p && ci < 255) resolved[ci++] = *p++;
            resolved[ci] = 0;
            p = resolved;
        }
        int pl = str_len(p);
        seL4_SetMR(0, (seL4_Word)pl);
        int mr = 1;
        seL4_Word w = 0;
        for (int i = 0; i < pl; i++) {
            w |= ((seL4_Word)(uint8_t)p[i]) << ((i % 8) * 8);
            if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
        }
        seL4_SetMR(mr, 0); /* data_len = 0 */
        seL4_Call(fs_ep_cap,
            seL4_MessageInfo_new(15 /* FS_WRITE_FILE */, 0, 0, mr + 1));
    }
    return -ENOSYS; /* Let normal open handle reading */
}

static long aios_sys_chdir(va_list ap) {
    const char *path = va_arg(ap, const char *);
    if (!path) return -ENOENT;
    if (path[0] == '/') {
        aios_set_cwd(path);
    } else if (path[0] == '.' && path[1] == 0) {
        /* stay */
    } else {
        char resolved[256];
        int ci = 0;
        while (aios_cwd[ci] && ci < 250) { resolved[ci] = aios_cwd[ci]; ci++; }
        if (ci > 1) resolved[ci++] = '/';
        while (*path && ci < 255) resolved[ci++] = *path++;
        resolved[ci] = 0;
        aios_set_cwd(resolved);
    }
    return 0;
}

static long aios_sys_getcwd(va_list ap) {
    char *buf = va_arg(ap, char *);
    size_t size = va_arg(ap, size_t);
    int len = str_len(aios_cwd);
    if ((int)size <= len) return -ERANGE;
    for (int i = 0; i <= len; i++) buf[i] = aios_cwd[i];
    return (long)buf;
}

static int aios_pid = 1;

static long aios_sys_getpid(va_list ap) {
    (void)ap;
    return (long)aios_pid;
}

static long aios_sys_getppid(va_list ap) {
    (void)ap;
    return 0;
}

static long aios_sys_getuid(va_list ap) { (void)ap; return (long)aios_uid; }
static long aios_sys_geteuid(va_list ap) { (void)ap; return (long)aios_uid; }
static long aios_sys_getgid(va_list ap) { (void)ap; return (long)aios_gid; }
static long aios_sys_getegid(va_list ap) { (void)ap; return (long)aios_gid; }

static long aios_sys_uname(va_list ap) {
    struct utsname *buf = va_arg(ap, struct utsname *);
    /* Zero then fill */
    char *p = (char *)buf;
    for (int i = 0; i < (int)sizeof(struct utsname); i++) p[i] = 0;

    const char *s = "AIOS";
    for (int i = 0; s[i]; i++) buf->sysname[i] = s[i];
    s = "aios";
    for (int i = 0; s[i]; i++) buf->nodename[i] = s[i];
    /* Get release from kernel via IPC */
    if (fs_ep_cap) {
        seL4_MessageInfo_t ur = seL4_Call(fs_ep_cap,
            seL4_MessageInfo_new(17 /* FS_UNAME */, 0, 0, 0));
        /* MR0-1=sysname, MR2-3=nodename, MR4-5=release, MR6-7=version, MR8-9=machine */
        int nlen = (int)seL4_MessageInfo_get_length(ur);
        if (nlen >= 10) {
            /* Unpack all 5 fields (16 bytes each, 2 MRs each) */
            char *fields[5] = { buf->sysname, buf->nodename, buf->release, buf->version, buf->machine };
            for (int f = 0; f < 5; f++) {
                seL4_Word w0 = seL4_GetMR(f * 2);
                seL4_Word w1 = seL4_GetMR(f * 2 + 1);
                for (int j = 0; j < 8; j++) fields[f][j] = (char)((w0 >> (j*8)) & 0xFF);
                for (int j = 0; j < 8; j++) fields[f][8+j] = (char)((w1 >> (j*8)) & 0xFF);
            }
            return 0;
        }
    }
    /* Fallback */
    s = "0.4.37";
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

/* ── getdents64 — directory reading ── */
static long aios_sys_getdents64(va_list ap) {
    int fd = va_arg(ap, int);
    void *dirp = va_arg(ap, void *);
    size_t count = va_arg(ap, size_t);

    if (fd < AIOS_FD_BASE || fd >= AIOS_FD_BASE + AIOS_MAX_FDS)
        return -EBADF;

    aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
    if (!f->active || !f->is_dir) return -ENOTDIR;

    int avail = f->size - f->pos;
    if (avail <= 0) return 0;

    /* Copy dirent records that fit */
    int copied = 0;
    char *out = (char *)dirp;
    while (f->pos < f->size && copied + 24 <= (int)count) {
        /* Read reclen from current position */
        uint16_t reclen = (uint8_t)f->data[f->pos + 16] | ((uint8_t)f->data[f->pos + 17] << 8);
        if (reclen == 0 || copied + reclen > (int)count) break;
        for (int i = 0; i < reclen; i++) out[copied + i] = f->data[f->pos + i];
        f->pos += reclen;
        copied += reclen;
    }
    return copied;
}

/* ── rename syscall ── */
static long aios_sys_renameat(va_list ap) {
    int olddirfd = va_arg(ap, int);
    const char *oldpath = va_arg(ap, const char *);
    int newdirfd = va_arg(ap, int);
    const char *newpath = va_arg(ap, const char *);
    (void)olddirfd; (void)newdirfd;
    if (!fs_ep_cap || !oldpath || !newpath) return -ENOSYS;

    /* Resolve paths */
    char old_full[256], new_full[256];
    if (oldpath[0] != '/') {
        int i = 0;
        const char *c = aios_cwd;
        while (*c && i < 250) old_full[i++] = *c++;
        if (i > 1) old_full[i++] = '/';
        while (*oldpath && i < 255) old_full[i++] = *oldpath++;
        old_full[i] = '\0';
    } else {
        int i = 0; while (*oldpath && i < 255) old_full[i++] = *oldpath++; old_full[i] = '\0';
    }
    if (newpath[0] != '/') {
        int i = 0;
        const char *c = aios_cwd;
        while (*c && i < 250) new_full[i++] = *c++;
        if (i > 1) new_full[i++] = '/';
        while (*newpath && i < 255) new_full[i++] = *newpath++;
        new_full[i] = '\0';
    } else {
        int i = 0; while (*newpath && i < 255) new_full[i++] = *newpath++; new_full[i] = '\0';
    }

    /* Pack both paths: MR0=oldlen, MR1..=old, then newlen, new.. */
    int olen = 0; while (old_full[olen]) olen++;
    int nlen = 0; while (new_full[nlen]) nlen++;
    seL4_SetMR(0, (seL4_Word)olen);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < olen; i++) {
        w |= ((seL4_Word)(uint8_t)old_full[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == olen - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_SetMR(mr++, (seL4_Word)nlen);
    w = 0;
    for (int i = 0; i < nlen; i++) {
        w |= ((seL4_Word)(uint8_t)new_full[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == nlen - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap, seL4_MessageInfo_new(18 /* FS_RENAME */, 0, 0, mr));
    long result = (long)seL4_GetMR(0);
    return result;
}

/* ── ftruncate syscall (stub) ── */
static long aios_sys_ftruncate(va_list ap) {
    int fd = va_arg(ap, int);
    long length = va_arg(ap, long);
    (void)fd; (void)length;
    return 0;  /* stub — pretend success */
}

/* ── real write to file fds ── */
/* (write to aios_fd is handled in aios_sys_write already for pipes,
    but regular file fds need FS_WRITE_FILE support) */

/* ── pipe() syscall ── */
static long aios_sys_pipe2(va_list ap) {
    int *fds = va_arg(ap, int *);
    int flags = va_arg(ap, int);
    (void)flags;
    if (!pipe_ep) return -ENOSYS;

    /* Create pipe via IPC */
    seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
        seL4_MessageInfo_new(60 /* PIPE_CREATE */, 0, 0, 0));
    int pipe_id = (int)(long)seL4_GetMR(0);
    if (pipe_id < 0) return -ENOMEM;

    /* Allocate two fds: read end and write end */
    int ri = aios_fd_alloc();
    if (ri < 0) return -EMFILE;
    int wi = aios_fd_alloc();
    if (wi < 0) { aios_fds[ri].active = 0; return -EMFILE; }

    aios_fds[ri].active = 1;
    aios_fds[ri].is_pipe = 1;
    aios_fds[ri].pipe_id = pipe_id;
    aios_fds[ri].pipe_read = 1;

    aios_fds[wi].active = 1;
    aios_fds[wi].is_pipe = 1;
    aios_fds[wi].pipe_id = pipe_id;
    aios_fds[wi].pipe_read = 0;

    fds[0] = AIOS_FD_BASE + ri;  /* read end */
    fds[1] = AIOS_FD_BASE + wi;  /* write end */
    return 0;
}

/* ── getpwuid / getpwnam via auth_ep IPC ── */
static struct passwd _pw_buf;
static char _pw_name[32];
static char _pw_dir[64];
static char _pw_shell[64];

/* Unpack a string from MRs starting at given index, return next MR index */
static int _unpack_mr_string(int start_mr, char *buf, int maxlen) {
    seL4_Word slen = seL4_GetMR(start_mr);
    int len = (int)slen;
    if (len > maxlen - 1) len = maxlen - 1;
    int mr = start_mr + 1;
    for (int i = 0; i < len; i++) {
        if (i % 8 == 0 && i > 0) mr++;
        buf[i] = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
    }
    buf[len] = '\0';
    return start_mr + 1 + (len + 7) / 8;
}

struct passwd *__wrap_getpwuid(uid_t uid) {
    if (!auth_ep) {
        /* Fallback: return minimal entry from stored uid */
        _pw_buf.pw_uid = aios_uid;
        _pw_buf.pw_gid = aios_gid;
        _pw_buf.pw_name = "root";
        _pw_buf.pw_passwd = "x";
        _pw_buf.pw_gecos = "";
        _pw_buf.pw_dir = "/";
        _pw_buf.pw_shell = "/bin/sh";
        return &_pw_buf;
    }

    seL4_SetMR(0, (seL4_Word)uid);
    seL4_Call(auth_ep, seL4_MessageInfo_new(51, 0, 0, 1));

    if (seL4_GetMR(0) != 0) return NULL;

    _pw_buf.pw_uid = uid;
    _pw_buf.pw_gid = (gid_t)seL4_GetMR(1);
    int mr = _unpack_mr_string(2, _pw_name, 32);
    mr = _unpack_mr_string(mr, _pw_dir, 64);
    _unpack_mr_string(mr, _pw_shell, 64);

    _pw_buf.pw_name = _pw_name;
    _pw_buf.pw_passwd = "x";
    _pw_buf.pw_gecos = _pw_name;
    _pw_buf.pw_dir = _pw_dir;
    _pw_buf.pw_shell = _pw_shell;
    return &_pw_buf;
}

struct passwd *__wrap_getpwnam(const char *name) {
    /* Simple approach: try uid 0 and 1000 (common defaults) */
    /* A proper implementation would need a NAME_LOOKUP IPC */
    struct passwd *pw;
    pw = __wrap_getpwuid(0);
    if (pw && pw->pw_name[0]) {
        int match = 1;
        const char *a = pw->pw_name, *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) return pw;
    }
    for (uid_t uid = 1000; uid < 1016; uid++) {
        pw = __wrap_getpwuid(uid);
        if (pw && pw->pw_name[0]) {
            int match = 1;
            const char *a = pw->pw_name, *b = name;
            while (*a && *b && *a == *b) { a++; b++; }
            if (*a == *b) return pw;
        }
    }
    return NULL;
}

/* ── pthreads (IPC to thread_server + userspace spinlocks) ── */

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void *), void *arg) {
    (void)attr;
    if (!thread_ep) return 11; /* EAGAIN — no thread server */

    seL4_SetMR(0, (seL4_Word)(uintptr_t)start_routine);
    seL4_SetMR(1, (seL4_Word)(uintptr_t)arg);
    seL4_MessageInfo_t reply = seL4_Call(thread_ep,
        seL4_MessageInfo_new(AIOS_THREAD_CREATE, 0, 0, 2));

    seL4_Word tid = seL4_GetMR(0);
    if ((long)tid <= 0) return 11; /* EAGAIN */
    if (thread) *thread = (pthread_t)tid;
    return 0;
}

int __wrap_pthread_join(pthread_t th, void **retval) {
    if (!thread_ep) return 3; /* ESRCH */

    seL4_SetMR(0, (seL4_Word)th);
    seL4_Call(thread_ep,
        seL4_MessageInfo_new(AIOS_THREAD_JOIN, 0, 0, 1));

    if (retval) *retval = NULL;
    return (int)(long)seL4_GetMR(0);
}

void __wrap_pthread_exit(void *retval) {
    (void)retval;
    /* Trigger VM fault — thread_server catches on fault ep */
    volatile int *null_ptr = (volatile int *)0;
    *null_ptr = 0;
    __builtin_unreachable();
}

int __wrap_pthread_detach(pthread_t th) {
    (void)th;
    return 0; /* stub — detached threads still cleaned up on process exit */
}

int __wrap_pthread_mutex_init(pthread_mutex_t *mutex,
                               const pthread_mutexattr_t *attr) {
    (void)attr;
    if (!mutex) return 22; /* EINVAL */
    /* Zero the struct — __lock is the first int field */
    char *p = (char *)mutex;
    for (int i = 0; i < (int)sizeof(pthread_mutex_t); i++) p[i] = 0;
    return 0;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (!mutex) return 22;
    volatile int *lock = (volatile int *)mutex;
    while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE)) {
        seL4_Yield(); /* yield to avoid burning CPU at equal priority */
    }
    return 0;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex) return 22;
    volatile int *lock = (volatile int *)mutex;
    __atomic_clear(lock, __ATOMIC_RELEASE);
    return 0;
}

int __wrap_pthread_mutex_destroy(pthread_mutex_t *mutex) {
    (void)mutex;
    return 0;
}

/* ── Init ── */
/* Environment variables */
static char *aios_envp[] = {
    "HOME=/",
    "PATH=/bin",
    "USER=root",
    "SHELL=/bin/sh",
    "TERM=vt100",
    "HOSTNAME=aios",
    NULL
};

/* ── waitpid/wait4 ── */
static long aios_sys_wait4(va_list ap) {
    int pid = va_arg(ap, int);
    int *wstatus = va_arg(ap, int *);
    /* int options = va_arg(ap, int); — ignored for now */
    /* struct rusage *rusage = va_arg(ap, struct rusage *); — ignored */

    if (!pipe_ep) return -38;  /* ENOSYS */

    /* Send PIPE_WAIT to pipe_server */
    seL4_SetMR(0, (seL4_Word)pid);
    seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
        seL4_MessageInfo_new(PIPE_WAIT, 0, 0, 1));

    long child_pid = (long)seL4_GetMR(0);
    int exit_status = (int)seL4_GetMR(1);

    if (wstatus && child_pid > 0) {
        /* Encode exit status in wait format: (status & 0xff) << 8 */
        *wstatus = (exit_status & 0xff) << 8;
    }

    return child_pid;
}

/* ── fork/clone ── */
static long aios_sys_clone(va_list ap) {
    /* On AArch64, clone(flags, stack, ...) — basic fork has flags=SIGCHLD, stack=0 */
    (void)ap;
    if (!pipe_ep) return -38; /* -ENOSYS */

    /* Send PIPE_FORK to pipe_server (badged, so server knows who we are) */
    seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
        seL4_MessageInfo_new(PIPE_FORK, 0, 0, 0));
    long result = (long)seL4_GetMR(0);
    /* result == 0 for child, child_pid for parent */
    /* Child's PID is patched directly into aios_pid by do_fork */
    return result;
}

/* sel4runtime exit callback — sends exit code via IPC before faulting */
static void aios_exit_cb(int code) {
    if (pipe_ep) {
        seL4_SetMR(0, (seL4_Word)code);
        seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_EXIT, 0, 0, 1));
    }
    /* Fault to trigger reaper */
    volatile int *p = (volatile int *)0;
    *p = 0;
    __builtin_unreachable();
}

void aios_init(seL4_CPtr serial_ep, seL4_CPtr fs_endpoint) {
    /* Skip if already initialized (e.g. by __wrap_main) */
    if (ser_ep && serial_ep == 0) return;
    ser_ep = serial_ep;
    fs_ep_cap = fs_endpoint;

    /* Set libc environ */
    extern char **environ;
    environ = aios_envp;

    /* Set CWD from PWD env if available */
    for (int i = 0; aios_envp[i]; i++) {
        if (aios_envp[i][0]=='.' && aios_envp[i][1]=='.' && aios_envp[i][2]=='.' && aios_envp[i][3]=='.') {
            aios_set_cwd(aios_envp[i] + 4);
            break;
        }
    }

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
/* fstatat on aarch64 */
    muslcsys_install_syscall(__NR_fstatat, aios_sys_fstatat);

    /* Easy POSIX stubs */
    muslcsys_install_syscall(__NR_exit, aios_sys_exit);
#ifdef __NR_mkdirat
    muslcsys_install_syscall(__NR_mkdirat, aios_sys_mkdirat);
#endif
#ifdef __NR_utimensat
    muslcsys_install_syscall(__NR_utimensat, aios_sys_utimensat);
#endif
#ifdef __NR_umask
    muslcsys_install_syscall(__NR_umask, aios_sys_umask);
#endif
#ifdef __NR_unlinkat
    muslcsys_install_syscall(__NR_unlinkat, aios_sys_unlinkat);
#endif
    muslcsys_install_syscall(__NR_exit_group, aios_sys_exit_group);
    muslcsys_install_syscall(__NR_chdir, aios_sys_chdir);
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
    muslcsys_install_syscall(__NR_getdents64, aios_sys_getdents64);
#ifdef __NR_clone
    muslcsys_install_syscall(__NR_clone, aios_sys_clone);
#endif
#ifdef __NR_wait4
    muslcsys_install_syscall(__NR_wait4, aios_sys_wait4);
#endif
#ifdef __NR_exit_group
    muslcsys_install_syscall(__NR_exit_group, aios_sys_exit_group);
#endif

    /* Register exit callback so sel4runtime_exit sends exit code via IPC */
    sel4runtime_set_exit(aios_exit_cb);
#ifdef __NR_pipe2
    muslcsys_install_syscall(__NR_pipe2, aios_sys_pipe2);
#endif
#ifdef __NR_renameat
    muslcsys_install_syscall(__NR_renameat, aios_sys_renameat);
#endif
#ifdef __NR_renameat2
    muslcsys_install_syscall(__NR_renameat2, aios_sys_renameat);
#endif
#ifdef __NR_ftruncate
    muslcsys_install_syscall(__NR_ftruncate, aios_sys_ftruncate);
#endif
}

void aios_init_full(seL4_CPtr serial_ep_arg, seL4_CPtr fs_ep_arg,
                     seL4_CPtr thread_ep_arg) {
    thread_ep = thread_ep_arg;
    aios_init(serial_ep_arg, fs_ep_arg);
}


/* __wrap_main: intercepts main() to strip cap args from argv.
 * exec_thread passes: argv[0]=serial_ep, argv[1]=fs_ep, argv[2..]=real args
 * We init the POSIX shim, then call real main with clean argv.
 */
static long _auto_parse(const char *s) {
    if (!s) return 0;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int __real_main(int argc, char **argv);

int __wrap_main(int argc, char **argv) {
    /* argv layout: [serial_ep, fs_ep, thread_ep, auth_ep, pipe_ep, CWD, progname, arg1, ...] */
    seL4_CPtr serial = 0, fs = 0, thr = 0, ath = 0, pip = 0;
    if (argc > 0 && argv[0]) serial = (seL4_CPtr)_auto_parse(argv[0]);
    if (argc > 1 && argv[1]) fs = (seL4_CPtr)_auto_parse(argv[1]);
    if (argc > 2 && argv[2]) thr = (seL4_CPtr)_auto_parse(argv[2]);
    if (argc > 3 && argv[3]) ath = (seL4_CPtr)_auto_parse(argv[3]);
    if (argc > 4 && argv[4]) pip = (seL4_CPtr)_auto_parse(argv[4]);
    thread_ep = thr;
    auth_ep = ath;
    pipe_ep = pip;
    aios_init(serial, fs);

    /* Parse uid:gid:/path from argv[5] */
    if (argc > 5 && argv[5]) {
        const char *s = argv[5];
        if (s[0] >= '0' && s[0] <= '9') {
            /* Format: uid:gid:[spipe:rpipe:]/path */
            uint32_t uid = 0;
            while (*s >= '0' && *s <= '9') { uid = uid * 10 + (*s - '0'); s++; }
            if (*s == ':') s++;
            uint32_t gid = 0;
            while (*s >= '0' && *s <= '9') { gid = gid * 10 + (*s - '0'); s++; }
            if (*s == ':') s++;
            aios_uid = uid;
            aios_gid = gid;
            /* Check for optional spipe:rpipe: before /path */
            if (*s >= '0' && *s <= '9') {
                int sp = 0;
                while (*s >= '0' && *s <= '9') { sp = sp * 10 + (*s - '0'); s++; }
                if (*s == ':') s++;
                int rp = 0;
                while (*s >= '0' && *s <= '9') { rp = rp * 10 + (*s - '0'); s++; }
                if (*s == ':') s++;
                if (sp != 99) stdout_pipe_id = sp;
                if (rp != 99) stdin_pipe_id = rp;
            }
            if (*s == '/') aios_set_cwd(s);
        } else if (s[0] == '/') {
            aios_set_cwd(s);
        }
    }

    /* Strip 6 args (ser, fs, thread, auth, pipe, cwd): real main sees [progname, arg1, ...] */
    return __real_main(argc - 6, argv + 6);
}
