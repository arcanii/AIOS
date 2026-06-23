/*
 * posix_misc.c -- AIOS POSIX miscellaneous syscall handlers
 * utimensat, umask, uname, ioctl, fcntl, dup, dup3, pipe2
 */
#include "posix_internal.h"
#include <termios.h>
#include "aios/tty.h"

/* Local termios state -- reflects tty_server RAW/COOKED/ECHO state.
 * TCGETS returns this. TCSETS updates this and sends TTY_IOCTLs. */
static struct termios aios_termios = {
    .c_iflag = ICRNL,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | CREAD | CLOCAL | B9600,
    .c_lflag = ECHO | ICANON | ISIG,
    .c_cc = {
        [VINTR]  = 0x03,   /* Ctrl-C */
        [VQUIT]  = 0x1C,   /* Ctrl-backslash */
        [VERASE] = 0x7F,   /* DEL */
        [VKILL]  = 0x15,   /* Ctrl-U */
        [VEOF]   = 0x04,   /* Ctrl-D */
        [VMIN]   = 1,
        [VTIME]  = 0,
        [VSUSP]  = 0x1A,   /* Ctrl-Z */
        [VWERASE]= 0x17,   /* Ctrl-W */
    },
};

/* v0.4.99: Send full termios to tty_server. v0.4.296: instance-aware -- inst 0 uses
 * the serial-console TTY_IOCTL (MR0=op); a PTY (inst>0) uses TTY_PTY_SLAVE_IOCTL with
 * MR0=inst, MR1=op, so the termios words shift one MR right. */
static void termios_send(int op, int inst) {
    int base = (inst > 0) ? 2 : 1;
    if (inst > 0) { seL4_SetMR(0, (seL4_Word)inst); seL4_SetMR(1, (seL4_Word)op); }
    else          { seL4_SetMR(0, (seL4_Word)op); }
    seL4_SetMR(base + 0, (seL4_Word)aios_termios.c_iflag);
    seL4_SetMR(base + 1, (seL4_Word)aios_termios.c_oflag);
    seL4_SetMR(base + 2, (seL4_Word)aios_termios.c_cflag);
    seL4_SetMR(base + 3, (seL4_Word)aios_termios.c_lflag);
    /* Pack c_cc[0..19] into 3 MRs (8 bytes per MR) */
    for (int m = 0; m < 3; m++) {
        seL4_Word w = 0;
        for (int b = 0; b < 8 && (m * 8 + b) < 20; b++) {
            w |= ((seL4_Word)aios_termios.c_cc[m * 8 + b]) << (b * 8);
        }
        seL4_SetMR(base + 4 + m, w);
    }
    int label = (inst > 0) ? TTY_PTY_SLAVE_IOCTL : TTY_IOCTL;
    seL4_Call(ser_ep, seL4_MessageInfo_new(label, 0, 0, base + 7));
}

/* v0.4.99: Fetch full termios from tty_server via TCGETS IPC (instance-aware).
 * The reply layout (termios packed at MR0..6) is identical for both ops. */
static void termios_fetch(int inst) {
    seL4_MessageInfo_t reply;
    if (inst > 0) {
        seL4_SetMR(0, (seL4_Word)inst);
        seL4_SetMR(1, (seL4_Word)TTY_IOCTL_TCGETS);
        reply = seL4_Call(ser_ep, seL4_MessageInfo_new(TTY_PTY_SLAVE_IOCTL, 0, 0, 2));
    } else {
        seL4_SetMR(0, (seL4_Word)TTY_IOCTL_TCGETS);
        reply = seL4_Call(ser_ep, seL4_MessageInfo_new(TTY_IOCTL, 0, 0, 1));
    }
    int nlen = (int)seL4_MessageInfo_get_length(reply);
    if (nlen >= 7) {
        aios_termios.c_iflag = (tcflag_t)seL4_GetMR(0);
        aios_termios.c_oflag = (tcflag_t)seL4_GetMR(1);
        aios_termios.c_cflag = (tcflag_t)seL4_GetMR(2);
        aios_termios.c_lflag = (tcflag_t)seL4_GetMR(3);
        for (int m = 0; m < 3; m++) {
            seL4_Word w = seL4_GetMR(4 + m);
            for (int b = 0; b < 8 && (m * 8 + b) < 20; b++) {
                aios_termios.c_cc[m * 8 + b] = (cc_t)((w >> (b * 8)) & 0xFF);
            }
        }
    }
}

