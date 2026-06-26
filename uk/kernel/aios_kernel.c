/*
 * aios_kernel.c -- the AIOS userspace kernel. Host-agnostic core.
 *
 * Dispatches the AIOS ABI for guest programs using ONLY the PAL (pal.h) -- no host headers, no
 * host syscalls. This exact code is meant to run unchanged over the Linux PAL (ptrace) today and
 * a future seL4 PAL (IPC) tomorrow (docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md).
 *
 * M1: WRITE + EXIT (first light).
 * M2: a VFS behind the ABI -- the kernel owns the AIOS fd namespace and services OPEN/READ/WRITE/
 *     CLOSE/LSEEK/FSTAT through opaque PAL backing objects.
 * M3d: the PROCESS MODEL. The kernel is now MULTI-PROCESS: a process table, a waitpid-style event
 *     loop over all guests (pal_guest_next), per-process fd tables over a refcounted open-file
 *     table (so fds shared across fork keep correct close semantics), and FORK / WAIT / EXEC /
 *     EXIT. This is what a shell needs.
 */
#include "aios_abi.h"
#include "aios_version.h"      /* host-agnostic version macros (the 0.5.x userspace-kernel line) */
#include "pal.h"
#include <stddef.h>
#include <stdint.h>

/* ---- kernel-internal diagnostics (its OWN stderr, separate from the guest fd tables) ---- */
static size_t kstrlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void kputs(const char *s) { pal_host_write(pal_host_std(AIOS_FD_STDERR), s, kstrlen(s)); }
static void kput_int(long v) {
    char b[24]; int i = sizeof b; int neg = v < 0;
    unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    b[--i] = '\0';
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    pal_host_write(pal_host_std(AIOS_FD_STDERR), &b[i], (size_t)(sizeof b - 1 - i));
}

/* ---- the open-file table: the kernel's "open file description" layer ----
 * A backing object (opaque pal_file_t) plus a refcount. Per-process fd tables hold indices into
 * this table; fork copies the indices and bumps refcounts, so parent and child SHARE the same
 * backing (POSIX: shared file offset) and a close in one does not pull it out from under the other
 * -- the backing is released (pal_host_close) only when the last referencing fd closes. The three
 * std streams are seeded permanent (they back the kernel's own stdio and are never closed). */
#define MAX_OFILES 256
typedef struct {
    pal_file_t backing;
    int        refcount;
    int        permanent;   /* std streams: never pal_host_close */
    int        is_pipe;     /* this backing is a pipe end */
    int        pipe_id;     /* which pipe (the read + write ends share it) */
    int        pipe_write;  /* 1 = write end, 0 = read end */
} ofile_t;
static ofile_t g_ofile[MAX_OFILES];
static int     g_next_pipe_id = 1;

static void ofile_init_std(void) {
    for (int i = 0; i < 3; i++) {
        g_ofile[i].backing = pal_host_std(i);
        g_ofile[i].refcount = 1;
        g_ofile[i].permanent = 1;
    }
}
static int ofile_alloc(pal_file_t backing) {
    for (int i = 0; i < MAX_OFILES; i++)
        if (!g_ofile[i].permanent && g_ofile[i].refcount == 0) {
            g_ofile[i] = (ofile_t){ .backing = backing, .refcount = 1 };  /* clears pipe tags */
            return i;
        }
    return -1;
}
static void ofile_ref(int oi)   { if (oi >= 0 && !g_ofile[oi].permanent) g_ofile[oi].refcount++; }
static void ofile_unref(int oi) {
    if (oi < 0 || g_ofile[oi].permanent) return;
    if (--g_ofile[oi].refcount == 0) pal_host_close(g_ofile[oi].backing);
}

/* ---- the process table ---- */
#define AIOS_MAX_FD 64
#define MAX_PROCS   64
enum { PS_FREE = 0, PS_RUNNING, PS_ZOMBIE, PS_BLOCKED_WAIT, PS_BLOCKED_READ, PS_BLOCKED_WRITE };
typedef struct {
    int           state;
    pal_pid_t     pid;
    pal_pid_t     parent_pid;          /* PAL_PID_NONE for init / orphans */
    int           exit_code;           /* valid in PS_ZOMBIE */
    unsigned long wait_for;            /* PS_BLOCKED_WAIT: AIOS_WAIT_ANY/0, or a specific pid */
    uint64_t      wait_status_gaddr;   /* PS_BLOCKED_WAIT: where to store the status (0 = none) */
    /* PS_BLOCKED_READ/WRITE: a pipe I/O parked because the pipe was empty/full. The kernel resumes
     * it from pipe_settle() when a peer makes the pipe ready (or closes its end -> EOF/EPIPE). */
    int           blk_fd;              /* the AIOS fd being read/written */
    uint64_t      blk_buf;             /* guest buffer */
    uint64_t      blk_len;             /* total bytes requested */
    uint64_t      blk_done;            /* bytes already transferred (writes) */
    int           blk_pipe;            /* pipe_id this I/O is parked on */
    int           fd[AIOS_MAX_FD];     /* AIOS fd -> ofile index, or -1 */
} proc_t;
static proc_t g_proc[MAX_PROCS];

