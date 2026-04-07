/*
 * posix_file.c -- AIOS POSIX file I/O syscall handlers
 * open, openat, read, write, close, lseek, writev, readv, ftruncate
 */
#include "posix_internal.h"

long aios_sys_open(va_list ap) {
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

long aios_sys_openat(va_list ap) {
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

long aios_sys_read(va_list ap) {
    int fd = va_arg(ap, int);
    void *buf = va_arg(ap, void *);
    size_t count = va_arg(ap, size_t);

    /* Signal Phase 4: return EINTR if signal was delivered */
    if (aios_sig_check() > 0) return -EINTR;

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

long aios_sys_write(va_list ap) {
    int fd = va_arg(ap, int);
    const void *buf = va_arg(ap, const void *);
    size_t count = va_arg(ap, size_t);

    /* Signal Phase 4: return EINTR if signal was delivered */
    if (aios_sig_check() > 0) return -EINTR;

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

long aios_sys_close(va_list ap) {
    int fd = va_arg(ap, int);
    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        /* close() never sends PIPE_CLOSE_WRITE.
         * Reason: in fork+dup2+exec pipelines, the child does
         * dup2(write_fd, 1) then close(write_fd). If close sent
         * PIPE_CLOSE_WRITE, EOF fires before exec even runs.
         * Instead, aios_exit_cb sends PIPE_CLOSE_WRITE when the
         * writer process exits. The shell sends PIPE_CLOSE to
         * destroy the pipe buffer after the pipeline completes. */
        f->active = 0;
        f->is_pipe = 0;
        f->pipe_id = -1;
        return 0;
    }
    /* Reset stdin/stdout pipe redirect on close */
    if (fd == 0) { stdin_pipe_id = -1; return 0; }
    if (fd == 1 || fd == 2) { stdout_pipe_id = -1; return 0; }
    return -EBADF;
}

long aios_sys_lseek(va_list ap) {
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

long aios_sys_writev(va_list ap) {
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

    /* writev: pipe fd and regular aios fd support */
    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        if (!f->active) return -EBADF;
        if (f->is_pipe && !f->pipe_read && pipe_ep) {
            long total = 0;
            for (int i = 0; i < iovcnt; i++) {
                const char *src = (const char *)iov[i].iov_base;
                size_t sent = 0;
                while (sent < iov[i].iov_len) {
                    int chunk = (int)(iov[i].iov_len - sent);
                    if (chunk > 900) chunk = 900;
                    seL4_SetMR(0, (seL4_Word)f->pipe_id);
                    seL4_SetMR(1, (seL4_Word)chunk);
                    int mr = 2;
                    seL4_Word w = 0;
                    for (int j = 0; j < chunk; j++) {
                        w |= ((seL4_Word)(uint8_t)src[sent + j]) << ((j % 8) * 8);
                        if (j % 8 == 7 || j == chunk - 1) { seL4_SetMR(mr++, w); w = 0; }
                    }
                    seL4_Call(pipe_ep, seL4_MessageInfo_new(61, 0, 0, mr));
                    sent += chunk;
                }
                total += (long)iov[i].iov_len;
            }
            return total;
        }
    }
    return -EBADF;
}

long aios_sys_readv(va_list ap) {
    int fd = va_arg(ap, int);
    struct iovec { void *iov_base; size_t iov_len; };
    struct iovec *iov = va_arg(ap, struct iovec *);
    int iovcnt = va_arg(ap, int);

    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
            aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
            if (!f->active) return -EBADF;
            /* readv: pipe read via IPC */
            if (f->is_pipe && f->pipe_read && pipe_ep) {
                char *dst = (char *)iov[i].iov_base;
                int want = (int)iov[i].iov_len;
                if (want > 900) want = 900;
                seL4_SetMR(0, (seL4_Word)f->pipe_id);
                seL4_SetMR(1, (seL4_Word)want);
                seL4_Call(pipe_ep, seL4_MessageInfo_new(62, 0, 0, 2));
                int got = (int)(long)seL4_GetMR(0);
                int mr = 1;
                for (int k = 0; k < got; k++) {
                    if (k % 8 == 0 && k > 0) mr++;
                    dst[k] = (char)((seL4_GetMR(mr) >> ((k % 8) * 8)) & 0xFF);
                }
                total += got;
                if (got < (int)iov[i].iov_len) break;
            } else {
                int avail = f->size - f->pos;
                int n = (int)iov[i].iov_len < avail ? (int)iov[i].iov_len : avail;
                char *dst = (char *)iov[i].iov_base;
                for (int j = 0; j < n; j++) dst[j] = f->data[f->pos + j];
                f->pos += n;
                total += n;
                if (n < (int)iov[i].iov_len) break;
            }
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

long aios_sys_openat_creat(const char *pathname, int flags, int mode) {
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

long aios_sys_ftruncate(va_list ap) {
    int fd = va_arg(ap, int);
    long length = va_arg(ap, long);
    (void)fd; (void)length;
    return 0;  /* stub — pretend success */
}

