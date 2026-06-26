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
enum { PS_FREE = 0, PS_RUNNING, PS_ZOMBIE, PS_BLOCKED_WAIT, PS_BLOCKED_READ, PS_BLOCKED_WRITE,
       PS_STOPPED /* job control: stopped by SIGSTOP/SIGTSTP, not resumed until SIGCONT */ };
typedef struct {
    int           state;
    pal_pid_t     pid;
    pal_pid_t     parent_pid;          /* PAL_PID_NONE for init / orphans */
    pal_pid_t     pgid;                /* PROCESS GROUP (job control); inherited across fork, preserved
                                        * across exec. init is its own leader (pgid == pid). */
    int           exit_code;           /* valid in PS_ZOMBIE */
    unsigned long wait_for;            /* PS_BLOCKED_WAIT: AIOS_WAIT_ANY/0, or a specific pid */
    unsigned long wait_flags;          /* PS_BLOCKED_WAIT: the wait options (WUNTRACED/WCONTINUED/...) */
    uint64_t      wait_status_gaddr;   /* PS_BLOCKED_WAIT: where to store the status (0 = none) */
    int           stopped_sig;         /* PS_STOPPED: the signal that stopped it (for WSTOPSIG) */
    int           report_stop;         /* a stop event awaits collection by wait(WUNTRACED) */
    int           report_cont;         /* a continue event awaits collection by wait(WCONTINUED) */
    /* PS_BLOCKED_READ/WRITE: a pipe I/O parked because the pipe was empty/full. The kernel resumes
     * it from pipe_settle() when a peer makes the pipe ready (or closes its end -> EOF/EPIPE). */
    int           blk_fd;              /* the AIOS fd being read/written */
    uint64_t      blk_buf;             /* guest buffer */
    uint64_t      blk_len;             /* total bytes requested */
    uint64_t      blk_done;            /* bytes already transferred (writes) */
    int           blk_pipe;            /* pipe_id this I/O is parked on */
    /* signals: per-process dispositions + one pending signal + saved regs while a handler runs */
    uint64_t      sig_handler[AIOS_NSIG];   /* 0 = SIG_DFL, 1 = SIG_IGN, else a guest handler address */
    uint64_t      sig_tramp;           /* the guest's sigreturn trampoline (registered via sigaction) */
    uint64_t      sig_mask;            /* BLOCKED signals (bit 1<<signum); inherited across fork. A
                                        * masked pending signal waits until sigprocmask unblocks it. */
    int           pending_sig;         /* a signal awaiting delivery (0 = none) */
    int           in_handler;          /* a handler is currently running on this guest */
    unsigned char sigsave[PAL_SIGSAVE_SIZE];   /* pre-signal regs, restored by sigreturn */
    char          cwd[1024];           /* PER-PROCESS current directory (absolute, normalized);
                                        * inherited across fork, preserved across exec. The kernel
                                        * pre-absolutes every guest path against it (cwd_join), so a
                                        * subshell's cd no longer leaks into siblings/parent. */
    unsigned int  umask;               /* PER-PROCESS file-creation mask; applied on open(O_CREAT)/
                                        * mkdir, inherited across fork, preserved across exec. The
                                        * host umask is neutralized so only this one masks. */
    int           fd[AIOS_MAX_FD];     /* AIOS fd -> ofile index, or -1 */
} proc_t;

static void sig_reset(proc_t *p) {     /* dispositions to default; no pending signal */
    for (int i = 0; i < AIOS_NSIG; i++) p->sig_handler[i] = AIOS_SIG_DFL;
    p->sig_tramp = 0;
    p->sig_mask = 0;
    p->pending_sig = 0;
    p->in_handler = 0;
}
static proc_t g_proc[MAX_PROCS];

/* The foreground PROCESS GROUP of the controlling terminal (job control). tcsetpgrp sets it,
 * tcgetpgrp reads it; seeded to init's pgid (init starts in the foreground). Terminal-signal routing
 * to this group is a later increment -- today it is faithfully tracked kernel state. */
static pal_pid_t g_fg_pgrp;

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

/* path helpers (defined below with the per-process-cwd layer); forward-declared for sys_open above them */
static long read_path(proc_t *p, uint64_t gpath, char *dst, size_t cap);
static long read_abspath(proc_t *p, uint64_t gpath, char *out, size_t outsz);