static proc_t *proc_find(pal_pid_t pid) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (g_proc[i].state != PS_FREE && g_proc[i].pid == pid) return &g_proc[i];
    return NULL;
}
static proc_t *proc_alloc(void) {
    for (int i = 0; i < MAX_PROCS; i++) if (g_proc[i].state == PS_FREE) return &g_proc[i];
    return NULL;
}

/* ---- per-process fd table ---- */
static void fd_table_init_std(proc_t *p) {
    for (int i = 0; i < AIOS_MAX_FD; i++) p->fd[i] = -1;
    p->fd[AIOS_FD_STDIN]  = 0;          /* ofile 0/1/2 = the permanent std streams */
    p->fd[AIOS_FD_STDOUT] = 1;
    p->fd[AIOS_FD_STDERR] = 2;
}
static int fd_alloc(proc_t *p) {
    for (int i = 0; i < AIOS_MAX_FD; i++) if (p->fd[i] < 0) return i;
    return -1;
}
static int        fd_valid(proc_t *p, uint64_t fd)   { return fd < AIOS_MAX_FD && p->fd[fd] >= 0; }
static pal_file_t fd_backing(proc_t *p, uint64_t fd) { return g_ofile[p->fd[fd]].backing; }

static void pipe_settle(int pipe_id);   /* defined below; wakes guests parked on a pipe */

/* Drop one fd's reference to its open-file object; if that closed the last ref to a pipe end, wake
 * the pipe's peers (a reader now sees EOF, a writer a broken pipe). Used by close, dup2, and -- so
 * a pipe write end held by a dying writer actually closes -- by process exit. */
static void fd_release(proc_t *p, int fd) {
    int oi = p->fd[fd];
    if (oi < 0) return;
    int was_pipe = g_ofile[oi].is_pipe;
    int pipe_id  = g_ofile[oi].pipe_id;
    int last     = !g_ofile[oi].permanent && g_ofile[oi].refcount == 1;
    ofile_unref(oi);
    p->fd[fd] = -1;
    if (was_pipe && last) pipe_settle(pipe_id);
}

/* ---- AIOS file syscalls (host-agnostic; reach the host only via the PAL) ---- */

static long sys_open(proc_t *p, uint64_t gpath, uint64_t flags, uint64_t mode) {
    char path[256];
    size_t n = pal_guest_read(p->pid, gpath, path, sizeof path - 1);
    if (n == 0) return -AIOS_EFAULT;
    path[n] = '\0';                                  /* backstop terminator */
    pal_file_t f = pal_host_open(path, flags, mode);
    if (f < 0) return (long)f;                        /* -errno from the PAL (ENOENT, EACCES, ...) */
    int oi = ofile_alloc(f);
    if (oi < 0) { pal_host_close(f); return -AIOS_EMFILE; }   /* open-file table full */
    int fd = fd_alloc(p);
    if (fd < 0) { ofile_unref(oi); return -AIOS_EMFILE; }     /* fd table full (unref closes f) */
    p->fd[fd] = oi;
    return fd;
}

static long sys_close(proc_t *p, uint64_t fd) {
    if (!fd_valid(p, fd)) return -AIOS_EBADF;
    fd_release(p, (int)fd);                            /* unref + wake pipe peers if last ref */
    return 0;
}

static long sys_dup2(proc_t *p, uint64_t oldfd, uint64_t newfd) {
    if (!fd_valid(p, oldfd) || newfd >= AIOS_MAX_FD) return -AIOS_EBADF;
    if (oldfd == newfd) return (long)newfd;
    if (p->fd[newfd] >= 0) fd_release(p, (int)newfd);  /* close whatever newfd referred to */
    p->fd[newfd] = p->fd[oldfd];                       /* alias the same open-file object */
    ofile_ref(p->fd[newfd]);
    return (long)newfd;
}

