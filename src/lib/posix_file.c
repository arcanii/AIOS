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
    seL4_SetMR(1, 0);   /* v0.4.257: dir=0 = READ xfer page */
    seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
        seL4_MessageInfo_new(78 /* PIPE_MAP_SHM */, 0, 0, 2));
    seL4_Word vaddr = seL4_GetMR(0);
    if (vaddr) f->shm_vaddr = (char *)vaddr;
}

/* v0.4.257 coalescing: the WRITE-direction xfer page for the current stdout pipe.
 * Mapped lazily + cached (remapped if stdout_pipe_id changes). Lets a writer push up
 * to 4KB per Call (PIPE_WRITE_SHM) instead of looping 900B MR-packed PIPE_WRITE. */
static char *stdout_wshm = (void *)0;
static int   stdout_wshm_pid = -1;
static void stdout_request_wshm(void) {
    if (!pipe_ep || stdout_pipe_id < 0) return;
    if (stdout_wshm && stdout_wshm_pid == stdout_pipe_id) return;
    seL4_SetMR(0, (seL4_Word)stdout_pipe_id);
    seL4_SetMR(1, 1);   /* dir=1 = WRITE xfer page */
    seL4_Call(pipe_ep, seL4_MessageInfo_new(78 /* PIPE_MAP_SHM */, 0, 0, 2));
    seL4_Word v = seL4_GetMR(0);
    stdout_wshm = v ? (char *)v : (void *)0;
    stdout_wshm_pid = v ? stdout_pipe_id : -1;
}

/* ===================== v0.4.298: bulk file write fast path =====================
 * One PIPE_PWRITE_BULK Call has pipe_server page-bounce the caller's source buffer
 * and vfs_pwrite it, replacing ceil(count/800) MR-packed FS_PWRITE round-trips for
 * a large file write (tcc linker output, cp). Server-gated by /proc/pwritebulk
 * (default OFF -> reply -ENOSYS -> fall back to the legacy loop, cached in
 * bulk_pwrite_state so a disabled box probes at most ONCE per process image -> the
 * default path is steady-state byte-identical). Returns bytes written (>0), or 0 to
 * tell the caller to use its legacy loop (feature off, too small, or a soft failure
 * like EPERM / an unmappable source page). Never negative. */
#define PIPE_PWRITE_BULK_L 94
#define BULK_PWRITE_MIN    4096    /* below this the legacy MR loop is already cheap */
#define BULK_PWRITE_CHUNK  65536   /* cap per IPC so pipe_server is not held for a whole multi-MB write */
static int bulk_pwrite_state = 0;  /* 0 unknown, 1 enabled, -1 disabled (don't re-probe) */

static long aios_bulk_pwrite(const char *path, int offset, const char *src, size_t count) {
    if (count < BULK_PWRITE_MIN || offset < 0 || !pipe_ep || bulk_pwrite_state < 0)
        return 0;
    if (count > BULK_PWRITE_CHUNK) count = BULK_PWRITE_CHUNK;  /* caller loops the rest */
    int pl = str_len(path);
    if (pl <= 0 || pl > 127) return 0;
    seL4_SetMR(0, (seL4_Word)src);          /* server reads our pages by vaddr */
    seL4_SetMR(1, (seL4_Word)count);
    seL4_SetMR(2, (seL4_Word)offset);
    seL4_SetMR(3, (seL4_Word)pl);
    int mr = 4;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_PWRITE_BULK_L, 0, 0, mr));
    long r = (long)seL4_GetMR(0);
    if (r == -ENOSYS) { bulk_pwrite_state = -1; return 0; }  /* off -> legacy, cached */
    bulk_pwrite_state = 1;
    return r > 0 ? r : 0;   /* EPERM / soft-fail (<=0) -> caller falls back to legacy */
}

/* ===================== v0.4.258: direct SPSC SHM-ring pipes =====================
 *
 * When a pipe was armed in ring mode (/proc/shmring) the writer and reader move
 * bytes through a shared ring in USERSPACE -- no per-chunk seL4_Call -- so a
 * `cmd | cmd` pipeline spans cores instead of serialising every chunk through the
 * core-0 pipe_server. The server is touched only at empty/full: a reader that
 * finds the ring empty blocks via the normal PIPE_READ / PIPE_READ_SHM path (the
 * server pulls from the SAME ring); a writer that just made data available wakes
 * a parked reader via PIPE_RING_WAKE. pipe_map_ring() returns 0 when the pipe is
 * not ring-armed, so every caller transparently falls back to the legacy path.
 * Cross-core coherency rests on the cacheable-inner-shareable mapping + the
 * release/acquire/fence barriers in shm_ring.h (QEMU cannot model it -- HW soak). */
#include "aios/shm_ring.h"

#define PIPE_MAP_RING_L   91
#define PIPE_RING_WAKE_L  92

/* Map the ring frame for pipe_id (dir 0=read, 1=write); returns the vaddr or 0. */
static char *pipe_map_ring(int pipe_id, int dir) {
    if (!pipe_ep || pipe_id < 0) return (void *)0;
    seL4_SetMR(0, (seL4_Word)pipe_id);
    seL4_SetMR(1, (seL4_Word)dir);
    seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_MAP_RING_L, 0, 0, 2));
    seL4_Word v = seL4_GetMR(0);
    return v ? (char *)v : (void *)0;
}

/* Cached ring vaddrs for the stdin (reader) and stdout (writer) redirects.
 * Keyed by pipe id: a single map attempt per pipe (re-attempt only if the id
 * changes), so a non-ring pipe costs ONE failed Call, not one per read/write. */
