/*
 * posix_stat.c -- AIOS POSIX stat/access syscall handlers
 * fstat, fstatat, access, faccessat
 */
#include "posix_internal.h"

/* djb2 hash -- deterministic pseudo-inode from path string */
static uint32_t path_to_ino(const char *path) {
    uint32_t h = 5381;
    while (*path) {
        h = ((h << 5) + h) + (uint8_t)*path;
        path++;
    }
    if (h == 0) h = 1;  /* reserve 0 for unused */
    return h;
}

long aios_sys_fstat(va_list ap) {
    int fd = va_arg(ap, int);
    struct stat *st = va_arg(ap, struct stat *);

    /* Zero the struct */
    char *p = (char *)st;
    for (int i = 0; i < (int)sizeof(struct stat); i++) p[i] = 0;

    /* stdout/stderr/stdin */
    if (fd < 3) {
        st->st_dev = 2;  /* separate device for ttys */
        st->st_ino = (ino_t)(fd + 1);
        st->st_mode = 0020666; /* char device */
        st->st_blksize = 4096;
        return 0;
    }

    /* aios fds */
    if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
        aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
        if (!f->active) return -EBADF;
        st->st_dev = 1;
        st->st_ino = path_to_ino(f->path);
        st->st_mode = 0100644; /* regular file */
        st->st_size = f->size;
        st->st_blksize = 4096;
        return 0;
    }
    return -EBADF;
}

long aios_sys_fstatat(va_list ap) {
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

    st->st_dev = 1;
    st->st_ino = path_to_ino(lookup);
    st->st_mode = mode;
    st->st_size = size;
    st->st_blksize = 4096;
    st->st_nlink = 1;
    return 0;
}

long aios_sys_access(va_list ap) {
    const char *pathname = va_arg(ap, const char *);
    int mode = va_arg(ap, int);
    (void)mode;
    uint32_t m, s;
    if (fetch_stat(pathname, &m, &s) == 0) return 0;
    return -ENOENT;
}

long aios_sys_faccessat(va_list ap) {
    int dirfd = va_arg(ap, int);
    const char *pathname = va_arg(ap, const char *);
    int mode = va_arg(ap, int);
    (void)dirfd; (void)mode;
    uint32_t m, s;
    if (fetch_stat(pathname, &m, &s) == 0) return 0;
    return -ENOENT;
}