static long sys_lseek(proc_t *p, uint64_t fd, uint64_t off, uint64_t whence) {
    if (!fd_valid(p, fd)) return -AIOS_EBADF;
    return (long)pal_host_lseek(fd_backing(p, fd), (long long)off, (int)whence);  /* -errno on fail */
}

static long sys_fstat(proc_t *p, uint64_t fd, uint64_t gstat) {
    if (!fd_valid(p, fd)) return -AIOS_EBADF;
    struct aios_stat s;
    int r = pal_host_fstat(fd_backing(p, fd), &s);
    if (r != 0) return r;                              /* -errno */
    if (pal_guest_write(p->pid, gstat, &s, sizeof s) != sizeof s) return -AIOS_EFAULT;
    return 0;
}

/* Read directory entries: ask the PAL to fill a kernel buffer with aios_dirent records, then bounce
 * them into the guest. fd must be a directory backing object (opened via OPEN). */
static long sys_getdents(proc_t *p, uint64_t fd, uint64_t gbuf, uint64_t len) {
    if (!fd_valid(p, fd)) return -AIOS_EBADF;
    char buf[4096];
    size_t cap = len < sizeof buf ? (size_t)len : sizeof buf;
    long n = pal_host_getdents(fd_backing(p, fd), buf, cap);
    if (n <= 0) return n;                              /* 0 = end of directory, -errno = error */
    if (pal_guest_write(p->pid, gbuf, buf, (size_t)n) != (size_t)n) return -AIOS_EFAULT;
    return n;
}

/* Bounce a path string out of the guest into `dst` (NUL-terminated). 0 on success, -errno. */
static long read_path(proc_t *p, uint64_t gpath, char *dst, size_t cap) {
    size_t n = pal_guest_read(p->pid, gpath, dst, cap - 1);
    if (n == 0) return -AIOS_EFAULT;
    dst[n] = '\0';
    return 0;
}

static long sys_stat(proc_t *p, uint64_t gpath, uint64_t gstat, int follow) {
    char path[256];
    long e = read_path(p, gpath, path, sizeof path); if (e) return e;
    struct aios_stat s;
    int r = pal_host_stat(path, &s, follow);
    if (r != 0) return r;
    if (pal_guest_write(p->pid, gstat, &s, sizeof s) != sizeof s) return -AIOS_EFAULT;
    return 0;
}

static long sys_getcwd(proc_t *p, uint64_t gbuf, uint64_t size) {
    char buf[1024];
    size_t cap = size < sizeof buf ? (size_t)size : sizeof buf;
    long r = pal_host_getcwd(buf, cap);
    if (r < 0) return r;
    if (pal_guest_write(p->pid, gbuf, buf, (size_t)r + 1) != (size_t)r + 1) return -AIOS_EFAULT; /* +NUL */
    return r;
}

static long sys_chdir (proc_t *p, uint64_t gpath) {
    char path[256]; long e = read_path(p, gpath, path, sizeof path); if (e) return e;
    return pal_host_chdir(path);
}
static long sys_unlink(proc_t *p, uint64_t gpath) {
    char path[256]; long e = read_path(p, gpath, path, sizeof path); if (e) return e;
    return pal_host_unlink(path);
}
static long sys_mkdir (proc_t *p, uint64_t gpath, uint64_t mode) {
    char path[256]; long e = read_path(p, gpath, path, sizeof path); if (e) return e;
    return pal_host_mkdir(path, (unsigned int)mode);
}
static long sys_rmdir (proc_t *p, uint64_t gpath) {
    char path[256]; long e = read_path(p, gpath, path, sizeof path); if (e) return e;
    return pal_host_rmdir(path);
}
static long sys_rename(proc_t *p, uint64_t gold, uint64_t gnew) {
    char o[256], n[256];
    long e = read_path(p, gold, o, sizeof o); if (e) return e;
    e = read_path(p, gnew, n, sizeof n);      if (e) return e;
    return pal_host_rename(o, n);
}

/* ---- the *at family (resolve a path relative to a dir fd, or AT_FDCWD) ---- */

/* Resolve a guest dirfd to a PAL directory handle: AIOS_AT_FDCWD -> PAL_AT_FDCWD ("relative to the
 * cwd"), otherwise the fd's backing object. 0, or -AIOS_EBADF for a bad fd. */
static long resolve_dir(proc_t *p, uint64_t dirfd, pal_file_t *out) {
    if ((int)dirfd == AIOS_AT_FDCWD) { *out = PAL_AT_FDCWD; return 0; }
    if (!fd_valid(p, dirfd)) return -AIOS_EBADF;
    *out = fd_backing(p, dirfd);
    return 0;
}

