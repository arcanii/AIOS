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

/* v0.4.99: Send full termios to tty_server via TCSETS IPC */
static void termios_send(void) {
    seL4_SetMR(0, (seL4_Word)TTY_IOCTL_TCSETS);
    seL4_SetMR(1, (seL4_Word)aios_termios.c_iflag);
    seL4_SetMR(2, (seL4_Word)aios_termios.c_oflag);
    seL4_SetMR(3, (seL4_Word)aios_termios.c_cflag);
    seL4_SetMR(4, (seL4_Word)aios_termios.c_lflag);
    /* Pack c_cc[0..19] into 3 MRs (8 bytes per MR) */
    for (int m = 0; m < 3; m++) {
        seL4_Word w = 0;
        for (int b = 0; b < 8 && (m * 8 + b) < 20; b++) {
            w |= ((seL4_Word)aios_termios.c_cc[m * 8 + b]) << (b * 8);
        }
        seL4_SetMR(5 + m, w);
    }
    seL4_Call(ser_ep, seL4_MessageInfo_new(TTY_IOCTL, 0, 0, 8));
}

/* v0.4.99: Fetch full termios from tty_server via TCGETS IPC */
static void termios_fetch(void) {
    seL4_SetMR(0, (seL4_Word)TTY_IOCTL_TCGETS);
    seL4_MessageInfo_t reply = seL4_Call(ser_ep,
        seL4_MessageInfo_new(TTY_IOCTL, 0, 0, 1));
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

    /* v0.4.99: Check if this fd is a tty (fd 0-2 or duped tty fd) */
    int is_tty_fd = (fd <= 2);
    if (!is_tty_fd && fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *af = &aios_fds[fd - AIOS_FD_BASE];
        if (af->active && af->is_tty) is_tty_fd = 1;
    }

    if (is_tty_fd) {
        /* TIOCGWINSZ -- terminal size for isatty + dash */
        if (req == 0x5413 && argp) {
            unsigned short *ws = (unsigned short *)argp;
            ws[0] = 24; ws[1] = 80; ws[2] = 0; ws[3] = 0;
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
            termios_fetch();
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
            seL4_SetMR(0, (seL4_Word)op);
            seL4_SetMR(1, (seL4_Word)aios_termios.c_iflag);
            seL4_SetMR(2, (seL4_Word)aios_termios.c_oflag);
            seL4_SetMR(3, (seL4_Word)aios_termios.c_cflag);
            seL4_SetMR(4, (seL4_Word)aios_termios.c_lflag);
            for (int m = 0; m < 3; m++) {
                seL4_Word w = 0;
                for (int b = 0; b < 8 && (m * 8 + b) < 20; b++) {
                    w |= ((seL4_Word)aios_termios.c_cc[m * 8 + b]) << (b * 8);
                }
                seL4_SetMR(5 + m, w);
            }
            seL4_Call(ser_ep, seL4_MessageInfo_new(TTY_IOCTL, 0, 0, 8));
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

/* v0.4.62: mprotect -- change memory protection
 * Stub: seL4 page-table updates not yet wired through VMM. */
long aios_sys_mprotect(va_list ap) {
    void *addr = va_arg(ap, void *);
    size_t len = va_arg(ap, size_t);
    int prot = va_arg(ap, int);
    (void)addr; (void)len; (void)prot;
    return 0;
}
