/*
 * posix_misc.c -- AIOS POSIX miscellaneous syscall handlers
 * utimensat, umask, uname, ioctl, fcntl, dup, dup3, pipe2
 */
#include "posix_internal.h"

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
    s = "0.4.37";
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
    if (fd <= 2) {
        /* v0.4.64: TIOCGWINSZ -- terminal size for isatty + dash */
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
        return 0;
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
    if (cmd == 3) return 2;  /* F_GETFL: O_RDWR */
    if (cmd == 4) return 0;  /* F_SETFL */
    return -EINVAL;
}

long aios_sys_dup(va_list ap) {
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
            return 0;
        }
    }

    /* pipe fd -> stdout redirect: dup2(pipe_write_fd, 1) */
    if ((newfd == 1 || newfd == 2) && oldfd >= AIOS_FD_BASE && oldfd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *src = &aios_fds[oldfd - AIOS_FD_BASE];
        if (!src->active) return -EBADF;
        if (src->is_pipe && !src->pipe_read) {
            stdout_pipe_id = src->pipe_id;
            return newfd;
        }
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
    /* Mark active immediately so second alloc gets different slot */
    aios_fds[ri].active = 1;
    int wi = aios_fd_alloc();
    if (wi < 0) { aios_fds[ri].active = 0; return -EMFILE; }

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

/* v0.4.62: mprotect -- change memory protection
 * Stub: seL4 page-table updates not yet wired through VMM. */
long aios_sys_mprotect(va_list ap) {
    void *addr = va_arg(ap, void *);
    size_t len = va_arg(ap, size_t);
    int prot = va_arg(ap, int);
    (void)addr; (void)len; (void)prot;
    return 0;
}