static long sys_openat(proc_t *p, uint64_t dirfd, uint64_t gpath, uint64_t flags, uint64_t mode) {
    char path[256];
    long e = read_path(p, gpath, path, sizeof path); if (e) return e;
    pal_file_t dir; e = resolve_dir(p, dirfd, &dir);  if (e) return e;
    pal_file_t f = pal_host_openat(dir, path, flags, mode);
    if (f < 0) return (long)f;                          /* -errno (ENOENT, ENOTDIR, ...) */
    int oi = ofile_alloc(f);
    if (oi < 0) { pal_host_close(f); return -AIOS_EMFILE; }
    int fd = fd_alloc(p);
    if (fd < 0) { ofile_unref(oi); return -AIOS_EMFILE; }
    p->fd[fd] = oi;
    return fd;
}
static long sys_fstatat(proc_t *p, uint64_t dirfd, uint64_t gpath, uint64_t gstat, uint64_t flags) {
    char path[256];
    long e = read_path(p, gpath, path, sizeof path); if (e) return e;
    pal_file_t dir; e = resolve_dir(p, dirfd, &dir);  if (e) return e;
    struct aios_stat s;
    int r = pal_host_fstatat(dir, path, &s, (flags & AIOS_AT_SYMLINK_NOFOLLOW) ? 0 : 1);
    if (r != 0) return r;
    if (pal_guest_write(p->pid, gstat, &s, sizeof s) != sizeof s) return -AIOS_EFAULT;
    return 0;
}
static long sys_unlinkat(proc_t *p, uint64_t dirfd, uint64_t gpath, uint64_t flags) {
    char path[256];
    long e = read_path(p, gpath, path, sizeof path); if (e) return e;
    pal_file_t dir; e = resolve_dir(p, dirfd, &dir);  if (e) return e;
    return pal_host_unlinkat(dir, path, (flags & AIOS_AT_REMOVEDIR) ? 1 : 0);
}
static long sys_faccessat(proc_t *p, uint64_t dirfd, uint64_t gpath, uint64_t amode) {
    char path[256];
    long e = read_path(p, gpath, path, sizeof path); if (e) return e;
    pal_file_t dir; e = resolve_dir(p, dirfd, &dir);  if (e) return e;
    return pal_host_faccessat(dir, path, (int)amode);
}

/* ---- pipes ----
 * A pipe is two backing ends (non-blocking at the host) sharing a pipe_id. read/write to a pipe
 * never block the single-threaded kernel: an empty read / full write PARKS the calling guest and
 * the kernel services others. pipe_settle() re-runs parked guests when a peer makes the pipe ready
 * (or closes its end). The bulk of a transfer is driven by the peers' own guest-level read/write
 * loops -- each loop iteration re-triggers settle -- so settle itself is a tiny non-recursive
 * fixpoint. read returns 0 (EOF) once all write ends close; write to an all-readers-closed pipe
 * returns short / -1 (the kernel ignores SIGPIPE so a host write yields PAL_EPIPE). */

/* One non-blocking read of a pipe read-end into the guest. Returns bytes delivered (>0),
 * 0 (EOF), or PAL_EWOULDBLOCK (empty but a write end is still open). */
static long pipe_read_once(proc_t *p, pal_file_t backing, uint64_t gbuf, uint64_t len) {
    char tmp[4096];
    size_t chunk = len < sizeof tmp ? (size_t)len : sizeof tmp;
    long n = pal_host_read(backing, tmp, chunk);
    if (n == PAL_EWOULDBLOCK) return PAL_EWOULDBLOCK;
    if (n <= 0) return 0;                            /* EOF (all write ends closed) or error */
    return (long)pal_guest_write(p->pid, gbuf, tmp, (size_t)n);
}

/* Push gbuf[*done .. len) into a pipe write-end as far as it accepts without blocking, advancing
 * *done. Returns 1 (all len written), PAL_EPIPE (no readers left), or 0 (pipe full, bytes pending). */
static int pipe_write_some(proc_t *p, pal_file_t backing, uint64_t gbuf, uint64_t len, uint64_t *done) {
    char tmp[4096];
    while (*done < len) {
        size_t chunk = (size_t)(len - *done);
        if (chunk > sizeof tmp) chunk = sizeof tmp;
        size_t got = pal_guest_read(p->pid, gbuf + *done, tmp, chunk);
        if (got == 0) return 1;                      /* unreadable guest buffer -> treat as done */
        long w = pal_host_write(backing, tmp, got);
        if (w == PAL_EWOULDBLOCK) return 0;          /* full -> park */
        if (w < 0) return PAL_EPIPE;                 /* broken pipe / error */
        *done += (size_t)w;
        if ((size_t)w < got) return 0;               /* partial (filled) -> park */
    }
    return 1;
}