static long sys_open(proc_t *p, uint64_t gpath, uint64_t flags, uint64_t mode) {
    char path[1536];
    long pe = read_abspath(p, gpath, path, sizeof path); if (pe) return pe;   /* absolute against p->cwd */
    if (flags & AIOS_O_CREAT) mode &= ~(uint64_t)p->umask;                    /* per-process create mask */
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

/* fcntl: F_DUPFD/F_DUPFD_CLOEXEC = alias the fd's backing onto the lowest free fd >= arg (dash parks
 * its script fd above 10 this way); the FD-flag + status-flag commands are accepted no-ops. */
static long sys_fcntl(proc_t *p, uint64_t fd, uint64_t cmd, uint64_t arg) {
    if (!fd_valid(p, fd)) return -AIOS_EBADF;
    switch (cmd) {
    case AIOS_F_DUPFD:
    case AIOS_F_DUPFD_CLOEXEC: {
        if (arg >= AIOS_MAX_FD) return -AIOS_EINVAL;
        int newfd = -1;
        for (int i = (int)arg; i < AIOS_MAX_FD; i++) if (p->fd[i] < 0) { newfd = i; break; }
        if (newfd < 0) return -AIOS_EMFILE;
        p->fd[newfd] = p->fd[fd];
        ofile_ref(p->fd[newfd]);
        return newfd;
    }
    case AIOS_F_GETFD: case AIOS_F_GETFL: return 0;     /* no per-fd flags modelled */
    case AIOS_F_SETFD: case AIOS_F_SETFL: return 0;
    default: return -AIOS_EINVAL;
    }
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

/* ---- per-process cwd ----
 * Make `in` absolute against p->cwd: absolute as-is, else cwd + "/" + in. No normalization here -- the
 * host (or openat2 RESOLVE_IN_ROOT) resolves "."/".."; only chdir's STORED cwd is normalized. The cwd
 * is a process attribute, so it lives in the kernel (not the host-shared PAL) -- a future seL4 PAL has
 * no cwd of its own either. */
static void cwd_join(proc_t *p, const char *in, char *out, size_t outsz) {
    size_t o = 0;
    if (in[0] != '/') {
        for (const char *c = p->cwd; *c && o < outsz - 1; c++) out[o++] = *c;   /* p->cwd (always >= "/") */
        if (!(o == 1 && out[0] == '/') && o < outsz - 1) out[o++] = '/';        /* separator, unless cwd == "/" */
    }
    for (const char *s = in; *s && o < outsz - 1; s++) out[o++] = *s;
    out[o] = '\0';
    if (o == 0) { out[0] = '/'; out[1] = '\0'; }
}

/* Collapse an absolute path textually ("."/empty dropped, ".." popped + clamped at "/"). Logical only
 * (the real resolution is the host's); keeps a tidy, bounded cwd for chdir/getcwd. */
static void path_norm(const char *in, char *out, size_t outsz) {
    size_t olen = 0;                                  /* build "/c1/c2/..."; empty result => "/" */
    const char *pp = in;
    while (*pp) {
        while (*pp == '/') pp++;
        if (!*pp) break;
        const char *seg = pp;
        while (*pp && *pp != '/') pp++;
        size_t clen = (size_t)(pp - seg);
        if (clen == 1 && seg[0] == '.') continue;
        if (clen == 2 && seg[0] == '.' && seg[1] == '.') {
            while (olen > 0 && out[olen - 1] != '/') olen--;
            if (olen > 0) olen--;
            continue;
        }
        if (olen + 1 + clen >= outsz) break;
        out[olen++] = '/';
        for (size_t i = 0; i < clen; i++) out[olen++] = seg[i];
    }
    if (olen == 0) out[olen++] = '/';
    out[olen] = '\0';
}

/* Read a guest path and make it absolute against p->cwd (for plain-path / AT_FDCWD ops). 0, or -errno. */
static long read_abspath(proc_t *p, uint64_t gpath, char *out, size_t outsz) {
    char rel[256];
    long e = read_path(p, gpath, rel, sizeof rel); if (e) return e;
    cwd_join(p, rel, out, outsz);
    return 0;
}

static long sys_stat(proc_t *p, uint64_t gpath, uint64_t gstat, int follow) {
    char path[1536];
    long e = read_abspath(p, gpath, path, sizeof path); if (e) return e;
    struct aios_stat s;
    int r = pal_host_stat(path, &s, follow);
    if (r != 0) return r;
    if (pal_guest_write(p->pid, gstat, &s, sizeof s) != sizeof s) return -AIOS_EFAULT;
    return 0;
}

/* getcwd returns the PER-PROCESS cwd the kernel tracks (no longer a host/PAL-global value). */
static long sys_getcwd(proc_t *p, uint64_t gbuf, uint64_t size) {
    size_t n = kstrlen(p->cwd);
    if (n + 1 > size) return -AIOS_ERANGE;
    if (pal_guest_write(p->pid, gbuf, p->cwd, n + 1) != n + 1) return -AIOS_EFAULT;   /* +NUL */
    return (long)n;
}

/* chdir verifies the target is a reachable directory (confined or not), then stores the normalized
 * absolute path as THIS process's cwd -- a sibling/parent's cwd is untouched. */
static long sys_chdir(proc_t *p, uint64_t gpath) {
    char abs[1536];
    long e = read_abspath(p, gpath, abs, sizeof abs); if (e) return e;
    int r = pal_host_chdir(abs);                       /* verify it is a directory */
    if (r != 0) return r;
    char norm[1024]; path_norm(abs, norm, sizeof norm);
    size_t i = 0; for (; norm[i] && i < sizeof p->cwd - 1; i++) p->cwd[i] = norm[i]; p->cwd[i] = '\0';
    return 0;
}
static long sys_unlink(proc_t *p, uint64_t gpath) {
    char path[1536]; long e = read_abspath(p, gpath, path, sizeof path); if (e) return e;
    return pal_host_unlink(path);
}
static long sys_mkdir (proc_t *p, uint64_t gpath, uint64_t mode) {
    char path[1536]; long e = read_abspath(p, gpath, path, sizeof path); if (e) return e;
    return pal_host_mkdir(path, (unsigned int)mode & ~p->umask);              /* per-process create mask */
}
static long sys_rmdir (proc_t *p, uint64_t gpath) {
    char path[1536]; long e = read_abspath(p, gpath, path, sizeof path); if (e) return e;
    return pal_host_rmdir(path);
}
static long sys_rename(proc_t *p, uint64_t gold, uint64_t gnew) {
    char o[1536], n[1536];
    long e = read_abspath(p, gold, o, sizeof o); if (e) return e;
    e = read_abspath(p, gnew, n, sizeof n);      if (e) return e;
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

/* Read a guest path for an *at op: resolve the dirfd, and -- if it is AT_FDCWD -- make the path
 * absolute against p->cwd (so it resolves against THIS process's cwd, not a global one). A real dirfd
 * keeps the path relative to it. Fills *outdir + `out`. 0, or -errno. */
static long read_at(proc_t *p, uint64_t dirfd, uint64_t gpath, pal_file_t *outdir, char *out, size_t outsz) {
    char rel[256];
    long e = read_path(p, gpath, rel, sizeof rel); if (e) return e;
    e = resolve_dir(p, dirfd, outdir);             if (e) return e;
    if (*outdir == PAL_AT_FDCWD) cwd_join(p, rel, out, outsz);
    else { size_t i = 0; for (; rel[i] && i < outsz - 1; i++) out[i] = rel[i]; out[i] = '\0'; }
    return 0;
}

static long sys_openat(proc_t *p, uint64_t dirfd, uint64_t gpath, uint64_t flags, uint64_t mode) {
    char path[1536]; pal_file_t dir;
    long e = read_at(p, dirfd, gpath, &dir, path, sizeof path); if (e) return e;
    if (flags & AIOS_O_CREAT) mode &= ~(uint64_t)p->umask;                    /* per-process create mask */
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
    char path[1536]; pal_file_t dir;
    long e = read_at(p, dirfd, gpath, &dir, path, sizeof path); if (e) return e;
    struct aios_stat s;
    int r = pal_host_fstatat(dir, path, &s, (flags & AIOS_AT_SYMLINK_NOFOLLOW) ? 0 : 1);
    if (r != 0) return r;
    if (pal_guest_write(p->pid, gstat, &s, sizeof s) != sizeof s) return -AIOS_EFAULT;
    return 0;
}
static long sys_unlinkat(proc_t *p, uint64_t dirfd, uint64_t gpath, uint64_t flags) {
    char path[1536]; pal_file_t dir;
    long e = read_at(p, dirfd, gpath, &dir, path, sizeof path); if (e) return e;
    return pal_host_unlinkat(dir, path, (flags & AIOS_AT_REMOVEDIR) ? 1 : 0);
}
static long sys_faccessat(proc_t *p, uint64_t dirfd, uint64_t gpath, uint64_t amode) {
    char path[1536]; pal_file_t dir;
    long e = read_at(p, dirfd, gpath, &dir, path, sizeof path); if (e) return e;
    return pal_host_faccessat(dir, path, (int)amode);
}

/* Read a symlink target into the guest (no NUL, like POSIX readlink). */
static long sys_readlink(proc_t *p, uint64_t gpath, uint64_t gbuf, uint64_t bufsize) {
    char path[1536], link[1024];
    long e = read_abspath(p, gpath, path, sizeof path); if (e) return e;
    size_t cap = bufsize < sizeof link ? (size_t)bufsize : sizeof link;
    long n = pal_host_readlink(path, link, cap);
    if (n < 0) return n;
    if (pal_guest_write(p->pid, gbuf, link, (size_t)n) != (size_t)n) return -AIOS_EFAULT;
    return n;
}

static long sys_isatty(proc_t *p, uint64_t fd) {
    if (!fd_valid(p, fd)) return 0;
    return pal_host_isatty(fd_backing(p, fd));
}

/* Set this process's file-creation mask; return the previous one (POSIX umask -- always succeeds). */
static long sys_umask(proc_t *p, uint64_t mask) {
    unsigned int old = p->umask;
    p->umask = (unsigned int)mask & 0777;
    return (long)old;
}

/* Read a clock (AIOS_CLOCK_*) into the guest's struct aios_timespec. The kernel's only time source. */
static long sys_clock_gettime(proc_t *p, uint64_t clk_id, uint64_t gts) {
    struct aios_timespec ts;
    int r = pal_host_clock_gettime((int)clk_id, &ts);
    if (r != 0) return r;
    if (pal_guest_write(p->pid, gts, &ts, sizeof ts) != sizeof ts) return -AIOS_EFAULT;
    return 0;
}

/* ---- file-metadata *at family (mode / owner / symlink / hardlink / times) ---- */
static long sys_fchmodat(proc_t *p, uint64_t dirfd, uint64_t gpath, uint64_t mode, uint64_t flags) {
    char path[1536]; pal_file_t dir;
    long e = read_at(p, dirfd, gpath, &dir, path, sizeof path); if (e) return e;
    return pal_host_fchmodat(dir, path, (unsigned int)mode, (flags & AIOS_AT_SYMLINK_NOFOLLOW) ? 1 : 0);
}
static long sys_fchownat(proc_t *p, uint64_t dirfd, uint64_t gpath, uint64_t owner, uint64_t group, uint64_t flags) {
    char path[1536]; pal_file_t dir;
    long e = read_at(p, dirfd, gpath, &dir, path, sizeof path); if (e) return e;
    return pal_host_fchownat(dir, path, (unsigned int)owner, (unsigned int)group, (flags & AIOS_AT_SYMLINK_NOFOLLOW) ? 1 : 0);
}
static long sys_symlinkat(proc_t *p, uint64_t gtarget, uint64_t newdirfd, uint64_t glinkpath) {
    char target[1024], linkpath[1536]; pal_file_t newdir;
    long e = read_path(p, gtarget, target, sizeof target); if (e) return e;   /* target stored verbatim */
    e = read_at(p, newdirfd, glinkpath, &newdir, linkpath, sizeof linkpath);  if (e) return e;
    return pal_host_symlinkat(target, newdir, linkpath);
}
static long sys_linkat(proc_t *p, uint64_t olddirfd, uint64_t goldpath, uint64_t newdirfd, uint64_t gnewpath, uint64_t flags) {
    char oldpath[1536], newpath[1536]; pal_file_t olddir, newdir;
    long e = read_at(p, olddirfd, goldpath, &olddir, oldpath, sizeof oldpath); if (e) return e;
    e = read_at(p, newdirfd, gnewpath, &newdir, newpath, sizeof newpath);      if (e) return e;
    return pal_host_linkat(olddir, oldpath, newdir, newpath, (flags & AIOS_AT_SYMLINK_FOLLOW) ? 1 : 0);
}
static long sys_utimensat(proc_t *p, uint64_t dirfd, uint64_t gpath, uint64_t gtimes, uint64_t flags) {
    char path[1536]; pal_file_t dir;
    long e = read_at(p, dirfd, gpath, &dir, path, sizeof path); if (e) return e;
    struct aios_timespec ts[2];
    const struct aios_timespec *tp = NULL;
    if (gtimes) {                                          /* NULL = "now"; else 2 timespecs */
        if (pal_guest_read(p->pid, gtimes, ts, sizeof ts) != sizeof ts) return -AIOS_EFAULT;
        tp = ts;
    }
    return pal_host_utimensat(dir, path, tp, (flags & AIOS_AT_SYMLINK_NOFOLLOW) ? 1 : 0);
}

/* ---- signals ---- */
static long sys_sigaction(proc_t *p, uint64_t signum, uint64_t handler, uint64_t tramp) {
    if (signum < 1 || signum >= AIOS_NSIG) return -AIOS_EINVAL;
    if (signum == 9 || signum == 19) return -AIOS_EINVAL;          /* SIGKILL/SIGSTOP uncatchable */
    uint64_t old = p->sig_handler[signum];
    p->sig_handler[signum] = handler;
    if (tramp) p->sig_tramp = tramp;                              /* the guest's sigreturn trampoline */
    return (long)old;
}
/* sigprocmask: examine/change the BLOCKED signal set. how = BLOCK/UNBLOCK/SETMASK; SIGKILL(9) and
 * SIGSTOP(19) can never be blocked. A signal blocked here stays pending (kreturn leaves it) until it
 * is unblocked -- delivered then, at the unblocking syscall's own exit. */
static long sys_sigprocmask(proc_t *p, uint64_t how, uint64_t set_gaddr, uint64_t old_gaddr) {
    uint64_t old = p->sig_mask;
    if (old_gaddr) pal_guest_write(p->pid, old_gaddr, &old, sizeof old);
    if (set_gaddr) {
        uint64_t set = 0;
        pal_guest_read(p->pid, set_gaddr, &set, sizeof set);
        if      (how == AIOS_SIG_BLOCK)   p->sig_mask |= set;
        else if (how == AIOS_SIG_UNBLOCK) p->sig_mask &= ~set;
        else if (how == AIOS_SIG_SETMASK) p->sig_mask = set;
        else return -AIOS_EINVAL;
        p->sig_mask &= ~((1ULL << 9) | (1ULL << 19));   /* SIGKILL / SIGSTOP are never blockable */
    }
    return 0;
}

static int wait_matches(unsigned long want, const proc_t *child);   /* defined with the wait code */
static void proc_stop(proc_t *p, int sig);
static void proc_cont(proc_t *p);

/* SIGSTOP(19)/SIGTSTP(20)/SIGTTIN(21)/SIGTTOU(22): default action = STOP the process. */
static int is_stop_sig(int sig) { return sig == 19 || sig == 20 || sig == 21 || sig == 22; }

/* Deliver one signal to one process. SIGCONT continues a stopped process IMMEDIATELY (it is not at a
 * syscall, so it cannot wait for the syscall-exit delivery path); otherwise the signal is POSTED as
 * pending and acted on at the target's next syscall exit (kreturn) -- the M5 model. A signal sent to
 * an already-stopped process is queued (delivered once it continues); a redundant stop is dropped. */
static void deliver_signal(proc_t *t, int signum) {
    if (!signum) return;                                         /* existence probe -- no signal */
    if (signum == 18 /*SIGCONT*/) { if (t->state == PS_STOPPED) proc_cont(t); else t->pending_sig = 18; return; }
    if (t->state == PS_STOPPED) { if (!is_stop_sig(signum)) t->pending_sig = signum; return; }
    t->pending_sig = signum;
}

/* kill: pid > 0 = one process; pid == 0 = the CALLER's process group; pid < 0 = the process group
 * -pid; pid == -1 (broadcast) is not supported. Negative/zero is how killpg(pgrp,sig) reaches a whole
 * job (e.g. dash SIGCONT'ing a stopped job). signum 0 = existence check (no signal posted). */
static long sys_kill(proc_t *p, uint64_t pid, uint64_t signum) {
    if (signum >= AIOS_NSIG) return -AIOS_EINVAL;
    long spid = (long)pid;
    if (spid > 0) {                                               /* a single process */
        proc_t *t = proc_find((pal_pid_t)spid);
        if (!t || t->state == PS_ZOMBIE) return -AIOS_ESRCH;
        deliver_signal(t, (int)signum);
        return 0;
    }
    pal_pid_t grp = (spid == 0) ? p->pgid : (pal_pid_t)(-spid);   /* the target process group */
    int found = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *t = &g_proc[i];
        if (t->state == PS_FREE || t->state == PS_ZOMBIE) continue;
        if (t->pgid != grp) continue;
        found = 1;
        deliver_signal(t, (int)signum);                          /* signum 0 -> just an existence probe */
    }
    return found ? 0 : -AIOS_ESRCH;
}

/* ---- process groups + controlling-terminal foreground group (job-control foundation) ---- */
/* setpgid(pid, pgid): pid 0 = caller; pgid 0 = pid (become a group leader). The target must be the
 * caller or one of its children (POSIX). Kernel-tracked state; terminal-signal routing comes later. */
static long sys_setpgid(proc_t *p, uint64_t pid, uint64_t pgid) {
    proc_t *t = (pid == 0) ? p : proc_find((pal_pid_t)pid);
    if (!t) return -AIOS_ESRCH;
    if (t != p && t->parent_pid != p->pid) return -AIOS_EPERM;   /* only self or a child */
    t->pgid = (pgid == 0) ? t->pid : (pal_pid_t)pgid;
    return 0;
}
static long sys_getpgid(proc_t *p, uint64_t pid) {
    proc_t *t = (pid == 0) ? p : proc_find((pal_pid_t)pid);
    if (!t) return -AIOS_ESRCH;
    return (long)t->pgid;
}
static long sys_tcsetpgrp(proc_t *p, uint64_t fd, uint64_t pgrp) {
    if (!fd_valid(p, fd)) return -AIOS_EBADF;
    if (!pal_host_isatty(fd_backing(p, fd))) return -AIOS_ENOTTY;
    g_fg_pgrp = (pal_pid_t)pgrp;
    return 0;
}
static long sys_tcgetpgrp(proc_t *p, uint64_t fd) {
    if (!fd_valid(p, fd)) return -AIOS_EBADF;
    if (!pal_host_isatty(fd_backing(p, fd))) return -AIOS_ENOTTY;
    return (long)g_fg_pgrp;
}

/* Mark p STOPPED (job control). The caller has already left the tracee stopped at its current stop
 * (kreturn plants the syscall result via setret first; the async path is already at a signal-stop).
 * Report the stop to a parent parked in wait(WUNTRACED) now, else leave report_stop to collect later. */
static void proc_stop(proc_t *p, int sig) {
    p->state = PS_STOPPED;
    p->stopped_sig = sig;
    p->report_stop = 1;
    proc_t *parent = (p->parent_pid != PAL_PID_NONE) ? proc_find(p->parent_pid) : NULL;
    if (parent && parent->state == PS_BLOCKED_WAIT && (parent->wait_flags & AIOS_WUNTRACED)
        && wait_matches(parent->wait_for, p)) {
        int status = ((sig & 0xff) << 8) | 0x7f;                 /* WIFSTOPPED, WSTOPSIG == sig */
        if (parent->wait_status_gaddr)
            pal_guest_write(parent->pid, parent->wait_status_gaddr, &status, sizeof status);
        p->report_stop = 0;
        parent->state = PS_RUNNING;
        pal_guest_return(parent->pid, (uint64_t)p->pid);
    }
}
/* Continue a stopped process: resume it from where it stopped, and report the continue to a parent
 * parked in wait(WCONTINUED) now (else leave report_cont to collect later). */
static void proc_cont(proc_t *p) {
    if (p->state != PS_STOPPED) return;
    p->state = PS_RUNNING;
    p->report_stop = 0;                                          /* an uncollected stop is superseded */
    p->report_cont = 1;
    pal_guest_resume(p->pid);                                    /* continue from the stop (result already planted) */
    proc_t *parent = (p->parent_pid != PAL_PID_NONE) ? proc_find(p->parent_pid) : NULL;
    if (parent && parent->state == PS_BLOCKED_WAIT && (parent->wait_flags & AIOS_WCONTINUED)
        && wait_matches(parent->wait_for, p)) {
        int status = 0xffff;                                     /* WIFCONTINUED */
        if (parent->wait_status_gaddr)
            pal_guest_write(parent->pid, parent->wait_status_gaddr, &status, sizeof status);
        p->report_cont = 0;
        parent->state = PS_RUNNING;
        pal_guest_return(parent->pid, (uint64_t)p->pid);
    }
}

/* Return a syscall result to p, then -- if p now has a pending signal -- act on it AT THE SYSCALL
 * EXIT (the syscall is fully serviced first, so its return value is intact; the handler runs next;
 * on sigreturn the guest resumes after the syscall). This is the single return path for every
 * dispatched syscall, so e.g. raise()'s handler runs before raise() returns, the POSIX ordering. */
static int sig_blocked(const proc_t *p, int sig) {               /* SIGKILL/SIGSTOP are never blocked */
    return sig != 9 && sig != 19 && (p->sig_mask & (1ULL << sig));
}
static void kreturn(proc_t *p, uint64_t ret) {
    int sig = p->pending_sig;
    if (!sig) { pal_guest_return(p->pid, ret); return; }         /* common case: no signal */
    if (sig_blocked(p, sig)) { pal_guest_return(p->pid, ret); return; }   /* blocked -> stays pending */
    p->pending_sig = 0;
    uint64_t h = p->sig_handler[sig];
    int dfl_ignore = (sig == 17 || sig == 18 || sig == 23 || sig == 28);   /* CHLD/CONT/URG/WINCH */
    if (h == AIOS_SIG_IGN || (h == AIOS_SIG_DFL && dfl_ignore)) {
        pal_guest_return(p->pid, ret);                           /* ignored -> just return */
        return;
    }
    if (is_stop_sig(sig) && h == AIOS_SIG_DFL) {                  /* default action of a stop signal: STOP */
        pal_guest_setret(p->pid, ret);                           /* finish the syscall, stay stopped at its exit */
        proc_stop(p, sig);                                       /* PS_STOPPED + notify a WUNTRACED parent */
        return;                                                  /* do NOT resume -- wait for SIGCONT */
    }
    if (h == AIOS_SIG_DFL) {                                      /* default action: terminate */
        pal_guest_exit(p->pid, 128 + sig);
        return;
    }
    if (p->sig_tramp == 0) { pal_guest_return(p->pid, ret); return; }   /* no trampoline -> can't deliver */
    pal_guest_setret(p->pid, ret);                               /* finish the syscall (stay stopped at exit) */
    p->in_handler = 1;
    pal_guest_deliver(p->pid, h, (uint64_t)sig, p->sig_tramp, p->sigsave);   /* run the handler */
}

/* A guest stopped on an ASYNCHRONOUS signal `sig` (e.g. terminal ^C -> SIGINT, delivered to the
 * process group). Act on its disposition: run the handler (the guest is stopped at an arbitrary PC;
 * pal_guest_deliver saves it as-is and sigreturn restores it), ignore it, or terminate. This is how
 * a running dash catches ^C and returns to its prompt while a handler-less foreground child dies. */
static void handle_signal_stop(proc_t *p, int sig) {
    if (sig < 1 || sig >= AIOS_NSIG) { pal_guest_resume(p->pid); return; }
    if (sig_blocked(p, sig)) { p->pending_sig = sig; pal_guest_resume(p->pid); return; }  /* blocked -> pend it */
    uint64_t h = p->sig_handler[sig];
    int dfl_ignore = (sig == 17 || sig == 18 || sig == 23 || sig == 28);   /* CHLD/CONT/URG/WINCH */
    if (h == AIOS_SIG_IGN || (h == AIOS_SIG_DFL && dfl_ignore)) { pal_guest_resume(p->pid); return; }
    if (is_stop_sig(sig) && h == AIOS_SIG_DFL) {                  /* default action of a stop signal: STOP */
        proc_stop(p, sig);                                       /* already at the signal-stop; just mark + notify */
        return;                                                  /* do NOT resume -- wait for SIGCONT */
    }
    if (h == AIOS_SIG_DFL) { pal_guest_exit(p->pid, 128 + sig); return; }   /* default: terminate */
    if (p->sig_tramp == 0) { pal_guest_exit(p->pid, 128 + sig); return; }
    p->in_handler = 1;
    pal_guest_deliver(p->pid, h, (uint64_t)sig, p->sig_tramp, p->sigsave);
}

/* A terminal signal (^C -> SIGINT, ^Z -> SIGTSTP) was caught by the kernel (the guests are off the
 * host pty's foreground group, so only the kernel receives it). Forward it to EVERY guest in the
 * FOREGROUND process group -- and to no one else (that is the whole point of job control: ^C kills
 * the foreground job, not the shell or background jobs). A RUNNING guest is interrupted with a host
 * signal so it stops and runs handle_signal_stop; a guest parked in a blocked syscall has that
 * syscall return EINTR with the signal delivered (kreturn). */
static void forward_terminal_signal(int sig) {
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *g = &g_proc[i];
        if (g->state == PS_FREE || g->state == PS_ZOMBIE || g->state == PS_STOPPED) continue;
        if (g->pgid != g_fg_pgrp) continue;
        /* Deliver entirely through the kernel's own pending-signal path -- never a host kill of a
         * tracee (a tracee stopped at a not-yet-serviced syscall would queue the signal, which the
         * setret/run-to-exit machinery then eats). A RUNNING guest takes the signal at its next
         * syscall (kreturn, or the do_read/do_write/do_wait entry check); a guest parked in a blocked
         * syscall has that syscall return EINTR with the signal delivered right now. */
        g->pending_sig = sig;
        if (g->state == PS_BLOCKED_WAIT || g->state == PS_BLOCKED_READ || g->state == PS_BLOCKED_WRITE) {
            g->state = PS_RUNNING;
            kreturn(g, (uint64_t)-AIOS_EINTR);
        }
    }
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

/* If p has a deliverable (unblocked) pending signal, return EINTR from the syscall it is at and run
 * the signal -- 1 if so. The "special" syscalls (read/write/wait) bypass kreturn, so they call this
 * at entry; otherwise a forwarded terminal signal (^C/^Z) would not reach a guest about to block. */
static int deliver_pending(proc_t *p) {
    int sig = p->pending_sig;
    if (!sig || sig_blocked(p, sig)) return 0;
    kreturn(p, (uint64_t)-AIOS_EINTR);
    return 1;
}

/* read: pipe read-ends are non-blocking (park on empty); everything else fills up to len. Owns its
 * own pal_guest_return / parking, so it is dispatched specially. */
static void do_read(proc_t *p, uint64_t fd, uint64_t gbuf, uint64_t len) {
    p->state = PS_RUNNING;
    if (deliver_pending(p)) return;                  /* a forwarded ^C/^Z interrupts the read */
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
    /* A single read (POSIX semantics): return what is available, do NOT loop to fill `len` -- a
     * terminal/pipe gives one line/chunk and looping would block forever waiting for more. */
    char tmp[4096];
    size_t chunk = len < sizeof tmp ? (size_t)len : sizeof tmp;
    long n = pal_host_read(g_ofile[oi].backing, tmp, chunk);
    if (n < 0) { pal_guest_return(p->pid, (uint64_t)n); return; }    /* -errno (incl. EINTR) */
    size_t put = (n > 0) ? pal_guest_write(p->pid, gbuf, tmp, (size_t)n) : 0;
    pal_guest_return(p->pid, (uint64_t)put);
}

/* write: pipe write-ends are non-blocking (park on full); everything else writes up to len. */
static void do_write(proc_t *p, uint64_t fd, uint64_t gbuf, uint64_t len) {
    p->state = PS_RUNNING;
    if (deliver_pending(p)) return;                  /* a forwarded ^C/^Z interrupts the write */
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
    child->wait_flags = 0;
    child->wait_status_gaddr = 0;
    child->stopped_sig = 0;
    child->report_stop = 0;
    child->report_cont = 0;
    for (int i = 0; i < AIOS_NSIG; i++) child->sig_handler[i] = parent->sig_handler[i];  /* inherited */
    child->sig_tramp = parent->sig_tramp;
    child->sig_mask = parent->sig_mask;                /* the signal mask is inherited across fork */
    child->pending_sig = 0;                            /* pending signals are NOT inherited */
    child->in_handler = 0;
    { size_t i = 0; for (; parent->cwd[i] && i < sizeof child->cwd - 1; i++) child->cwd[i] = parent->cwd[i];
      child->cwd[i] = '\0'; }                          /* cwd inherited across fork */
    child->umask = parent->umask;                      /* umask inherited across fork */
    child->pgid = parent->pgid;                        /* process group inherited across fork */
    for (int i = 0; i < AIOS_MAX_FD; i++) {
        child->fd[i] = parent->fd[i];
        if (parent->fd[i] >= 0) ofile_ref(parent->fd[i]);
    }
    pal_guest_resume(parent->pid);
    pal_guest_resume(cpid);
}

/* wait: report a matching child's state change -- an exit (reap), or (with WUNTRACED/WCONTINUED) a
 * stop/continue (no reap). If a matching child is still alive but has no event, PARK the caller until
 * one comes (WNOHANG -> return 0 instead). -ECHILD if it has no matching child at all. */
static void do_wait(proc_t *p, unsigned long want, uint64_t gstatus, unsigned long flags) {
    if (deliver_pending(p)) return;                  /* a forwarded ^C/^Z interrupts the wait */
    for (int i = 0; i < MAX_PROCS; i++) {             /* 1. an exited (zombie) child -> report + reap */
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
    if (flags & AIOS_WUNTRACED) for (int i = 0; i < MAX_PROCS; i++) {   /* 2. a newly-stopped child */
        proc_t *c = &g_proc[i];
        if (c->state == PS_STOPPED && c->report_stop && c->parent_pid == p->pid && wait_matches(want, c)) {
            if (gstatus) { int status = ((c->stopped_sig & 0xff) << 8) | 0x7f;
                           pal_guest_write(p->pid, gstatus, &status, sizeof status); }
            c->report_stop = 0;                       /* reported -- NOT reaped (it stays stopped) */
            pal_guest_return(p->pid, (uint64_t)c->pid);
            return;
        }
    }
    if (flags & AIOS_WCONTINUED) for (int i = 0; i < MAX_PROCS; i++) {  /* 3. a newly-continued child */
        proc_t *c = &g_proc[i];
        if (c->report_cont && c->parent_pid == p->pid && wait_matches(want, c)) {
            if (gstatus) { int status = 0xffff;
                           pal_guest_write(p->pid, gstatus, &status, sizeof status); }
            c->report_cont = 0;
            pal_guest_return(p->pid, (uint64_t)c->pid);
            return;
        }
    }
    for (int i = 0; i < MAX_PROCS; i++) {             /* 4. any matching child still alive -> park (or poll) */
        proc_t *c = &g_proc[i];
        if (c->state != PS_FREE && c->state != PS_ZOMBIE &&
            c->parent_pid == p->pid && wait_matches(want, c)) {
            if (flags & AIOS_WNOHANG) { pal_guest_return(p->pid, 0); return; }   /* nothing ready yet */
            p->state = PS_BLOCKED_WAIT;               /* park: a matching child is still alive */
            p->wait_for = want;
            p->wait_flags = flags;
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
    /* M4 boundary policy: every AIOS syscall number is >= AIOS_SYS_WRITE (0x1000). A lower number
     * means the guest emitted a real host (Linux) syscall, trying to bypass the kernel -- an escape
     * attempt. The PAL already neutralizes it (it never runs on the host); make the policy explicit
     * and loud: the guest dies. A well-behaved AIOS program never trips this. */
    if (sc->nr < AIOS_SYS_WRITE) {
        kputs("[aios-uk] SECURITY: guest pid ");
        kput_int((long)p->pid);
        kputs(" issued a non-AIOS (host) syscall nr=");
        kput_int((long)sc->nr);
        kputs(" -> escape attempt; killing the guest (boundary enforced)\n");
        pal_guest_exit(p->pid, 159);                                    /* 128 + 31 (SIGSYS-flavoured) */
        return;
    }
    switch (sc->nr) {
    case AIOS_SYS_MMAP: pal_guest_mmap(p->pid, (size_t)sc->arg[0]); pal_guest_resume(p->pid); return;
    case AIOS_SYS_EXEC: {                              /* resolve the program path against p->cwd first */
        char xrel[256], xabs[1536];
        long xe = read_path(p, sc->arg[0], xrel, sizeof xrel);
        if (xe == 0) {
            cwd_join(p, xrel, xabs, sizeof xabs);
            if (pal_guest_exec(p->pid, xabs, sc->arg[1], sc->arg[2]) == 0)
                for (int i = 1; i < AIOS_NSIG; i++)    /* caught signals reset to default across exec */
                    if (p->sig_handler[i] != AIOS_SIG_IGN) p->sig_handler[i] = AIOS_SIG_DFL;
        } else {
            pal_guest_setret(p->pid, (uint64_t)xe);    /* unreadable path pointer -> -EFAULT */
        }
        pal_guest_resume(p->pid); return;
    }
    case AIOS_SYS_FORK: do_fork(p);                                        return;
    case AIOS_SYS_WAIT: do_wait(p, (unsigned long)sc->arg[0], sc->arg[1], (unsigned long)sc->arg[2]); return;
    case AIOS_SYS_EXIT: pal_guest_exit(p->pid, (int)sc->arg[0]);           return;
    case AIOS_SYS_SIGRETURN: pal_guest_sigreturn(p->pid, p->sigsave); p->in_handler = 0; return;
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
    case AIOS_SYS_READLINK:ret = (uint64_t)sys_readlink(p, sc->arg[0], sc->arg[1], sc->arg[2]);          break;
    case AIOS_SYS_FCNTL:   ret = (uint64_t)sys_fcntl(p, sc->arg[0], sc->arg[1], sc->arg[2]);             break;
    case AIOS_SYS_ISATTY:  ret = (uint64_t)sys_isatty(p, sc->arg[0]);                                    break;
    case AIOS_SYS_CLOCK_GETTIME: ret = (uint64_t)sys_clock_gettime(p, sc->arg[0], sc->arg[1]);           break;
    case AIOS_SYS_FCHMODAT:  ret = (uint64_t)sys_fchmodat (p, sc->arg[0], sc->arg[1], sc->arg[2], sc->arg[3]);            break;
    case AIOS_SYS_FCHOWNAT:  ret = (uint64_t)sys_fchownat (p, sc->arg[0], sc->arg[1], sc->arg[2], sc->arg[3], sc->arg[4]); break;
    case AIOS_SYS_SYMLINKAT: ret = (uint64_t)sys_symlinkat(p, sc->arg[0], sc->arg[1], sc->arg[2]);                        break;
    case AIOS_SYS_LINKAT:    ret = (uint64_t)sys_linkat   (p, sc->arg[0], sc->arg[1], sc->arg[2], sc->arg[3], sc->arg[4]); break;
    case AIOS_SYS_UTIMENSAT: ret = (uint64_t)sys_utimensat(p, sc->arg[0], sc->arg[1], sc->arg[2], sc->arg[3]);            break;
    case AIOS_SYS_UMASK:     ret = (uint64_t)sys_umask(p, sc->arg[0]);                                                   break;
    case AIOS_SYS_SIGACTION:ret = (uint64_t)sys_sigaction(p, sc->arg[0], sc->arg[1], sc->arg[2]);        break;
    case AIOS_SYS_KILL:    ret = (uint64_t)sys_kill(p, sc->arg[0], sc->arg[1]);                          break;
    case AIOS_SYS_SETPGID:  ret = (uint64_t)sys_setpgid(p, sc->arg[0], sc->arg[1]);                      break;
    case AIOS_SYS_GETPGID:  ret = (uint64_t)sys_getpgid(p, sc->arg[0]);                                  break;
    case AIOS_SYS_TCSETPGRP:ret = (uint64_t)sys_tcsetpgrp(p, sc->arg[0], sc->arg[1]);                    break;
    case AIOS_SYS_TCGETPGRP:ret = (uint64_t)sys_tcgetpgrp(p, sc->arg[0]);                                break;
    case AIOS_SYS_SIGPROCMASK:ret = (uint64_t)sys_sigprocmask(p, sc->arg[0], sc->arg[1], sc->arg[2]);    break;
    default:             ret = (uint64_t)-AIOS_ENOSYS; /* unknown AIOS syscall */           break;
    }
    kreturn(p, ret);   /* return the result + deliver a pending signal (e.g. raise) at the syscall exit */
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
    init->wait_flags = 0;
    init->wait_status_gaddr = 0;
    init->stopped_sig = 0;
    init->report_stop = 0;
    init->report_cont = 0;
    sig_reset(init);
    /* seed init's cwd from the host (the tracer's startup dir unconfined, or "/" confined) */
    { char seed[1024]; long sn = pal_host_getcwd(seed, sizeof seed);
      size_t i = 0; if (sn > 0) for (; seed[i] && i < sizeof init->cwd - 1; i++) init->cwd[i] = seed[i];
      if (i == 0) { init->cwd[i++] = '/'; } init->cwd[i] = '\0'; }
    init->umask = 022;                                /* POSIX default file-creation mask */
    init->pgid = pid;                                 /* init is its own process-group leader */
    g_fg_pgrp = pid;                                  /* ... and starts in the terminal foreground */
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
        if (r == 3) {                                 /* a terminal signal (^C/^Z) -> the foreground group */
            forward_terminal_signal(code);
            continue;
        }
        if (r == 2) {                                 /* `who` got an async signal (code = signum) */
            if (p) handle_signal_stop(p, code); else pal_guest_resume(who);
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