static char *stdin_ring = (void *)0;   static int stdin_ring_pid  = -2;
static char *stdout_ring = (void *)0;  static int stdout_ring_pid = -2;
static char *stdin_ring_get(void) {
    if (stdin_pipe_id < 0 || !pipe_ep) return (void *)0;
    if (stdin_ring_pid != stdin_pipe_id) {
        stdin_ring = pipe_map_ring(stdin_pipe_id, 0);
        stdin_ring_pid = stdin_pipe_id;   /* mark attempted, even on 0 */
    }
    return stdin_ring;
}
static char *stdout_ring_get(void) {
    if (stdout_pipe_id < 0 || !pipe_ep) return (void *)0;
    if (stdout_ring_pid != stdout_pipe_id) {
        stdout_ring = pipe_map_ring(stdout_pipe_id, 1);
        stdout_ring_pid = stdout_pipe_id;
    }
    return stdout_ring;
}

/* Direct ring read. Returns >0 bytes copied, 0 on EOF, or -1 when the ring is
 * empty and the writer is still live (caller must block via the server). */
static int shm_ring_recv_fast(char *ring, char *dst, int want) {
    shm_ring_hdr_t *h = (shm_ring_hdr_t *)ring;
    const char *data = shm_ring_data(ring);
    uint32_t head = h->head;            /* reader owns head -- plain load of our own index */
    uint32_t tail = h->tail;
    shm_ring_observe();                 /* acquire: order the tail load BEFORE the data reads */
    uint32_t avail = tail - head;
    if (avail == 0) {
        /* Empty: distinguish a true EOF from a transient gap WITHOUT dropping the
         * writer's final bytes. Read writer_closed FIRST, then (acquire) re-read
         * tail, so observing closed=1 implies observing every pre-close write --
         * the writer/server set writer_closed only after the final tail publish is
         * globally visible. The barrier MUST sit BETWEEN the two loads: without it
         * the A72 can satisfy them out of order and see closed=1 with a stale tail,
         * a false EOF that silently loses the tail of the stream. */
        int closed = (int)h->writer_closed;
        shm_ring_observe();             /* acquire: closed-load BEFORE the tail re-load */
        tail = h->tail;
        avail = tail - head;
        if (avail == 0)
            return closed ? 0 : -1;
        shm_ring_observe();             /* acquire: re-loaded tail BEFORE the data reads */
    }
    int n = (int)avail;
    if (n > want) n = want;
    for (int i = 0; i < n; i++)
        dst[i] = data[(head + (uint32_t)i) & SHM_RING_MASK];
    shm_ring_observe();                 /* load->store: reads complete before head frees cells */
    h->head = head + (uint32_t)n;
    return n;
}

/* Direct ring write (writer mapped the ring). Copies up to count bytes, yielding on
 * a full ring until the reader drains it (no syscall on full in the common case).
 * Wakes a reader parked in the server via PIPE_RING_WAKE when it observes
 * reader_waiting. Returns bytes written (< count only if the reader closed = EPIPE). */
static long shm_ring_send(int pipe_id, char *ring, const char *src, size_t count) {
    shm_ring_hdr_t *h = (shm_ring_hdr_t *)ring;
    char *data = shm_ring_data(ring);
    size_t sent = 0;
    while (sent < count) {
        if (h->reader_closed) break;            /* EPIPE: reader gone, stop (short write) */
        uint32_t tail = h->tail;                /* writer owns tail */
        uint32_t head = h->head;
        shm_ring_observe();                     /* acquire reader's head */
        uint32_t space = SHM_RING_DATA - (tail - head);
        if (space == 0) {
            /* Full pipe -> block until the reader drains (POSIX write semantics).
             * Never silently drop: a parked reader is SERVED via PIPE_RING_WAKE
             * (which advances head right here), so the writer cannot deadlock
             * holding bytes the parked reader is waiting for; a runnable reader is
             * given the core via Yield. The only exit is reader_closed above. */
            shm_ring_fence();                    /* StoreLoad: see a fresh reader_waiting */
            if (h->reader_waiting) {
                seL4_SetMR(0, (seL4_Word)pipe_id);
                seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_RING_WAKE_L, 0, 0, 1));
            } else {
                seL4_Yield();
            }
            continue;
        }
        uint32_t idx = tail & SHM_RING_MASK;
        uint32_t run = SHM_RING_DATA - idx;      /* contiguous bytes to the buffer end */
        uint32_t chunk = (uint32_t)(count - sent);
        if (chunk > space) chunk = space;
        if (chunk > run) chunk = run;
        for (uint32_t i = 0; i < chunk; i++)
            data[idx + i] = src[sent + i];
        shm_ring_publish();                      /* release: data stores before tail */
        h->tail = tail + chunk;
        sent += chunk;
        shm_ring_fence();                        /* StoreLoad: tail before reader_waiting */
        if (h->reader_waiting) {
            seL4_SetMR(0, (seL4_Word)pipe_id);
            seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_RING_WAKE_L, 0, 0, 1));
        }
    }
    return (long)sent;
}

/* Non-static entry for the stdio backend (aios_stdio_write, aios_posix.c) -- the HOT
 * path for buffered stdout to a pipe (every filter tool's printf/fputs flushes through
 * it). Without this the ring is never used by real pipelines (only raw read()/write()
 * callers like the netconsole relay mapped it). Returns 1 if the ring handled the write
 * (*out = bytes sent; < count only on reader-close EPIPE), 0 if no ring (caller uses its
 * legacy PIPE_WRITE loop). */
int aios_stdout_ring_write(const char *src, size_t count, long *out) {
    char *wr = stdout_ring_get();
    if (!wr) return 0;
    *out = shm_ring_send(stdout_pipe_id, wr, src, count);
    return 1;
}