/* Retry a parked reader/writer; returns 1 if it made progress (and is now resumed or refilled). */
static int try_deliver_read(proc_t *p) {
    long n = pipe_read_once(p, g_ofile[p->fd[p->blk_fd]].backing, p->blk_buf, p->blk_len);
    if (n == PAL_EWOULDBLOCK) return 0;              /* still empty */
    p->state = PS_RUNNING;
    pal_guest_return(p->pid, (uint64_t)n);           /* data (>0) or EOF (0) */
    return 1;
}
static int try_continue_write(proc_t *p) {
    uint64_t before = p->blk_done;
    int r = pipe_write_some(p, g_ofile[p->fd[p->blk_fd]].backing, p->blk_buf, p->blk_len, &p->blk_done);
    if (r == 1)        { p->state = PS_RUNNING; pal_guest_return(p->pid, (uint64_t)p->blk_len); return 1; }
    if (r == PAL_EPIPE){ p->state = PS_RUNNING;
                         pal_guest_return(p->pid, p->blk_done ? p->blk_done : (uint64_t)-AIOS_EPIPE); return 1; }
    return p->blk_done > before;                     /* wrote some but still blocked (full) */
}

/* A pipe's readiness changed. Retry every guest parked on it until none can progress -- a small
 * fixpoint (no recursion: peers drive the bulk transfer via their own guest-level loops). */
static void pipe_settle(int pipe_id) {
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < MAX_PROCS; i++) {
            proc_t *q = &g_proc[i];
            if (q->blk_pipe != pipe_id) continue;
            if      (q->state == PS_BLOCKED_READ)  { if (try_deliver_read(q))   progress = 1; }
            else if (q->state == PS_BLOCKED_WRITE) { if (try_continue_write(q)) progress = 1; }
        }
    }
}

/* read: pipe read-ends are non-blocking (park on empty); everything else fills up to len. Owns its
 * own pal_guest_return / parking, so it is dispatched specially. */
static void do_read(proc_t *p, uint64_t fd, uint64_t gbuf, uint64_t len) {
    p->state = PS_RUNNING;
    if (!fd_valid(p, fd)) { pal_guest_return(p->pid, (uint64_t)-AIOS_EBADF); return; }
    int oi = p->fd[fd];
    if (g_ofile[oi].is_pipe && !g_ofile[oi].pipe_write) {
        long n = pipe_read_once(p, g_ofile[oi].backing, gbuf, len);
        if (n == PAL_EWOULDBLOCK) {                  /* empty + writer open -> park the reader */
            p->state = PS_BLOCKED_READ;
            p->blk_fd = (int)fd; p->blk_buf = gbuf; p->blk_len = len;
            p->blk_done = 0; p->blk_pipe = g_ofile[oi].pipe_id;
            return;
        }
        if (n > 0) pipe_settle(g_ofile[oi].pipe_id); /* freed buffer space -> wake writers */
        pal_guest_return(p->pid, (uint64_t)n);
        return;
    }
    char tmp[1024];
    size_t total = 0;
    while (total < len) {
        size_t chunk = (size_t)len - total;
        if (chunk > sizeof tmp) chunk = sizeof tmp;
        long n = pal_host_read(g_ofile[oi].backing, tmp, chunk);
        if (n < 0)  { if (!total) { pal_guest_return(p->pid, (uint64_t)n); return; } break; }  /* -errno */
        if (n == 0) break;                           /* EOF */
        size_t put = pal_guest_write(p->pid, gbuf + total, tmp, (size_t)n);
        total += put;
        if (put < (size_t)n) break;
    }
    pal_guest_return(p->pid, (uint64_t)total);
}

