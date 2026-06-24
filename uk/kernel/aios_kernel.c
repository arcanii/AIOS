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
typedef struct { pal_file_t backing; int refcount; int permanent; } ofile_t;
static ofile_t g_ofile[MAX_OFILES];

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
            g_ofile[i].backing = backing; g_ofile[i].refcount = 1; return i;
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
enum { PS_FREE = 0, PS_RUNNING, PS_ZOMBIE, PS_BLOCKED_WAIT };
typedef struct {
    int           state;
    pal_pid_t     pid;
    pal_pid_t     parent_pid;          /* PAL_PID_NONE for init / orphans */
    int           exit_code;           /* valid in PS_ZOMBIE */
    unsigned long wait_for;            /* PS_BLOCKED_WAIT: AIOS_WAIT_ANY/0, or a specific pid */
    uint64_t      wait_status_gaddr;   /* PS_BLOCKED_WAIT: where to store the status (0 = none) */
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

/* ---- AIOS file syscalls (host-agnostic; reach the host only via the PAL) ---- */

static long sys_open(proc_t *p, uint64_t gpath, uint64_t flags, uint64_t mode) {
    char path[256];
    size_t n = pal_guest_read(p->pid, gpath, path, sizeof path - 1);
    if (n == 0) return -1;
    path[n] = '\0';                                  /* backstop terminator */
    pal_file_t f = pal_host_open(path, flags, mode);
    if (f == PAL_FILE_INVALID) return -1;
    int oi = ofile_alloc(f);
    if (oi < 0) { pal_host_close(f); return -1; }    /* open-file table full */
    int fd = fd_alloc(p);
    if (fd < 0) { ofile_unref(oi); return -1; }      /* fd table full (unref closes f) */
    p->fd[fd] = oi;
    return fd;
}

static long sys_read(proc_t *p, uint64_t fd, uint64_t gbuf, uint64_t len) {
    if (!fd_valid(p, fd)) return -1;
    char tmp[1024];
    size_t total = 0;
    while (total < len) {
        size_t chunk = (size_t)len - total;
        if (chunk > sizeof tmp) chunk = sizeof tmp;
        long n = pal_host_read(fd_backing(p, fd), tmp, chunk);
        if (n < 0)  return total ? (long)total : -1;
        if (n == 0) break;                           /* EOF */
        size_t put = pal_guest_write(p->pid, gbuf + total, tmp, (size_t)n);
        total += put;
        if (put < (size_t)n) break;                  /* guest buffer not fully writable */
    }
    return (long)total;
}

static long sys_write(proc_t *p, uint64_t fd, uint64_t gbuf, uint64_t len) {
    if (!fd_valid(p, fd)) return -1;
    char tmp[1024];
    size_t total = 0;
    while (total < len) {
        size_t chunk = (size_t)len - total;
        if (chunk > sizeof tmp) chunk = sizeof tmp;
        size_t got = pal_guest_read(p->pid, gbuf + total, tmp, chunk);
        if (got == 0) break;
        long w = pal_host_write(fd_backing(p, fd), tmp, got);
        if (w < 0) return total ? (long)total : -1;
        total += (size_t)w;
        if ((size_t)w < got) break;
    }
    return (long)total;
}

static long sys_close(proc_t *p, uint64_t fd) {
    if (!fd_valid(p, fd)) return -1;
    ofile_unref(p->fd[fd]);                          /* releases the backing iff last ref */
    p->fd[fd] = -1;
    return 0;
}

static long sys_lseek(proc_t *p, uint64_t fd, uint64_t off, uint64_t whence) {
    if (!fd_valid(p, fd)) return -1;
    return (long)pal_host_lseek(fd_backing(p, fd), (long long)off, (int)whence);
}

static long sys_fstat(proc_t *p, uint64_t fd, uint64_t gstat) {
    if (!fd_valid(p, fd)) return -1;
    struct aios_stat s;
    s._pad = 0;
    if (pal_host_fstat(fd_backing(p, fd), &s.size, &s.mode) != 0) return -1;
    if (pal_guest_write(p->pid, gstat, &s, sizeof s) != sizeof s) return -1;
    return 0;
}

/* ---- the process syscalls ---- */

static int wait_matches(unsigned long want, const proc_t *child) {
    return want == AIOS_WAIT_ANY || want == 0 || want == (unsigned long)child->pid;
}

/* fork: COPY the parent's fd table into a fresh child (refcounting shared backings), then let the
 * PAL place each side's return value (child pid in the parent, 0 in the child) and resume both. */
static void do_fork(proc_t *parent) {
    proc_t *child = proc_alloc();
    if (!child) { pal_guest_return(parent->pid, (uint64_t)-1); return; }   /* too many procs */
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
    pal_guest_return(p->pid, (uint64_t)-1);           /* ECHILD: no such child */
}

/* A guest exited. First detach its own children (free zombies, orphan the living). Then either
 * wake a parent parked in wait (delivering the status + reaping), or become a zombie for a running
 * parent to reap later, or -- no parent -- free outright. */
static void on_exit(proc_t *p, int code) {
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
    case AIOS_SYS_FORK: do_fork(p);                                       return;
    case AIOS_SYS_WAIT: do_wait(p, (unsigned long)sc->arg[0], sc->arg[1]); return;
    case AIOS_SYS_EXIT: pal_guest_exit(p->pid, (int)sc->arg[0]);          return;
    }
    uint64_t ret;
    switch (sc->nr) {
    case AIOS_SYS_WRITE: ret = (uint64_t)sys_write(p, sc->arg[0], sc->arg[1], sc->arg[2]); break;
    case AIOS_SYS_READ:  ret = (uint64_t)sys_read (p, sc->arg[0], sc->arg[1], sc->arg[2]); break;
    case AIOS_SYS_OPEN:  ret = (uint64_t)sys_open (p, sc->arg[0], sc->arg[1], sc->arg[2]); break;
    case AIOS_SYS_CLOSE: ret = (uint64_t)sys_close(p, sc->arg[0]);                         break;
    case AIOS_SYS_LSEEK: ret = (uint64_t)sys_lseek(p, sc->arg[0], sc->arg[1], sc->arg[2]); break;
    case AIOS_SYS_FSTAT: ret = (uint64_t)sys_fstat(p, sc->arg[0], sc->arg[1]);             break;
    default:             ret = (uint64_t)-1;       /* unknown AIOS syscall */              break;
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

    kputs("[aios-uk] AIOS userspace kernel -- M3d (process model: fork/exec/wait) (Linux/ptrace PAL)\n");
    kputs("[aios-uk] launching guest: ");
    kputs(guest);
    kputs("\n");

    int code = aios_kernel_run(guest, guest_argv);

    kputs("[aios-uk] init guest exited via AIOS ABI, code=");
    kput_int(code);
    kputs("\n");
    return code;
}