long aios_sys_utimensat(va_list ap) {
    /* Stub — ignore timestamps for now */
    (void)ap;
    return 0;
}

long aios_sys_umask(va_list ap) {
    int mask = va_arg(ap, int);
    (void)mask;
    return 022; /* return old umask */
}

long aios_sys_uname(va_list ap) {
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
        /* New protocol (>=12 MRs): version is 32 bytes split across MR6-7 +
         * MR10-11 (the full kernel build timestamp); the other 4 fields are
         * 16 bytes / 2 MRs each. MR0-1 sysname, MR2-3 nodename, MR4-5 release,
         * MR8-9 machine. (Server keeps MR0-9 backward compatible.) */
        int nlen = (int)seL4_MessageInfo_get_length(ur);
        if (nlen >= 12) {
            const int base[4] = { 0, 2, 4, 8 };
            char *dst[4] = { buf->sysname, buf->nodename, buf->release, buf->machine };
            for (int f = 0; f < 4; f++)
                for (int m = 0; m < 2; m++) {
                    seL4_Word w = seL4_GetMR(base[f] + m);
                    for (int j = 0; j < 8; j++) dst[f][m*8 + j] = (char)((w >> (j*8)) & 0xFF);
                }
            const int vmr[4] = { 6, 7, 10, 11 };
            for (int m = 0; m < 4; m++) {
                seL4_Word w = seL4_GetMR(vmr[m]);
                for (int j = 0; j < 8; j++) buf->version[m*8 + j] = (char)((w >> (j*8)) & 0xFF);
            }
            return 0;
        }
    }
    /* Fallback */
    s = "0.4.80";
    for (int i = 0; s[i]; i++) buf->release[i] = s[i];
    s = "seL4 15.0.0 SMP";
    for (int i = 0; s[i]; i++) buf->version[i] = s[i];
    s = "aarch64";
    for (int i = 0; s[i]; i++) buf->machine[i] = s[i];
    return 0;
}

long aios_sys_ioctl(va_list ap) {
    int fd = va_arg(ap, int);
    int req = va_arg(ap, int);
    void *argp = va_arg(ap, void *);

    /* v0.4.99: Check if this fd is a tty (fd 0-2 or duped tty fd).
     * v0.4.296: also resolve the controlling PTY instance -- fd 0/1/2 inherit the
     * process global; a duped tty fd carries its own tty_inst. inst 0 = serial. */
    int is_tty_fd = (fd <= 2);
    int inst = (fd <= 2) ? aios_tty_inst : 0;
    if (!is_tty_fd && fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *af = &aios_fds[fd - AIOS_FD_BASE];
        if (af->active && af->is_tty) { is_tty_fd = 1; inst = af->tty_inst; }
    }

    if (is_tty_fd) {
        /* TIOCGWINSZ -- terminal size for isatty + dash. A PTY returns the real
         * window size set by the SSH client (so vi/less size correctly); serial
         * keeps the fixed 24x80. */
        if (req == 0x5413 && argp) {
            unsigned short *ws = (unsigned short *)argp;
            if (inst > 0 && ser_ep) {
                seL4_SetMR(0, (seL4_Word)inst);
                seL4_SetMR(1, (seL4_Word)TTY_IOCTL_GET_WINSZ);
                seL4_Call(ser_ep, seL4_MessageInfo_new(TTY_PTY_SLAVE_IOCTL, 0, 0, 2));
                ws[0] = (unsigned short)seL4_GetMR(0);  /* rows */
                ws[1] = (unsigned short)seL4_GetMR(1);  /* cols */
            } else {
                ws[0] = 24; ws[1] = 80;
            }
            ws[2] = 0; ws[3] = 0;
            return 0;
        }
        /* TIOCGPGRP -- foreground process group */
        if (req == 0x540F && argp) {
            *(int *)argp = aios_pid > 0 ? aios_pid : 1;
            return 0;
        }
        /* TIOCSPGRP -- set fg pgrp (stub) */
        if (req == 0x5410) return 0;
        /* TCGETS -- get terminal attributes from server */
        if (req == 0x5401 && argp) {
            termios_fetch(inst);
            struct termios *t = (struct termios *)argp;
            *t = aios_termios;
            return 0;
        }
        /* TCSETS / TCSETSW / TCSETSF -- set terminal attributes */
        if ((req == 0x5402 || req == 0x5403 || req == 0x5404) && argp) {
            struct termios *t = (struct termios *)argp;
            aios_termios = *t;
            int op = TTY_IOCTL_TCSETS;
            if (req == 0x5403) op = TTY_IOCTL_TCSETSW;
            if (req == 0x5404) op = TTY_IOCTL_TCSETSF;
            termios_send(op, inst);
            return 0;
        }
        return 0;
    }
    /* Pipe fds: FIONREAD for poll support */
    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *af = &aios_fds[fd - AIOS_FD_BASE];
        if (af->active && !af->is_pipe) return -ENOTTY;
        return -ENOTTY;
    }
    return -ENOTTY;
}