/* write: pipe write-ends are non-blocking (park on full); everything else writes up to len. */
static void do_write(proc_t *p, uint64_t fd, uint64_t gbuf, uint64_t len) {
    p->state = PS_RUNNING;
    if (!fd_valid(p, fd)) { pal_guest_return(p->pid, (uint64_t)-AIOS_EBADF); return; }
    int oi = p->fd[fd];
    if (g_ofile[oi].is_pipe && g_ofile[oi].pipe_write) {
        uint64_t done = 0;
        int r = pipe_write_some(p, g_ofile[oi].backing, gbuf, len, &done);
        pipe_settle(g_ofile[oi].pipe_id);            /* wrote bytes -> wake readers */
        if (r == 1)         { pal_guest_return(p->pid, (uint64_t)len); return; }
        if (r == PAL_EPIPE) { pal_guest_return(p->pid, done ? done : (uint64_t)-AIOS_EPIPE); return; }
        p->state = PS_BLOCKED_WRITE;                 /* pipe full, bytes pending -> park */
        p->blk_fd = (int)fd; p->blk_buf = gbuf; p->blk_len = len;
        p->blk_done = done; p->blk_pipe = g_ofile[oi].pipe_id;
        return;
    }
    char tmp[1024];
    size_t total = 0;
    while (total < len) {
        size_t chunk = (size_t)len - total;
        if (chunk > sizeof tmp) chunk = sizeof tmp;
        size_t got = pal_guest_read(p->pid, gbuf + total, tmp, chunk);
        if (got == 0) break;
        long w = pal_host_write(g_ofile[oi].backing, tmp, got);
        if (w < 0) { if (!total) { pal_guest_return(p->pid, (uint64_t)w); return; } break; }  /* -errno */
        total += (size_t)w;
        if ((size_t)w < got) break;
    }
    pal_guest_return(p->pid, (uint64_t)total);
}

/* pipe: allocate two backing ends + two fds, tag them as a pipe, and store the fd pair in guest. */
static void do_pipe(proc_t *p, uint64_t gfds) {
    pal_file_t rd, wr;
    int pr = pal_host_pipe(&rd, &wr);
    if (pr != 0) { pal_guest_return(p->pid, (uint64_t)pr); return; }   /* -errno */
    int ri = ofile_alloc(rd);
    int wi = ofile_alloc(wr);
    int afd0 = -1, afd1 = -1;
    if (ri >= 0 && wi >= 0) {
        afd0 = fd_alloc(p);
        if (afd0 >= 0) { p->fd[afd0] = ri; afd1 = fd_alloc(p); }
    }
    if (ri < 0 || wi < 0 || afd0 < 0 || afd1 < 0) {  /* out of slots -> tear everything down */
        if (afd0 >= 0) p->fd[afd0] = -1;
        if (ri >= 0) ofile_unref(ri); else pal_host_close(rd);
        if (wi >= 0) ofile_unref(wi); else pal_host_close(wr);
        pal_guest_return(p->pid, (uint64_t)-AIOS_EMFILE);
        return;
    }
    p->fd[afd1] = wi;
    int pipe_id = g_next_pipe_id++;
    g_ofile[ri].is_pipe = 1; g_ofile[ri].pipe_id = pipe_id; g_ofile[ri].pipe_write = 0;
    g_ofile[wi].is_pipe = 1; g_ofile[wi].pipe_id = pipe_id; g_ofile[wi].pipe_write = 1;
    int fds[2] = { afd0, afd1 };                     /* fds[0] = read end, fds[1] = write end */
    pal_guest_write(p->pid, gfds, fds, sizeof fds);
    pal_guest_return(p->pid, 0);
}

/* ---- the process syscalls ---- */

static int wait_matches(unsigned long want, const proc_t *child) {
    return want == AIOS_WAIT_ANY || want == 0 || want == (unsigned long)child->pid;
}

/* fork: COPY the parent's fd table into a fresh child (refcounting shared backings), then let the
 * PAL place each side's return value (child pid in the parent, 0 in the child) and resume both. */
static void do_fork(proc_t *parent) {
    proc_t *child = proc_alloc();
    if (!child) { pal_guest_return(parent->pid, (uint64_t)-AIOS_EAGAIN); return; }   /* too many procs */
    pal_pid_t cpid = pal_guest_fork(parent->pid);
    if (cpid == PAL_PID_NONE) { pal_guest_resume(parent->pid); return; }   /* PAL set x0 = -1 */

    child->state = PS_RUNNING;
    child->pid = cpid;
    child->parent_pid = parent->pid;
    child->exit_code = 0;
    child->wait_for = 0;
    child->wait_status_gaddr = 0;
    for (int i = 0; i < AIOS_MAX_FD; i++) {
        child->fd[i] = parent->fd[i];
        if (parent->fd[i] >= 0) ofile_ref(parent->fd[i]);
    }
    pal_guest_resume(parent->pid);
    pal_guest_resume(cpid);
}

/* wait: reap a matching zombie child now, or -- if a matching child is still alive -- PARK the
 * caller (do not resume it) until that child exits (see on_exit). -1 if it has no such child. */
