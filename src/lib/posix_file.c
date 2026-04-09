/*
 * posix_file.c -- AIOS POSIX file I/O syscall handlers
 * open, openat, read, write, close, lseek, writev, readv, ftruncate
 */
#include "posix_internal.h"

/* v0.4.67: SHM pipe transfer -- request xfer page mapping from server.
 * Cap copies cleaned up by pipe_maybe_free (CNode_Revoke + cslot free). */
static void pipe_request_shm(aios_fd_t *f) {
    if (!pipe_ep || f->shm_vaddr) return;
    seL4_SetMR(0, (seL4_Word)f->pipe_id);
    seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
        seL4_MessageInfo_new(78 /* PIPE_MAP_SHM */, 0, 0, 1));
    seL4_Word vaddr = seL4_GetMR(0);
    if (vaddr) f->shm_vaddr = (char *)vaddr;
}

long aios_sys_open(va_list ap) {
    const char *pathname = va_arg(ap, const char *);
    int flags = va_arg(ap, int);
    va_arg(ap, int); /* mode */

    /* v0.4.64: /dev/null -- infinite sink, empty source */
    {
        const char *dn = "/dev/null";
        int dnmatch = 1;
        for (int i = 0; dn[i]; i++)
            if (pathname[i] != dn[i]) { dnmatch = 0; break; }
        if (dnmatch && pathname[9] == 0) {
            int idx = aios_fd_alloc();
            if (idx < 0) return -EMFILE;
            aios_fd_t *f = &aios_fds[idx];
            f->active = 1;
            f->is_dir = 0;
            f->is_pipe = 0;
            f->is_devnull = 1;
            f->size = 0;
            f->pos = 0;
            f->path[0] = 0;
            return AIOS_FD_BASE + idx;
        }
    }

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

    /* v0.4.64: /dev/null -- infinite sink, empty source */
    {
        const char *dn = "/dev/null";
        int dnmatch = 1;
        for (int i = 0; dn[i]; i++)
            if (pathname[i] != dn[i]) { dnmatch = 0; break; }
        if (dnmatch && pathname[9] == 0) {
            int idx = aios_fd_alloc();
            if (idx < 0) return -EMFILE;
            aios_fd_t *f = &aios_fds[idx];
            f->active = 1;
            f->is_dir = 0;
            f->is_pipe = 0;
            f->is_devnull = 1;
            f->size = 0;
            f->pos = 0;
            f->path[0] = 0;
            return AIOS_FD_BASE + idx;
        }
    }

    if (!fs_ep_cap) return -ENOENT;

    int is_dir = (flags & 040000) != 0;
    int is_creat = (flags & 0100) != 0;
    int is_wronly = ((flags & 3) == 1);
    int is_rdwr = ((flags & 3) == 2);
    int is_append = (flags & 02000) != 0;
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
        /* v0.4.72: write-only fd with resolved path and correct size */
        if (is_wronly || is_rdwr) {{
            int idx = aios_fd_alloc();
            if (idx < 0) return -EMFILE;
            aios_fd_t *f = &aios_fds[idx];
            f->active = 1;
            f->is_dir = 0;
            f->is_tty = 0;
            f->is_append = is_append;
            /* Use resolved path (p) not raw pathname */
            str_copy(f->path, p, sizeof(f->path));
            /* Get existing size for append mode */
            uint32_t _fm, _fs;
            if (is_append && fetch_stat(p, &_fm, &_fs) == 0) {{
                f->size = (int)_fs;
                f->pos = (int)_fs;
            }} else {{
                f->size = 0;
                f->pos = 0;
            }}
            return AIOS_FD_BASE + idx;
        }}
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
        /* Get file size first */
        uint32_t fmode, fsize;
        if (fetch_stat(res_path, &fmode, &fsize) != 0) return -ENOENT;
        str_copy(f->path, res_path, sizeof(f->path));
        f->is_append = is_append;
        if ((int)fsize <= (int)sizeof(f->data) - 1) {
            /* Small file: load into buffer via fetch_pread loop.
             * FS_CAT (fetch_file) truncates at ~952 bytes per IPC.
             * fetch_pread handles multi-round-trip correctly. */
            int loaded = 0;
            while (loaded < (int)fsize) {
                int got = fetch_pread(res_path, loaded,
                    f->data + loaded, (int)fsize - loaded);
                if (got <= 0) break;
                loaded += got;
            }
            if (fsize > 0 && loaded <= 0) return -ENOENT;  /* EMPTY_FILE_FIX_V072 */
            f->data[loaded] = 0;
            f->active = 1;
            f->size = loaded;
            f->pos = is_append ? loaded : 0;
        } else {
            /* Large file: demand-read via FS_PREAD */
            f->active = 1;
            f->size = (int)fsize;
            f->pos = is_append ? (int)fsize : 0;
            f->data[0] = 0;
        }
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
        /* v0.4.68: use TTY_READ for cooked input from line queue.
         * tty_server line discipline handles echo, backspace, Ctrl-U.
         * We get clean, edited lines -- no raw control chars. */
        char *cbuf = (char *)buf;
        int want = (int)count;
        if (want > 900) want = 900;
        seL4_CPtr sep = aios_get_serial_ep();
        while (1) {
            seL4_SetMR(0, (seL4_Word)want);
            seL4_MessageInfo_t reply = seL4_Call(sep,
                seL4_MessageInfo_new(71, 0, 0, 1));  /* TTY_READ */
            int got = (int)(long)seL4_GetMR(0);
            if (got > 0) {
                int mr = 1;
                for (int i = 0; i < got; i++) {
                    if (i > 0 && i % 8 == 0) mr++;
                    cbuf[i] = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
                }
                return (long)got;
            }
            seL4_Yield();
        }
    }

    /* aios fd */
    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        if (!f->active) return -EBADF;
        /* Pipe read */
        if (f->is_pipe && f->pipe_read && pipe_ep) {
            char *cbuf = (char *)buf;
            /* v0.4.66: try SHM path (up to 4096 bytes per call) */
            if (!f->shm_vaddr) pipe_request_shm(f);
            if (f->shm_vaddr) {
                int want = (int)count;
                if (want > 4096) want = 4096;
                seL4_SetMR(0, (seL4_Word)f->pipe_id);
                seL4_SetMR(1, (seL4_Word)want);
                seL4_Call(pipe_ep,
                    seL4_MessageInfo_new(80 /* PIPE_READ_SHM */, 0, 0, 2));
                int got = (int)(long)seL4_GetMR(0);
                for (int i = 0; i < got; i++)
                    cbuf[i] = f->shm_vaddr[i];
                return (long)got;
            }
            /* Fallback: MR-based read */
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
        if (f->size > (int)sizeof(f->data) - 1) {
            /* Large file: on-demand read via FS_PREAD */
            int total = 0;
            while (total < n) {
                int chunk = n - total;
                if (chunk > 900) chunk = 900;
                int got = fetch_pread(f->path, f->pos + total,
                                      dst + total, chunk);
                if (got <= 0) break;
                total += got;
            }
            f->pos += total;
            return total;
        }
        /* Small file: read from buffer */
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

    /* stdout/stderr -- check for file or pipe redirect REDIR_WRITE_V072 */
    if (fd == 1 || fd == 2) {
        int _redir = (fd == 1) ? stdout_redir_idx : stderr_redir_idx;
        if (_redir >= 0) {  /* REDIR_COPY_V072 */
            aios_fd_t *rf = (fd == 1) ? &stdout_redir_copy : &stderr_redir_copy;
            if (rf->path[0] && fs_ep_cap) {
                const char *src = (const char *)buf;
                size_t total = 0;
                while (total < count) {
                    int chunk = (int)(count - total);
                    if (chunk > 800) chunk = 800;
                    int wrote = fetch_pwrite(rf->path,
                        rf->is_append ? rf->size : rf->pos,
                        src + total, chunk);
                    if (wrote <= 0) break;
                    total += wrote;
                    rf->pos += wrote;
                    if (rf->pos > rf->size) rf->size = rf->pos;
                }
                return (long)total;
            }
        }
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
        if (f->is_devnull) return (long)count;
        /* Regular file write -- use FS_APPEND (19) for O_APPEND fds,
         * FS_WRITE_FILE (15) otherwise. v0.4.66 */
        if (!f->is_pipe && !f->is_dir && f->path[0] && fs_ep_cap) {
            /* v0.4.70: Write directly to disk via FS_PWRITE */
            const char *src = (const char *)buf;
            size_t total = 0;
            while (total < count) {
                int chunk = (int)(count - total);
                if (chunk > 800) chunk = 800;
                int wrote = fetch_pwrite(f->path,
                    f->is_append ? f->size : f->pos,
                    src + total, chunk);
                if (wrote <= 0) break;
                total += wrote;
                f->pos += wrote;
                if (f->pos > f->size) f->size = f->pos;
            }
            return (long)total;
        }
        if (f->is_pipe && !f->pipe_read && pipe_ep) {
            const char *src = (const char *)buf;
            /* v0.4.66: try SHM path (up to 4096 bytes per call) */
            if (!f->shm_vaddr) pipe_request_shm(f);
            if (f->shm_vaddr) {
                size_t sent = 0;
                while (sent < count) {
                    int chunk = (int)(count - sent);
                    if (chunk > 4096) chunk = 4096;
                    for (int i = 0; i < chunk; i++)
                        f->shm_vaddr[i] = src[sent + i];
                    seL4_SetMR(0, (seL4_Word)f->pipe_id);
                    seL4_SetMR(1, (seL4_Word)chunk);
                    seL4_Call(pipe_ep,
                        seL4_MessageInfo_new(79 /* PIPE_WRITE_SHM */, 0, 0, 2));
                    sent += chunk;
                }
                return (long)count;
            }
            /* Fallback: MR-based write */
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
        f->is_devnull = 0;
        f->pipe_id = -1;
        f->shm_vaddr = 0;
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
        if (f->is_devnull) {
            long total = 0;
            for (int i = 0; i < iovcnt; i++) total += (long)iov[i].iov_len;
            return total;
        }
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
        /* Regular file writev via FS_PWRITE */
        if (!f->is_pipe && !f->is_dir && f->path[0] && fs_ep_cap) {
            long total = 0;
            for (int i = 0; i < iovcnt; i++) {
                const char *src = (const char *)iov[i].iov_base;
                size_t iov_sent = 0;
                while (iov_sent < iov[i].iov_len) {
                    int chunk = (int)(iov[i].iov_len - iov_sent);
                    if (chunk > 800) chunk = 800;
                    int wrote = fetch_pwrite(f->path,
                        f->is_append ? f->size : f->pos,
                        src + iov_sent, chunk);
                    if (wrote <= 0) break;
                    iov_sent += wrote;
                    f->pos += wrote;
                    if (f->pos > f->size) f->size = f->pos;
                }
                total += (long)iov_sent;
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


/* v0.4.62: pread64 -- positional read, does not change file offset */
long aios_sys_pread64(va_list ap) {
    int fd = va_arg(ap, int);
    void *buf = va_arg(ap, void *);
    size_t count = va_arg(ap, size_t);
    off_t offset = va_arg(ap, off_t);

    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        if (!f->active) return -EBADF;
        if (f->is_pipe) return -ESPIPE;
        if (offset < 0 || offset >= f->size) return 0;
        int avail = f->size - (int)offset;
        int n = (int)count < avail ? (int)count : avail;
        char *dst = (char *)buf;
        for (int i = 0; i < n; i++) dst[i] = f->data[(int)offset + i];
        /* file position (f->pos) is NOT modified */
        return n;
    }
    return -EBADF;
}

/* v0.4.62: pwrite64 -- positional write, does not change file offset */
long aios_sys_pwrite64(va_list ap) {
    int fd = va_arg(ap, int);
    const void *buf = va_arg(ap, const void *);
    size_t count = va_arg(ap, size_t);
    off_t offset = va_arg(ap, off_t);
    (void)offset;

    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        if (!f->active) return -EBADF;
        if (f->is_pipe) return -ESPIPE;
        /* Write via FS_WRITE_FILE IPC (offset not yet honored by ext2) */
        if (!f->is_dir && f->path[0] && fs_ep_cap) {
            const char *s = (const char *)buf;
            int plen = 0; while (f->path[plen]) plen++;
            seL4_SetMR(0, (seL4_Word)plen);
            int mr = 1;
            seL4_Word w = 0;
            for (int i = 0; i < plen; i++) {
                w |= ((seL4_Word)(uint8_t)f->path[i]) << ((i % 8) * 8);
                if (i % 8 == 7 || i == plen - 1) { seL4_SetMR(mr++, w); w = 0; }
            }
            int wlen = (int)count;
            if (wlen > 3000) wlen = 3000;
            seL4_SetMR(mr++, (seL4_Word)wlen);
            w = 0;
            for (int i = 0; i < wlen; i++) {
                w |= ((seL4_Word)(uint8_t)s[i]) << ((i % 8) * 8);
                if (i % 8 == 7 || i == wlen - 1) { seL4_SetMR(mr++, w); w = 0; }
            }
            seL4_Call(fs_ep_cap, seL4_MessageInfo_new(15, 0, 0, mr));
            return (long)wlen;
        }
        return (long)count;
    }
    return -EBADF;
}