long aios_sys_fcntl(va_list ap) {
    int fd = va_arg(ap, int);
    int cmd = va_arg(ap, int);
    /* v0.4.64: F_DUPFD / F_DUPFD_CLOEXEC for dash fd management */
    if (cmd == 0 || cmd == 1030) {
        va_arg(ap, int); /* minfd -- next available aios fd */
        if (fd < 3) {
            int idx = aios_fd_alloc();
            if (idx < 0) return -EMFILE;
            aios_fds[idx].active = 1;
            aios_fds[idx].is_devnull = 0;
            aios_fds[idx].is_tty = 1;  /* REDIR_DUP_V072: mark as terminal copy */
            aios_fds[idx].tty_inst = aios_tty_inst;  /* v0.4.296: inherit PTY instance */
            aios_fds[idx].size = 0;
            aios_fds[idx].pos = 0;
            return AIOS_FD_BASE + idx;
        }
        if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
            aios_fd_t *src = &aios_fds[fd - AIOS_FD_BASE];
            if (!src->active) return -EBADF;
            int idx = aios_fd_alloc();
            if (idx < 0) return -EMFILE;
            aios_fds[idx] = *src;
            return AIOS_FD_BASE + idx;
        }
        return -EBADF;
    }
    if (cmd == 1) return 0;  /* F_GETFD */
    if (cmd == 2) return 0;  /* F_SETFD */
    if (cmd == 3) {  /* F_GETFL */
        int fl = 2;  /* O_RDWR base */
        if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
            aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
            if (f->active && f->is_nonblock) fl |= 0x800;
            if (f->active && f->is_append) fl |= 0x400;
        }
        return fl;
    }
    if (cmd == 4) {  /* F_SETFL */
        int newfl = va_arg(ap, int);
        if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
            aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
            if (f->active) f->is_nonblock = (newfl & 0x800) ? 1 : 0;
        }
        return 0;
    }
    return -EINVAL;
}