static void do_wait(proc_t *p, unsigned long want, uint64_t gstatus) {
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *c = &g_proc[i];
        if (c->state == PS_ZOMBIE && c->parent_pid == p->pid && wait_matches(want, c)) {
            if (gstatus) { int status = (c->exit_code & 0xff) << 8;
                           pal_guest_write(p->pid, gstatus, &status, sizeof status); }
            pal_pid_t cpid = c->pid;
            c->state = PS_FREE;                       /* reaped */
            pal_guest_return(p->pid, (uint64_t)cpid);
            return;
        }
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *c = &g_proc[i];
        if ((c->state == PS_RUNNING || c->state == PS_BLOCKED_WAIT) &&
            c->parent_pid == p->pid && wait_matches(want, c)) {
            p->state = PS_BLOCKED_WAIT;               /* park: a matching child is still alive */
            p->wait_for = want;
            p->wait_status_gaddr = gstatus;
            return;                                   /* do NOT resume p */
        }
    }
    pal_guest_return(p->pid, (uint64_t)-AIOS_ECHILD); /* no such child */
}

/* A guest exited. First detach its own children (free zombies, orphan the living). Then either
 * wake a parent parked in wait (delivering the status + reaping), or become a zombie for a running
 * parent to reap later, or -- no parent -- free outright. */
static void on_exit(proc_t *p, int code) {
    for (int i = 0; i < AIOS_MAX_FD; i++) fd_release(p, i);  /* close its fds (pipe ends -> EOF) */
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *c = &g_proc[i];
        if (c != p && c->state != PS_FREE && c->parent_pid == p->pid) {
            if (c->state == PS_ZOMBIE) c->state = PS_FREE;   /* no one left to reap it */
            else                       c->parent_pid = PAL_PID_NONE;  /* orphan */
        }
    }
    proc_t *parent = (p->parent_pid != PAL_PID_NONE) ? proc_find(p->parent_pid) : NULL;
    if (parent && parent->state == PS_BLOCKED_WAIT && wait_matches(parent->wait_for, p)) {
        if (parent->wait_status_gaddr) { int status = (code & 0xff) << 8;
            pal_guest_write(parent->pid, parent->wait_status_gaddr, &status, sizeof status); }
        pal_pid_t cpid = p->pid;
        parent->state = PS_RUNNING;
        p->state = PS_FREE;                           /* reaped by the wakeup */
        pal_guest_return(parent->pid, (uint64_t)cpid);
    } else if (parent) {
        p->exit_code = code;
        p->state = PS_ZOMBIE;                         /* parent alive but not waiting (yet) */
    } else {
        p->state = PS_FREE;                           /* orphan / init: nothing waits */
    }
}

/* Service one trapped syscall for process p. mmap/exec/fork/wait/exit are "special": they place
 * the guest's result themselves and manage their own resume (or parking), so they do NOT go
 * through pal_guest_return. */