/* v0.4.79: generate /proc/self/fd directory entries (getdents64 format) */
static int procfd_gen_dirents(char *buf, int bufsz) {
    int out = 0;
    uint64_t ino = 0xFD00;
    uint64_t doff = 0;

    /* Write one dirent record. Returns bytes written or 0 if full. */
    #define PFD_DIRENT(nm, nlen, dt) do { \
        int rl = (19 + (nlen) + 1 + 7) & ~7; \
        if (rl < 24) rl = 24; \
        if (out + rl > bufsz) goto pfd_done; \
        doff += rl; \
        uint64_t _ino = ino++; \
        for(int _j=0;_j<8;_j++) buf[out+_j]=(char)((_ino>>(_j*8))&0xFF); \
        for(int _j=0;_j<8;_j++) buf[out+8+_j]=(char)((doff>>(_j*8))&0xFF); \
        buf[out+16]=(char)(rl&0xFF); buf[out+17]=(char)((rl>>8)&0xFF); \
        buf[out+18]=(char)(dt); \
        for(int _j=0;_j<(nlen);_j++) buf[out+19+_j]=(nm)[_j]; \
        buf[out+19+(nlen)]=0; \
        for(int _j=19+(nlen)+1;_j<rl;_j++) buf[out+_j]=0; \
        out += rl; \
    } while(0)

    PFD_DIRENT(".", 1, 4);   /* DT_DIR */
    PFD_DIRENT("..", 2, 4);
    PFD_DIRENT("0", 1, 10);  /* DT_LNK */
    PFD_DIRENT("1", 1, 10);
    PFD_DIRENT("2", 1, 10);

    for (int _ai = 0; _ai < AIOS_MAX_FDS; _ai++) {
        if (!aios_fds[_ai].active) continue;
        int fdn = AIOS_FD_BASE + _ai;
        char nb[6]; int ni = 0;
        int v = fdn;
        if (v >= 100) { nb[ni++] = (char)('0' + v/100); v %= 100; }
        if (fdn >= 10) { nb[ni++] = (char)('0' + v/10); v %= 10; }
        nb[ni++] = (char)('0' + v);
        PFD_DIRENT(nb, ni, 10);
    }

    #undef PFD_DIRENT