long aios_sys_dup(va_list ap) {
    int oldfd = va_arg(ap, int);
    if (oldfd < 3) {
        /* dup stdin/stdout/stderr -- allocate new aios fd pointing to same */
        int idx = aios_fd_alloc();
        if (idx < 0) return -EMFILE;
        aios_fds[idx].active = 1;
        aios_fds[idx].is_tty = 1;  /* v0.4.72: mark as terminal copy */
        aios_fds[idx].tty_inst = aios_tty_inst;  /* v0.4.296: inherit PTY instance */
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

long aios_sys_dup3(va_list ap) {
    int oldfd = va_arg(ap, int);
    int newfd = va_arg(ap, int);
    int flags = va_arg(ap, int);
    (void)flags;

    /* v0.4.64: dup2(fd, fd) returns fd per POSIX */
    if (oldfd == newfd) {
        if (flags != 0) return -EINVAL;
        return newfd;
    }

    /* pipe fd -> stdin redirect: dup2(pipe_read_fd, 0) */
    if (newfd == 0 && oldfd >= AIOS_FD_BASE && oldfd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *src = &aios_fds[oldfd - AIOS_FD_BASE];
        if (!src->active) return -EBADF;
        if (src->is_pipe && src->pipe_read) {
            stdin_pipe_id = src->pipe_id;
            /* v0.4.67: notify server (see stdout comment above) */
            if (pipe_ep) {
                seL4_SetMR(0, (seL4_Word)stdout_pipe_id);
                seL4_SetMR(1, (seL4_Word)stdin_pipe_id);
                seL4_Call(pipe_ep,
                    seL4_MessageInfo_new(81 /* PIPE_SET_PIPES */, 0, 0, 2));
            }
            return 0;
        }
    }

    /* v0.4.72: dup2(aios_fd, stdout/stderr) -- redirect or restore */
    if ((newfd == 1 || newfd == 2) && oldfd >= AIOS_FD_BASE && oldfd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *src = &aios_fds[oldfd - AIOS_FD_BASE];
        if (!src->active) return -EBADF;
        /* Terminal restore: saved fd from dup(1) / fcntl(1,F_DUPFD) */
        if (src->is_tty) {
            if (newfd == 1) { stdout_redir_idx = -1; stdout_pipe_id = -1; }
            if (newfd == 2) stderr_redir_idx = -1;
            return newfd;
        }
        /* Pipe redirect */
        if (src->is_pipe && !src->pipe_read) {
            stdout_pipe_id = src->pipe_id;
            if (pipe_ep) {
                seL4_SetMR(0, (seL4_Word)stdout_pipe_id);
                seL4_SetMR(1, (seL4_Word)stdin_pipe_id);
                seL4_Call(pipe_ep,
                    seL4_MessageInfo_new(81 /* PIPE_SET_PIPES */, 0, 0, 2));
            }
            return newfd;
        }
        /* File redirect: copy fd state so close cannot invalidate REDIR_COPY_V072 */
        if (newfd == 1) { stdout_redir_copy = *src; stdout_redir_idx = 0; }
        else { stderr_redir_copy = *src; stderr_redir_idx = 0; }
        return newfd;
    }

    /* aios fd -> aios fd copy */
    if (oldfd >= AIOS_FD_BASE && oldfd < AIOS_FD_BASE + AIOS_MAX_FDS
        && newfd >= AIOS_FD_BASE && newfd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *src = &aios_fds[oldfd - AIOS_FD_BASE];
        if (!src->active) return -EBADF;
        aios_fd_t *dst = &aios_fds[newfd - AIOS_FD_BASE];
        *dst = *src;
        return newfd;
    }

    /* stdin/stdout/stderr -> aios fd */
    if (oldfd < 3) {
        if (newfd >= AIOS_FD_BASE && newfd < AIOS_FD_BASE + AIOS_MAX_FDS) {
            aios_fds[newfd - AIOS_FD_BASE].active = 1;
            aios_fds[newfd - AIOS_FD_BASE].size = 0;
            aios_fds[newfd - AIOS_FD_BASE].pos = 0;
            return newfd;
        }
        return newfd;
    }

    return -EINVAL;
}

long aios_sys_pipe2(va_list ap) {
    int *fds = va_arg(ap, int *);
    int flags = va_arg(ap, int);
    if (!pipe_ep) return -ENOSYS;

    /* Create pipe via IPC */
    seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
        seL4_MessageInfo_new(60 /* PIPE_CREATE */, 0, 0, 0));
    int pipe_id = (int)(long)seL4_GetMR(0);
    if (pipe_id < 0) return -ENOMEM;

    /* Allocate two fds: read end and write end */
    int ri = aios_fd_alloc();
    if (ri < 0) return -EMFILE;
    /* Mark active immediately so second alloc gets different slot */
    aios_fds[ri].active = 1;
    int wi = aios_fd_alloc();
    if (wi < 0) { aios_fds[ri].active = 0; return -EMFILE; }

    aios_fds[ri].is_pipe = 1;
    aios_fds[ri].pipe_id = pipe_id;
    aios_fds[ri].pipe_read = 1;
    aios_fds[ri].shm_vaddr = 0;

    aios_fds[wi].active = 1;
    aios_fds[wi].is_pipe = 1;
    aios_fds[wi].pipe_id = pipe_id;
    aios_fds[wi].pipe_read = 0;
    aios_fds[wi].shm_vaddr = 0;

    /* v0.4.79: propagate O_NONBLOCK to both ends */
    int nb = (flags & 0x800) ? 1 : 0;
    aios_fds[ri].is_nonblock = nb;
    aios_fds[wi].is_nonblock = nb;

    fds[0] = AIOS_FD_BASE + ri;  /* read end */
    fds[1] = AIOS_FD_BASE + wi;  /* write end */
    return 0;
}

/* v0.4.144: file-backed mmap registry (architecture A -- eager client-side).
 * mmap(MAP_SHARED/MAP_PRIVATE, fd, off) reads the file region into fresh
 * anonymous pages (PIPE_MMAP_ANON) at map time; msync/munmap write a
 * MAP_SHARED + writable region back via FS_PWRITE. Single-process semantics:
 * two processes mapping the same file get independent frames. Write-back rides
 * the path-based FS_PWRITE, so a MAP_SHARED + PROT_WRITE map persists even
 * though AIOS open() is read-only -- mmap write permission is deliberately
 * decoupled from the fd open mode here. */
#ifndef MAP_SHARED
#define MAP_SHARED  0x01
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0x02
#endif
#ifndef PROT_WRITE
#define PROT_WRITE  0x2
#endif

#define AIOS_MAX_FILE_MMAPS 32   /* v0.4.295: 16->32 -- anon demand-maps now share this registry */
typedef struct {
    int       active;
    uintptr_t vaddr;     /* page-aligned base returned to caller */
    size_t    length;    /* requested mapping length in bytes */
    size_t    pages;     /* ceil(length / 4096) */
    long      offset;    /* page-aligned file offset */
    int       flags;     /* MAP_SHARED / MAP_PRIVATE */
    int       prot;      /* PROT_* -- gates write-back */
    char      path[128]; /* resolved file path */
} aios_file_mmap_t;
static aios_file_mmap_t aios_file_mmaps[AIOS_MAX_FILE_MMAPS];

static aios_file_mmap_t *file_mmap_find(uintptr_t va) {
    for (int i = 0; i < AIOS_MAX_FILE_MMAPS; i++) {
        aios_file_mmap_t *m = &aios_file_mmaps[i];
        if (m->active && va >= m->vaddr
            && va < m->vaddr + m->pages * 4096) return m;
    }
    return NULL;
}

/* Write a MAP_SHARED + writable mapping back to its file, clamped to the file
 * current size so we never extend it with trailing zero bytes. Returns 0 on
 * success or when there is nothing to do, -1 on a write error. */
static int file_mmap_writeback(aios_file_mmap_t *m) {
    if (!(m->flags & MAP_SHARED) || !(m->prot & PROT_WRITE)) return 0;
    if (!m->path[0] || !fs_ep_cap || !pipe_ep) return 0;
    uint32_t fmode = 0, fsize = 0;
    if (fetch_stat(m->path, &fmode, &fsize) != 0) return -1;
    long avail = (long)fsize - m->offset;
    if (avail <= 0) return 0;
    size_t wlen = m->length;
    if ((long)wlen > avail) wlen = (size_t)avail;
    /* C: hand the whole clamped region to the server, which maps each child
     * page and writes it back with vfs_pwrite. MR0=vaddr, MR1=len, MR2=offset,
     * MR3=path_len, MR4+=path. */
    int pl = str_len(m->path);
    seL4_SetMR(0, (seL4_Word)m->vaddr);
    seL4_SetMR(1, (seL4_Word)wlen);
    seL4_SetMR(2, (seL4_Word)m->offset);
    seL4_SetMR(3, (seL4_Word)pl);
    int mr = 4;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)m->path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_Call(pipe_ep, seL4_MessageInfo_new(87 /* PIPE_MSYNC */, 0, 0, mr));
    return ((long)seL4_GetMR(0) < 0) ? -1 : 0;
}