static void dispatch(proc_t *p, const pal_syscall_t *sc) {
    switch (sc->nr) {
    case AIOS_SYS_MMAP: pal_guest_mmap(p->pid, (size_t)sc->arg[0]); pal_guest_resume(p->pid); return;
    case AIOS_SYS_EXEC: pal_guest_exec(p->pid, sc->arg[0], sc->arg[1], sc->arg[2]);
                        pal_guest_resume(p->pid); return;
    case AIOS_SYS_FORK: do_fork(p);                                        return;
    case AIOS_SYS_WAIT: do_wait(p, (unsigned long)sc->arg[0], sc->arg[1]); return;
    case AIOS_SYS_EXIT: pal_guest_exit(p->pid, (int)sc->arg[0]);           return;
    /* read/write/pipe own their own return/parking (pipes may block) -> dispatched specially */
    case AIOS_SYS_READ:  do_read (p, sc->arg[0], sc->arg[1], sc->arg[2]); return;
    case AIOS_SYS_WRITE: do_write(p, sc->arg[0], sc->arg[1], sc->arg[2]); return;
    case AIOS_SYS_PIPE:  do_pipe (p, sc->arg[0]);                         return;
    }
    uint64_t ret;
    switch (sc->nr) {
    case AIOS_SYS_OPEN:  ret = (uint64_t)sys_open (p, sc->arg[0], sc->arg[1], sc->arg[2]); break;
    case AIOS_SYS_CLOSE: ret = (uint64_t)sys_close(p, sc->arg[0]);                         break;
    case AIOS_SYS_LSEEK: ret = (uint64_t)sys_lseek(p, sc->arg[0], sc->arg[1], sc->arg[2]); break;
    case AIOS_SYS_FSTAT: ret = (uint64_t)sys_fstat(p, sc->arg[0], sc->arg[1]);             break;
    case AIOS_SYS_DUP2:  ret = (uint64_t)sys_dup2 (p, sc->arg[0], sc->arg[1]);             break;
    case AIOS_SYS_STAT:  ret = (uint64_t)sys_stat (p, sc->arg[0], sc->arg[1], 1);          break;
    case AIOS_SYS_LSTAT: ret = (uint64_t)sys_stat (p, sc->arg[0], sc->arg[1], 0);          break;
    case AIOS_SYS_GETCWD:ret = (uint64_t)sys_getcwd(p, sc->arg[0], sc->arg[1]);            break;
    case AIOS_SYS_CHDIR: ret = (uint64_t)sys_chdir(p, sc->arg[0]);                         break;
    case AIOS_SYS_UNLINK:ret = (uint64_t)sys_unlink(p, sc->arg[0]);                        break;
    case AIOS_SYS_MKDIR: ret = (uint64_t)sys_mkdir(p, sc->arg[0], sc->arg[1]);             break;
    case AIOS_SYS_RMDIR: ret = (uint64_t)sys_rmdir(p, sc->arg[0]);                         break;
    case AIOS_SYS_RENAME:ret = (uint64_t)sys_rename(p, sc->arg[0], sc->arg[1]);            break;
    case AIOS_SYS_GETPID:ret = (uint64_t)p->pid;                                           break;
    case AIOS_SYS_GETDENTS:ret = (uint64_t)sys_getdents(p, sc->arg[0], sc->arg[1], sc->arg[2]); break;
    case AIOS_SYS_OPENAT:  ret = (uint64_t)sys_openat (p, sc->arg[0], sc->arg[1], sc->arg[2], sc->arg[3]); break;
    case AIOS_SYS_FSTATAT: ret = (uint64_t)sys_fstatat(p, sc->arg[0], sc->arg[1], sc->arg[2], sc->arg[3]); break;
    case AIOS_SYS_UNLINKAT:ret = (uint64_t)sys_unlinkat(p, sc->arg[0], sc->arg[1], sc->arg[2]);          break;
    case AIOS_SYS_FACCESSAT:ret = (uint64_t)sys_faccessat(p, sc->arg[0], sc->arg[1], sc->arg[2]);        break;
    default:             ret = (uint64_t)-AIOS_ENOSYS; /* unknown AIOS syscall */           break;
    }
    pal_guest_return(p->pid, ret);
}

/* The kernel's event loop: wait for ANY guest's next syscall or exit, service it, repeat -- until
 * no guests remain. Returns the INIT guest's exit code (the program the kernel was launched with). */
int aios_kernel_run(const char *guest_path, char *const guest_argv[]) {
    ofile_init_std();
    pal_pid_t pid = pal_guest_spawn(guest_path, guest_argv);
    if (pid == PAL_PID_NONE) return -1;

    proc_t *init = proc_alloc();
    init->state = PS_RUNNING;
    init->pid = pid;
    init->parent_pid = PAL_PID_NONE;
    init->exit_code = 0;
    init->wait_for = 0;
    init->wait_status_gaddr = 0;
    fd_table_init_std(init);

    pal_pid_t init_pid = pid;
    int init_code = 0;
    pal_guest_resume(pid);                            /* start init toward its first syscall */

    for (;;) {
        pal_pid_t who; pal_syscall_t sc; int code = 0;
        int r = pal_guest_next(&who, &sc, &code);
        if (r < 0) break;                             /* no live guests left */
        proc_t *p = proc_find(who);
        if (r == 0) {                                 /* `who` exited */
            if (who == init_pid) init_code = code;
            if (p) on_exit(p, code);
            continue;
        }
        if (!p) { pal_guest_resume(who); continue; }  /* unknown pid (shouldn't happen) */
        dispatch(p, &sc);
    }
    return init_code;
}

int main(int argc, char **argv) {
    if (argc < 2) { kputs("usage: aios-uk <guest-program> [args...]\n"); return 2; }
    const char *guest = argv[1];
    /* The guest's argv is the kernel's argv shifted by one: guest argv[0] = the guest program. */
    char *const *guest_argv = (char *const *)&argv[1];

    kputs("[aios-uk] AIOS v" AIOS_VERSION_STR " -- " AIOS_VERSION_LINE " on Linux (ptrace PAL)\n");
    kputs("[aios-uk] launching guest: ");
    kputs(guest);
    kputs("\n");

    int code = aios_kernel_run(guest, guest_argv);

    kputs("[aios-uk] init guest exited via AIOS ABI, code=");
    kput_int(code);
    kputs("\n");
    return code;
}