pfd_done:
    return out;
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

    /* v0.4.78: /dev/urandom, /dev/random, /dev/zero */
    {
        const char *devs[] = { "/dev/urandom", "/dev/random", "/dev/zero", 0 };
        for (int di = 0; devs[di]; di++) {
            const char *dp = devs[di];
            int dm = 1, dl = 0;
            while (dp[dl]) { if (pathname[dl] != dp[dl]) dm = 0; dl++; }
            if (dm && pathname[dl] == 0) {
                int idx = aios_fd_alloc();
                if (idx < 0) return -EMFILE;
                aios_fd_t *f = &aios_fds[idx];
                f->active = 1;
                f->is_dir = 0;
                f->is_pipe = 0;
                f->is_devnull = 1;
                f->size = 0;
                f->pos = 0;
                str_copy(f->path, dp, sizeof(f->path));
                return AIOS_FD_BASE + idx;
            }
        }
    }

    /* v0.4.79: /proc/self/fd in aios_sys_open (procfd_open_intercept) */
    {
        char _rp[256];
        resolve_path(pathname, _rp, sizeof(_rp));
        const char *_psf = "/proc/self/fd";
        int _pm = 1;
        for (int _i = 0; _psf[_i]; _i++)
            if (_rp[_i] != _psf[_i]) { _pm = 0; break; }
        if (_pm && _rp[13] == 0) {
            int idx = aios_fd_alloc();
            if (idx < 0) return -EMFILE;
            aios_fd_t *_f = &aios_fds[idx];
            _f->active = 1;
            _f->is_dir = 1;
            _f->is_pipe = 0;
            _f->is_devnull = 0;
            _f->is_nonblock = 0;
            _f->pos = 0;
            str_copy(_f->path, "/proc/self/fd", sizeof(_f->path));
            _f->size = procfd_gen_dirents(_f->data, (int)sizeof(_f->data));
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
        f->pos = 0;
        str_copy(f->path, res_path, sizeof(f->path));
        /* Use stat to get real file size -- fetch_file returns only
         * what fits in IPC MRs (~900 bytes).  Large files (libc.a,
         * ELF binaries) need the true size so the read path can
         * switch to on-demand FS_PREAD for data beyond the buffer. */
        uint32_t st_mode, st_size;
        if (fetch_stat(res_path, &st_mode, &st_size) == 0 && st_size > 0) {
            f->size = (int)st_size;
        } else {
            f->size = n;  /* procfs/dynamic files: stat returns 0, use buffered amount */
        }
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

    /* v0.4.78: /dev/urandom, /dev/random, /dev/zero */
    {
        const char *devs[] = { "/dev/urandom", "/dev/random", "/dev/zero", 0 };
        for (int di = 0; devs[di]; di++) {
            const char *dp = devs[di];
            int dm = 1, dl = 0;
            while (dp[dl]) { if (pathname[dl] != dp[dl]) dm = 0; dl++; }
            if (dm && pathname[dl] == 0) {
                int idx = aios_fd_alloc();
                if (idx < 0) return -EMFILE;
                aios_fd_t *f = &aios_fds[idx];
                f->active = 1;
                f->is_dir = 0;
                f->is_pipe = 0;
                f->is_devnull = 1;
                f->size = 0;
                f->pos = 0;
                str_copy(f->path, dp, sizeof(f->path));
                return AIOS_FD_BASE + idx;
            }
        }
    }

    /* v0.4.79: /proc/self/fd -- synthetic directory of open fds */
    {
        char _rp[256];
        resolve_path(pathname, _rp, sizeof(_rp));
        const char *_psf = "/proc/self/fd";
        int _pm = 1;
        for (int _i = 0; _psf[_i]; _i++)
            if (_rp[_i] != _psf[_i]) { _pm = 0; break; }
        if (_pm && _rp[13] == 0) {
            int idx = aios_fd_alloc();
            if (idx < 0) return -EMFILE;
            aios_fd_t *_f = &aios_fds[idx];
            _f->active = 1;
            _f->is_dir = 1;
            _f->is_pipe = 0;
            _f->is_devnull = 0;
            _f->is_nonblock = 0;
            _f->pos = 0;
            str_copy(_f->path, "/proc/self/fd", sizeof(_f->path));
            _f->size = procfd_gen_dirents(_f->data, (int)sizeof(_f->data));
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
            /* v0.4.274: honor the create result. fs_server replies MR0 = -1 when
             * fs_check_path_write denies the create (e.g. a non-root process in a
             * write-protected dir). Previously the reply was ignored, so open(O_CREAT)
             * there returned a USABLE fd although no file was created (silent-fail
             * follow-up from the identity-privesc audit). Fail the open per POSIX. */
            if ((int)(long)seL4_GetMR(0) != 0)
                return -EACCES;
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
        if (fsize == 0) {
            /* Zero-size from stat: procfs and dynamic files report
             * size=0 because content is generated on read.  Use
             * fetch_file (FS_CAT) which reads the actual content. */
            int n = fetch_file(res_path, f->data, sizeof(f->data));
            f->active = 1;
            f->size = (n > 0) ? n : 0;
            f->pos = 0;
        } else if ((int)fsize <= (int)sizeof(f->data) - 1) {
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
        /* v0.4.296: PTY slave read (this process is on a PTY). Blocking yield-loop
         * like the serial is_tty path: tty_server returns whatever cooked line /
         * raw keys are available (0 if none), and a pending signal breaks out with
         * EINTR (so VINTR-generated SIGINT interrupts a blocked prompt read). */
        if (aios_tty_inst > 0 && ser_ep) {
            char *cbuf = (char *)buf;
            int want = (int)count; if (want > 900) want = 900;
            while (1) {
                seL4_SetMR(0, (seL4_Word)aios_tty_inst);
                seL4_SetMR(1, (seL4_Word)want);
                seL4_MessageInfo_t reply = seL4_Call(ser_ep,
                    seL4_MessageInfo_new(85 /* TTY_PTY_SLAVE_READ */, 0, 0, 2));
                int got = (int)(long)seL4_GetMR(0);
                if (got > 0) {
                    int mr = 1;
                    for (int i = 0; i < got; i++) {
                        if (i > 0 && i % 8 == 0) mr++;
                        cbuf[i] = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
                    }
                    return (long)got;
                }
                if (aios_sig_check() > 0) return -EINTR;
                seL4_Yield();
            }
        }
        if (stdin_pipe_id >= 0 && pipe_ep) {
            /* Read from pipe instead of serial */
            char *cbuf = (char *)buf;
            /* v0.4.258: ring fast path -- read straight from the shared ring with no
             * syscall; only block via the server (below) when the ring is empty. */
            char *rr = stdin_ring_get();
            if (rr) {
                int rwant = (int)count; if (rwant > 4096) rwant = 4096;
                int rgot = shm_ring_recv_fast(rr, cbuf, rwant);
                if (rgot > 0) return (long)rgot;
                if (rgot == 0) return 0;   /* EOF */
                /* rgot == -1: empty + writer live -> block via PIPE_READ below */
            }
            int want = (int)count;
            if (want > 900) want = 900;
            seL4_SetMR(0, (seL4_Word)stdin_pipe_id);
            seL4_SetMR(1, (seL4_Word)want);
            seL4_SetMR(2, 0);  /* v0.4.79: blocking read */
            seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
                seL4_MessageInfo_new(62, 0, 0, 3));
            int got = (int)(long)seL4_GetMR(0);
            if (got == -4) { aios_sig_check(); return -EINTR; }  /* v0.4.87: dispatch + EINTR */
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
            if (aios_sig_check() > 0) return -EINTR;
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
            /* v0.4.258: ring fast path -- direct read, no syscall; on empty fall
             * through to the PIPE_READ_SHM block path (server pulls from the ring). */
            if (!f->ring_tried) { f->ring_vaddr = pipe_map_ring(f->pipe_id, 0); f->ring_tried = 1; }
            if (f->ring_vaddr) {
                int rwant = (int)count; if (rwant > 4096) rwant = 4096;
                int rgot = shm_ring_recv_fast(f->ring_vaddr, cbuf, rwant);
                if (rgot > 0) return (long)rgot;
                if (rgot == 0) return 0;                 /* EOF */
                if (f->is_nonblock) return -EAGAIN;      /* empty + O_NONBLOCK */
                /* else: empty + writer live -> block via PIPE_READ_SHM below */
            }
            /* v0.4.66: try SHM path (up to 4096 bytes per call) */
            if (!f->shm_vaddr) pipe_request_shm(f);
            if (f->shm_vaddr) {
                int want = (int)count;
                if (want > 4096) want = 4096;
                seL4_SetMR(0, (seL4_Word)f->pipe_id);
                seL4_SetMR(1, (seL4_Word)want);
                seL4_SetMR(2, (seL4_Word)(f->is_nonblock ? 1 : 0));
                seL4_Call(pipe_ep,
                    seL4_MessageInfo_new(80 /* PIPE_READ_SHM */, 0, 0, 3));
                int got = (int)(long)seL4_GetMR(0);
                if (got == -1) return -EAGAIN;  /* v0.4.79: nonblocking */
                /* v0.4.85: -2 means xfer_valid not ready, fall to MR path */
                if (got == -2) {
                    /* fall through to MR-based PIPE_READ below */
                } else {
                    for (int i = 0; i < got; i++)
                        cbuf[i] = f->shm_vaddr[i];
                    return (long)got;
                }
            }
            /* Fallback: MR-based read */
            int want = (int)count;
            if (want > 900) want = 900;
            seL4_SetMR(0, (seL4_Word)f->pipe_id);
            seL4_SetMR(1, (seL4_Word)want);
            seL4_SetMR(2, (seL4_Word)(f->is_nonblock ? 1 : 0));
            seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
                seL4_MessageInfo_new(62, 0, 0, 3));
            int got = (int)(long)seL4_GetMR(0);
            if (got == -4) { aios_sig_check(); return -EINTR; }  /* v0.4.87: dispatch + EINTR */
            if (got == -1) return -EAGAIN;  /* v0.4.79: nonblocking */
            int mr = 1;
            for (int i = 0; i < got; i++) {
                if (i % 8 == 0 && i > 0) mr++;
                cbuf[i] = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
            }
            return (long)got;
        }
        /* v0.4.81: socket read via net_server RECVFROM */
        if (f->is_socket && net_ep) {
            char *cbuf = (char *)buf;
            int want = (int)count;
            if (want > 900) want = 900;
            seL4_SetMR(0, (seL4_Word)f->socket_id);
            seL4_SetMR(1, (seL4_Word)want);
            seL4_SetMR(2, (seL4_Word)(f->is_nonblock ? 1 : 0));  /* v0.4.84: pass O_NONBLOCK */
            seL4_Call(net_ep, seL4_MessageInfo_new(NET_RECVFROM_L, 0, 0, 3));
            int got = (int)(long)seL4_GetMR(0);
            if (got <= 0) return got;
            int mr = 3;
            seL4_Word w = 0;
            for (int i = 0; i < got; i++) {
                if (i % 8 == 0) w = seL4_GetMR(mr++);
                cbuf[i] = (char)(w & 0xFF);
                w >>= 8;
            }
            return (long)got;
        }
        /* v0.4.78: virtual device read (/dev/urandom, /dev/zero) */
        /* v0.4.99: is_tty blocking read from serial (zsh SHTTY) */
        if (f->is_tty) {
            if (!ser_ep) return -EIO;
            int want = (int)count;
            if (want > 200) want = 200;
            int pty = (f->tty_inst > 0);   /* v0.4.296: duped tty fd on a PTY */
            /* Block until at least 1 byte available (like real tty) */
            while (1) {
                seL4_MessageInfo_t reply;
                if (pty) {
                    seL4_SetMR(0, (seL4_Word)f->tty_inst);
                    seL4_SetMR(1, (seL4_Word)want);
                    reply = seL4_Call(ser_ep,
                        seL4_MessageInfo_new(85 /* TTY_PTY_SLAVE_READ */, 0, 0, 2));
                } else {
                    seL4_SetMR(0, (seL4_Word)want);
                    reply = seL4_Call(ser_ep,
                        seL4_MessageInfo_new(71 /* TTY_READ */, 0, 0, 1));
                }
                int got = (int)seL4_GetMR(0);
                if (got > 0) {
                    char *dst = (char *)buf;
                    int mr = 1;
                    for (int i = 0; i < got; i++) {
                        if (i > 0 && i % 8 == 0) mr++;
                        dst[i] = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
                    }
                    return (long)got;
                }
                /* No data yet -- yield and retry */
                seL4_Yield();
            }
        }
                if (f->is_devnull && f->path[0] == '/') {
            char *dst = (char *)buf;
            int n = (int)count;
            if (n > 4096) n = 4096;
            if (f->path[5] == 'u' || f->path[5] == 'r') {
                /* /dev/urandom or /dev/random -- crypto_server CSPRNG */
                if (crypto_ep) {
                    int got = 0;
                    while (got < n) {
                        int want = n - got;
                        if (want > (int)(seL4_MsgMaxLength * sizeof(seL4_Word)))
                            want = (int)(seL4_MsgMaxLength * sizeof(seL4_Word));
                        seL4_SetMR(0, 1);  /* CRYPTO_OP_RANDOM */
                        seL4_SetMR(1, (seL4_Word)want);
                        seL4_MessageInfo_t reply = seL4_Call(crypto_ep,
                            seL4_MessageInfo_new(0, 0, 0, 2));
                        int nw = (int)seL4_MessageInfo_get_length(reply);
                        for (int wi = 0; wi < nw && got < n; wi++) {
                            seL4_Word w = seL4_GetMR(wi);
                            int rem = n - got;
                            int chunk = (int)sizeof(seL4_Word);
                            if (chunk > rem) chunk = rem;
                            __builtin_memcpy(dst + got, &w, chunk);
                            got += chunk;
                        }
                    }
                    return (long)n;
                }
                /* Fallback: splitmix64 if crypto_server not available */
                uint64_t state;
                __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(state));
                state += (uint64_t)(uintptr_t)buf;
                for (int i = 0; i < n; i++) {
                    state += 0x9E3779B97F4A7C15ULL;
                    uint64_t z = state;
                    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
                    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
                    z = z ^ (z >> 31);
                    dst[i] = (char)(z & 0xFF);
                }
                return (long)n;
            }
            if (f->path[5] == 'z') {
                /* /dev/zero -- return zero bytes */
                for (int i = 0; i < n; i++) dst[i] = 0;
                return (long)n;
            }
            return 0;  /* /dev/null -- EOF */
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
                /* v0.4.298: bulk fast path for a large `cmd > file` redirect. */
                if (!rf->is_append) {
                    while (total < count) {
                        long br = aios_bulk_pwrite(rf->path, rf->pos, src + total, count - total);
                        if (br <= 0) break;
                        total += (size_t)br;
                        rf->pos += (int)br;
                        if (rf->pos > rf->size) rf->size = rf->pos;
                    }
                }
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
            /* v0.4.258: ring fast path -- write straight into the shared ring (no
             * per-chunk Call); the reader drains it on its own core. */
            char *wr = stdout_ring_get();
            /* shm_ring_send returns < count only when the reader closed (EPIPE);
             * a live reader is always drained to completion. Return its count so a
             * short write surfaces instead of looping forever on a dead reader. */
            if (wr) return shm_ring_send(stdout_pipe_id, wr, src, count);
            size_t sent = 0;
            int stall = 0;                 /* bounded backpressure retry (never hang) */
            stdout_request_wshm();         /* v0.4.257: map the 4KB write xfer page once */
            while (sent < count) {
                int chunk = (int)(count - sent);
                /* v0.4.257 coalescing fast path: up to 4KB per Call via the write xfer page,
                 * vs 900B MR-packed PIPE_WRITE. The reply is the accepted count (< chunk if
                 * the ring is filling) -- advance by it and re-loop. */
                if (stdout_wshm) {
                    int c = chunk > 4096 ? 4096 : chunk;
                    for (int i = 0; i < c; i++) stdout_wshm[i] = src[sent + i];
                    seL4_SetMR(0, (seL4_Word)stdout_pipe_id);
                    seL4_SetMR(1, (seL4_Word)c);
                    seL4_Call(pipe_ep, seL4_MessageInfo_new(79 /* PIPE_WRITE_SHM */, 0, 0, 2));
                    int w = (int)(long)seL4_GetMR(0);
                    if (w > 0) { sent += w; stall = 0; continue; }
                    if (w == 0) { if (++stall > 100000) break; seL4_Yield(); continue; }
                    stdout_wshm = (void *)0;   /* w<0: SHM unusable -> MR fallback below */
                }
                if (chunk > 900) chunk = 900;  /* MR limit (960B IPC ceiling) */
                seL4_SetMR(0, (seL4_Word)stdout_pipe_id);
                seL4_SetMR(1, (seL4_Word)chunk);
                int mr = 2;
                seL4_Word w = 0;
                for (int i = 0; i < chunk; i++) {
                    w |= ((seL4_Word)(uint8_t)src[sent + i]) << ((i % 8) * 8);
                    if (i % 8 == 7 || i == chunk - 1) { seL4_SetMR(mr++, w); w = 0; }
                }
                seL4_Call(pipe_ep, seL4_MessageInfo_new(61, 0, 0, mr));
                int mw = (int)(long)seL4_GetMR(0);
                if (mw > 0) { sent += mw; stall = 0; }
                else { if (++stall > 100000) break; seL4_Yield(); }
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
        /* v0.4.99: is_tty route to serial (duped tty fds from zsh SHTTY) */
        if (f->is_tty) {
            return (long)aios_stdio_write((void *)buf, count);
        }
        /* v0.4.81: socket write via net_server SENDTO */
        if (f->is_socket && net_ep) {
            const char *src = (const char *)buf;
            size_t sent = 0;
            while (sent < count) {
                int chunk = (int)(count - sent);
                if (chunk > 900) chunk = 900;
                seL4_SetMR(0, (seL4_Word)f->socket_id);
                seL4_SetMR(1, (seL4_Word)chunk);
                seL4_SetMR(2, 0);
                seL4_SetMR(3, 0);
                int mr = 4;
                seL4_Word w = 0;
                for (int i = 0; i < chunk; i++) {
                    w |= ((seL4_Word)(uint8_t)src[sent + i]) << ((i % 8) * 8);
                    if (i % 8 == 7 || i == chunk - 1) { seL4_SetMR(mr++, w); w = 0; }
                }
                seL4_Call(net_ep, seL4_MessageInfo_new(NET_SENDTO_L, 0, 0, mr));
                int rc = (int)(long)seL4_GetMR(0);
                if (rc <= 0) { if (sent == 0) return -EIO; break; }
                sent += rc;
            }
            return (long)sent;
        }
        /* Regular file write -- use FS_APPEND (19) for O_APPEND fds,
         * FS_WRITE_FILE (15) otherwise. v0.4.66 */
        if (!f->is_pipe && !f->is_dir && f->path[0] && fs_ep_cap) {
            /* v0.4.70: Write directly to disk via FS_PWRITE */
            const char *src = (const char *)buf;
            size_t total = 0;
            /* v0.4.298: bulk fast path -- page-bounce the source in pipe_server
             * (one IPC per 64KB vs ceil(count/800) FS_PWRITE round-trips). O_APPEND
             * stays on the legacy loop (its per-chunk f->size offset bookkeeping). */
            if (!f->is_append) {
                while (total < count) {
                    long br = aios_bulk_pwrite(f->path, f->pos, src + total, count - total);
                    if (br <= 0) break;
                    total += (size_t)br;
                    f->pos += (int)br;
                    if (f->pos > f->size) f->size = f->pos;
                }
            }
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
            /* v0.4.258: ring fast path -- direct write, no per-chunk Call. */
            if (!f->ring_tried) { f->ring_vaddr = pipe_map_ring(f->pipe_id, 1); f->ring_tried = 1; }
            /* < count only on reader-close (EPIPE); a live reader drains fully. */
            if (f->ring_vaddr) return shm_ring_send(f->pipe_id, f->ring_vaddr, src, count);
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
        /* v0.4.87: always notify pipe_server when closing a pipe
         * write end. Server tracks write_refs authoritatively.
         * Previous last_write_ref heuristic was unreliable (missed
         * closes from sshd relay, leaving phantom write refs). */
        if (f->is_pipe && !f->pipe_read && pipe_ep) {
            seL4_SetMR(0, (seL4_Word)f->pipe_id);
            seL4_Call(pipe_ep,
                seL4_MessageInfo_new(70 /* PIPE_CLOSE_WRITE */, 0, 0, 1));
        }
        /* Socket close: notify server to free slot */
        if (f->is_socket && net_ep) {
            seL4_SetMR(0, (seL4_Word)f->socket_id);
            seL4_Call(net_ep,
                seL4_MessageInfo_new(NET_CLOSE_SOCK_L, 0, 0, 1));
        }
        f->active = 0;
        f->is_pipe = 0;
        f->is_socket = 0;
        f->is_devnull = 0;
        f->pipe_id = -1;
        f->socket_id = -1;
        f->shm_vaddr = 0;
        f->ring_vaddr = 0;     /* v0.4.258 */
        f->ring_tried = 0;
        return 0;
    }
    /* Reset stdin/stdout pipe redirect on close */
    if (fd == 0) { stdin_pipe_id = -1; return 0; }
    if (fd == 1) { stdout_pipe_id = -1; return 0; }
    /* v0.4.165: closing fd 2 (stderr) must NOT reset stdout_pipe_id. fd 1 and
     * fd 2 share the single stdout_pipe_id (a parent like netconsole dup2's
     * both to one pipe), so resetting it on close(2) also broke fd 1 routing:
     * a shell `2>&1` redirect (dash's savefd closes fd 2 before re-duping) sent
     * all subsequent stdout to the serial console instead of the pipe, so the
     * reader saw no data and no EOF and hung. Leaving stderr routed to the same
     * pipe is exactly what 2>&1 wants anyway. */
    if (fd == 2) { return 0; }
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

    /* writev: pipe fd, tty fd, and regular aios fd support */
    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        if (!f->active) return -EBADF;
        /* v0.4.99: is_tty writev routes to serial (zsh shout FILE*) */
        if (f->is_tty) {
            long total = 0;
            for (int i = 0; i < iovcnt; i++) {
                total += (long)aios_stdio_write(iov[i].iov_base, iov[i].iov_len);
            }
            return total;
        }
        if (f->is_devnull) {
            long total = 0;
            for (int i = 0; i < iovcnt; i++) total += (long)iov[i].iov_len;
            return total;
        }
        if (f->is_pipe && !f->pipe_read && pipe_ep) {
            /* v0.4.258: SHM-ring fast path for the writev pipe end (writev/readv are
             * how stdio flushes/fills, so this is on the real filter-pipeline path). */
            if (!f->ring_tried) { f->ring_vaddr = pipe_map_ring(f->pipe_id, 1); f->ring_tried = 1; }
            long total = 0;
            for (int i = 0; i < iovcnt; i++) {
                const char *src = (const char *)iov[i].iov_base;
                if (f->ring_vaddr) {
                    long s = shm_ring_send(f->pipe_id, f->ring_vaddr, src, iov[i].iov_len);
                    total += s;
                    if (s < (long)iov[i].iov_len) break;   /* reader closed -> EPIPE */
                    continue;
                }
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
                /* v0.4.298: bulk fast path -- musl flushes a large fwrite via
                 * writev (iov[1] is the user buffer), so this is THE stdio big-write
                 * path the tcc linker output rides. O_APPEND stays on the legacy loop. */
                if (!f->is_append) {
                    while (iov_sent < iov[i].iov_len) {
                        long br = aios_bulk_pwrite(f->path, f->pos, src + iov_sent,
                                                   iov[i].iov_len - iov_sent);
                        if (br <= 0) break;
                        iov_sent += (size_t)br;
                        f->pos += (int)br;
                        if (f->pos > f->size) f->size = f->pos;
                    }
                }
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
        /* v0.4.258: a 0-length iov contributes 0 bytes and is NOT EOF -- musl's
         * __stdio_read passes iov[0].iov_len = len - !!buf_size, so a BUFFERED
         * FILE always presents a 0-length first iov (the real read targets iov[1]).
         * Skip it: the SHM-ring recv_fast returns 0 for want==0, which the ring
         * branches below would otherwise mistake for EOF -> premature end-of-stream. */
        if (iov[i].iov_len == 0) continue;
        if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
            aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
            if (!f->active) return -EBADF;
            /* readv: pipe read via IPC */
            if (f->is_pipe && f->pipe_read && pipe_ep) {
                char *dst = (char *)iov[i].iov_base;
                /* v0.4.258: SHM-ring fast path (readv is how stdio fills its buffer). */
                if (!f->ring_tried) { f->ring_vaddr = pipe_map_ring(f->pipe_id, 0); f->ring_tried = 1; }
                if (f->ring_vaddr) {
                    int rwant = (int)iov[i].iov_len; if (rwant > 4096) rwant = 4096;
                    int rgot = shm_ring_recv_fast(f->ring_vaddr, dst, rwant);
                    if (rgot > 0) { total += rgot; if (rgot < (int)iov[i].iov_len) break; continue; }
                    if (rgot == 0) break;   /* EOF */
                }
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
            /* v0.4.296: PTY slave read (blocking yield-loop, like the read() path). */
            if (aios_tty_inst > 0 && ser_ep) {
                char *dst = (char *)iov[i].iov_base;
                int want = (int)iov[i].iov_len; if (want > 900) want = 900;
                int got = 0;
                while (1) {
                    seL4_SetMR(0, (seL4_Word)aios_tty_inst);
                    seL4_SetMR(1, (seL4_Word)want);
                    seL4_Call(ser_ep, seL4_MessageInfo_new(85 /* TTY_PTY_SLAVE_READ */, 0, 0, 2));
                    got = (int)(long)seL4_GetMR(0);
                    if (got > 0) break;
                    if (aios_sig_check() > 0) return total > 0 ? total : -EINTR;
                    seL4_Yield();
                }
                int mr = 1;
                for (int k = 0; k < got; k++) {
                    if (k % 8 == 0 && k > 0) mr++;
                    dst[k] = (char)((seL4_GetMR(mr) >> ((k % 8) * 8)) & 0xFF);
                }
                total += got;
                if (got < (int)iov[i].iov_len) break;
                continue;
            }
            /* stdin — check pipe redirect */
            if (stdin_pipe_id >= 0 && pipe_ep) {
                char *dst = (char *)iov[i].iov_base;
                /* v0.4.258: SHM-ring fast path (the wc end of seq|wc reads via readv). */
                char *rr = stdin_ring_get();
                if (rr) {
                    int rwant = (int)iov[i].iov_len; if (rwant > 4096) rwant = 4096;
                    int rgot = shm_ring_recv_fast(rr, dst, rwant);
                    if (rgot > 0) { total += rgot; if (rgot < (int)iov[i].iov_len) break; continue; }
                    if (rgot == 0) break;   /* EOF */
                }
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

/* v0.4.130: real ftruncate via FS_TRUNCATE IPC. Resolves fd to a path
 * (we keep the path on the fd record) and sends to fs_server, which
 * updates the inode's i_size. Shrinks leak the freed blocks for now;
 * grows do not zero-fill the gap (caller must pwrite if it cares). */
long aios_sys_ftruncate(va_list ap) {
    int fd = va_arg(ap, int);
    long length = va_arg(ap, long);
    if (length < 0) return -EINVAL;
    if (fd < AIOS_FD_BASE || fd >= AIOS_FD_BASE + AIOS_MAX_FDS) return -EBADF;
    aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
    if (!f->active) return -EBADF;
    if (f->is_pipe || f->is_dir) return -EINVAL;
    if (!f->path[0] || !fs_ep_cap) return -ENOSYS;
    int plen = 0; while (f->path[plen]) plen++;
    seL4_SetMR(0, (seL4_Word)plen);
    seL4_SetMR(1, (seL4_Word)length);
    int mr = 2;
    seL4_Word w = 0;
    for (int i = 0; i < plen; i++) {
        w |= ((seL4_Word)(uint8_t)f->path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == plen - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_Call(fs_ep_cap, seL4_MessageInfo_new(21 /* FS_TRUNCATE */, 0, 0, mr));
    long rc = (long)seL4_GetMR(0);
    if (rc == 0) {
        /* Update cached size so subsequent fstat sees the new value. */
        f->size = (int)length;
    }
    return rc;
}


/* v0.4.188: fsync/fdatasync/sync -- flush the write-back block cache to disk.
 *
 * The block cache (drive 0) is write-back, so without an explicit flush a
 * power cut can lose recently-written data that has not yet hit the threshold
 * or been evicted. These route to FS_SYNC, which runs blk_cache_flush() on the
 * fs_thread (the only safe place -- the backend DMA ring is unlocked). The
 * flush is whole-cache (the cache keys on block numbers, not inodes), which is
 * a valid superset for a per-fd fsync; fdatasync maps to the same flush. */
static long do_fs_sync(void) {
    if (!fs_ep_cap) return 0;   /* no disk -> nothing to sync */
    seL4_Call(fs_ep_cap, seL4_MessageInfo_new(22 /* FS_SYNC */, 0, 0, 0));
    return (long)seL4_GetMR(0);
}

long aios_sys_sync(va_list ap) {
    (void)ap;
    do_fs_sync();
    return 0;   /* sync(2) returns void; the syscall returns 0 */
}

long aios_sys_fsync(va_list ap) {
    int fd = va_arg(ap, int);
    /* Standard fds (0/1/2) and any in-range aios fd: only real disk-backed
     * files need a flush; pipes, sockets, ttys and /dev are no-op success. */
    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        if (!f->active) return -EBADF;
        if (f->is_pipe || f->is_socket || f->is_tty || f->is_devnull) return 0;
        return do_fs_sync();
    }
    if (fd >= 0 && fd < AIOS_FD_BASE) return 0;   /* stdio etc. */
    return -EBADF;
}

long aios_sys_fdatasync(va_list ap) {
    return aios_sys_fsync(ap);   /* same whole-cache flush */
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
        if (offset < 0) return -EINVAL;
        /* v0.4.178: read through fetch_pread (chunked FS_PREAD) so this works
         * for files larger than the in-memory f->data buffer -- the old
         * version copied straight out of f->data, which is only valid for
         * small (<=4 KB) cached files. Does NOT change f->pos. */
        if (!f->is_dir && f->path[0] && fs_ep_cap) {
            char *dst = (char *)buf;
            size_t total = 0;
            while (total < count) {
                int chunk = (int)(count - total);
                if (chunk > 900) chunk = 900;
                int got = fetch_pread(f->path, (int)(offset + total),
                                      dst + total, chunk);
                if (got <= 0) break;
                total += got;
                if (got < chunk) break;   /* short read = EOF */
            }
            return (long)total;
        }
        return 0;
    }
    return -EBADF;
}

/* pwrite64 -- positional write; does not change the file offset.
 * v0.4.178: rewritten to honor `offset` and chunk through fetch_pwrite (the
 * same FS_PWRITE path write() uses). The old version capped at 3000 bytes and
 * packed them all into MRs -> seL4_MessageInfo_new(>127) asserts and HALTS the
 * system on any pwrite over ~1 KB; it also ignored the offset entirely. SFTP
 * uploads exposed both. */
long aios_sys_pwrite64(va_list ap) {
    int fd = va_arg(ap, int);
    const void *buf = va_arg(ap, const void *);
    size_t count = va_arg(ap, size_t);
    off_t offset = va_arg(ap, off_t);

    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        if (!f->active) return -EBADF;
        if (f->is_pipe) return -ESPIPE;
        if (offset < 0) return -EINVAL;
        if (!f->is_dir && f->path[0] && fs_ep_cap) {
            const char *s = (const char *)buf;
            size_t total = 0;
            /* v0.4.298: bulk fast path -- explicit offset, no append ambiguity. */
            while (total < count) {
                long br = aios_bulk_pwrite(f->path, (int)(offset + (off_t)total),
                                           s + total, count - total);
                if (br <= 0) break;
                total += (size_t)br;
            }
            while (total < count) {
                int chunk = (int)(count - total);
                if (chunk > 800) chunk = 800;  /* keep MR count under the 127 cap */
                int wrote = fetch_pwrite(f->path, (int)(offset + total),
                                         s + total, chunk);
                if (wrote <= 0) break;
                total += wrote;
            }
            if ((int)(offset + total) > f->size) f->size = (int)(offset + total);
            return (long)total;
        }
        return (long)count;
    }
    return -EBADF;
}