/* v0.4.128: munmap via PIPE_MUNMAP_ANON IPC. Frees the pages in the
 * caller's vspace (cookie tracking deletes the underlying frame too).
 * Pages not currently mapped are silently skipped. Returns 0 on success
 * or -EINVAL for bogus inputs.
 * v0.4.144: a file-backed mapping flushes MAP_SHARED writes, then frees the
 * whole mapping and drops its registry entry. */
long aios_sys_munmap(va_list ap) {
    void *addr = va_arg(ap, void *);
    size_t len = va_arg(ap, size_t);
    if (len == 0) return 0;
    uintptr_t va = (uintptr_t)addr;
    if (va & 0xFFF) return -EINVAL;
    size_t pages = (len + 4095) / 4096;
    if (!pipe_ep) return -ENOSYS;
    aios_file_mmap_t *m = file_mmap_find(va);
    if (m && va == m->vaddr) {
        /* B: flush MAP_SHARED writes, then have the server free resident pages,
         * release the reservation, and drop its descriptor. */
        file_mmap_writeback(m);
        seL4_SetMR(0, (seL4_Word)va);
        seL4_SetMR(1, (seL4_Word)m->pages);
        seL4_Call(pipe_ep, seL4_MessageInfo_new(88 /* PIPE_MUNMAP_FILE */, 0, 0, 2));
        m->active = 0;
        return (long)seL4_GetMR(0);
    }
    seL4_SetMR(0, (seL4_Word)va);
    seL4_SetMR(1, (seL4_Word)pages);
    seL4_Call(pipe_ep, seL4_MessageInfo_new(84 /* PIPE_MUNMAP_ANON */, 0, 0, 2));
    return (long)seL4_GetMR(0);
}

/* v0.4.126/127: mprotect via PIPE_MPROTECT IPC. Server walks the caller's
 * vspace and calls seL4_ARM_Page_Map per page with the requested rights.
 * Supports PROT_NONE (rights cleared, accesses fault), PROT_READ,
 * PROT_WRITE, and PROT_EXEC (controls the ARM Execute-Never attribute).
 * Pages not currently mapped are skipped silently -- mprotect on
 * demand-paged BSS only takes effect on pages that have been faulted in. */
long aios_sys_mprotect(va_list ap) {
    void *addr = va_arg(ap, void *);
    size_t len = va_arg(ap, size_t);
    int prot = va_arg(ap, int);
    if (len == 0) return 0;
    uintptr_t va = (uintptr_t)addr;
    if (va & 0xFFF) return -EINVAL;
    size_t pages = (len + 4095) / 4096;
    if (!pipe_ep) return -ENOSYS;
    seL4_SetMR(0, (seL4_Word)va);
    seL4_SetMR(1, (seL4_Word)pages);
    seL4_SetMR(2, (seL4_Word)prot);
    seL4_Call(pipe_ep, seL4_MessageInfo_new(85 /* PIPE_MPROTECT */, 0, 0, 3));
    long rc = (long)seL4_GetMR(0);
    return rc;
}

/* v0.4.104: mmap MAP_ANONYMOUS with on-demand allocation via IPC.
 * v0.4.144: file-backed mmap (architecture A -- eager client-side).
 *
 * For MAP_ANONYMOUS, request fresh pages from pipe_server (root) which
 * allocates frames and maps them in our VSpace. This avoids stealing
 * from the static morecore_area (which is eagerly mapped at ELF load).
 *
 * For a file-backed mapping, allocate the same anonymous pages and then read
 * the file region [offset, offset+length) into them with FS_PREAD. Bytes past
 * EOF stay zero (fresh frames are zero-filled). The mapping is recorded so
 * msync/munmap can write a MAP_SHARED + writable region back. MAP_PRIVATE and
 * read-only maps never write back.
 *
 * Returns vaddr on success, negative errno on failure. */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
long aios_sys_mmap(va_list ap) {
    void *addr = va_arg(ap, void *);
    size_t length = va_arg(ap, size_t);
    int prot = va_arg(ap, int);
    int flags = va_arg(ap, int);
    int fd = va_arg(ap, int);
    long offset = va_arg(ap, long);
    (void)addr;

    if (length == 0) return -EINVAL;
    if (!pipe_ep) {
        /* No pipe_ep yet (very early init) -- caller can fall back to morecore */
        return -ENOMEM;
    }

    /* Round length up to whole pages */
    size_t pages = (length + 4095) / 4096;
    if (pages == 0) pages = 1;
    /* v0.4.295: file-backed caps at 1024 pages (server PIPE_MMAP_FILE); anonymous
     * is now DEMAND-PAGED up to 65536 pages = 256 MB (PIPE_MMAP_ANON_LAZY), so the
     * old 4 MB eager cap no longer blocks tcc/musl large heaps. */
    if (!(flags & MAP_ANONYMOUS) && pages > 1024) return -ENOMEM;
    if (pages > 65536) return -ENOMEM;

    if (!(flags & MAP_ANONYMOUS)) {
        /* File-backed: validate the fd, grab anonymous pages, fill from disk. */
        if (offset & 0xFFF) return -EINVAL;   /* offset must be page-aligned */
        if (fd < AIOS_FD_BASE || fd >= AIOS_FD_BASE + AIOS_MAX_FDS) return -EBADF;
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        if (!f->active || f->is_pipe || f->is_dir || f->is_socket || !f->path[0])
            return -EACCES;
        /* Reserve a registry slot up front so we never leak pages. */
        int slot = -1;
        for (int i = 0; i < AIOS_MAX_FILE_MMAPS; i++)
            if (!aios_file_mmaps[i].active) { slot = i; break; }
        if (slot < 0) return -ENOMEM;

        /* C: the server allocates the pages and fills them from the file with
         * vfs_pread (root-side), so the client never round-trips the data.
         * MR0=pages, MR1=offset, MR2=path_len, MR3+=path. */
        int pl = str_len(f->path);
        seL4_SetMR(0, (seL4_Word)pages);
        seL4_SetMR(1, (seL4_Word)offset);
        seL4_SetMR(2, (seL4_Word)pl);
        int mr = 3;
        seL4_Word w = 0;
        for (int i = 0; i < pl; i++) {
            w |= ((seL4_Word)(uint8_t)f->path[i]) << ((i % 8) * 8);
            if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
        }
        seL4_Call(pipe_ep, seL4_MessageInfo_new(86 /* PIPE_MMAP_FILE */, 0, 0, mr));
        uintptr_t vaddr = (uintptr_t)seL4_GetMR(0);
        if (vaddr == 0) return -ENOMEM;

        aios_file_mmap_t *m = &aios_file_mmaps[slot];
        m->active = 1;
        m->vaddr  = vaddr;
        m->length = length;
        m->pages  = pages;
        m->offset = offset;
        m->flags  = flags;
        m->prot   = prot;
        str_copy(m->path, f->path, sizeof(m->path));
        return (long)vaddr;
    }

    /* MAP_ANONYMOUS path -- v0.4.295: DEMAND-PAGED via PIPE_MMAP_ANON_LAZY.
     * Reserve the VA range (no frames); each page faults in zero-filled on first
     * touch (handle_file_mmap_fault, anon branch). Registered in the file_mmap
     * registry so munmap routes to PIPE_MUNMAP_FILE (releases the reservation);
     * writeback no-ops for anon (empty path). Lifts the old 4 MB eager cap. */
    {
        int slot = -1;
        for (int i = 0; i < AIOS_MAX_FILE_MMAPS; i++)
            if (!aios_file_mmaps[i].active) { slot = i; break; }
        if (slot < 0) return -ENOMEM;
        seL4_SetMR(0, (seL4_Word)pages);
        seL4_Call(pipe_ep, seL4_MessageInfo_new(93 /* PIPE_MMAP_ANON_LAZY */, 0, 0, 1));
        uintptr_t vaddr = (uintptr_t)seL4_GetMR(0);
        if (vaddr == 0) return -ENOMEM;
        aios_file_mmap_t *m = &aios_file_mmaps[slot];
        m->active = 1;
        m->vaddr  = vaddr;
        m->length = length;
        m->pages  = pages;
        m->offset = 0;
        m->flags  = flags;
        m->prot   = prot;
        m->path[0] = 0;
        return (long)vaddr;
    }
}

/* v0.4.144: msync -- write a MAP_SHARED + writable file mapping back to disk.
 * MAP_PRIVATE, read-only, and anonymous mappings are no-ops. Whole-region
 * write-back (no per-page dirty tracking in v1); the (addr, length) sub-range
 * is treated as a flush of the whole containing mapping. */
long aios_sys_msync(va_list ap) {
    void *addr = va_arg(ap, void *);
    size_t length = va_arg(ap, size_t);
    int flags = va_arg(ap, int);
    (void)length; (void)flags;
    aios_file_mmap_t *m = file_mmap_find((uintptr_t)addr);
    if (!m) return 0;          /* not a tracked file mapping -- nothing to sync */
    if (file_mmap_writeback(m) != 0) return -EIO;
    return 0;
}
