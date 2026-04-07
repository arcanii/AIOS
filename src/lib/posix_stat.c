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
    (void)flags;

    /* AT_FDCWD (-100) = resolve relative to CWD (handled below).
     * Real dirfd = resolve relative to that open directory. */
    if (dirfd != -100 && pathname[0] != '/') {
        /* Look up directory path from fd table */
        if (dirfd >= AIOS_FD_BASE && dirfd < AIOS_FD_BASE + AIOS_MAX_FDS) {
            aios_fd_t *df = &aios_fds[dirfd - AIOS_FD_BASE];
            if (df->active && df->is_dir) {
                char dirpath[256];
                int di = 0;
                while (df->path[di] && di < 250) {
                    dirpath[di] = df->path[di]; di++;
                }
                if (di > 1) dirpath[di++] = '/';
                const char *s = pathname;
                while (*s && di < 255) dirpath[di++] = *s++;
                dirpath[di] = 0;
                uint32_t mode, size;
                if (fetch_stat(dirpath, &mode, &size) != 0)
                    return -ENOENT;
                char *p = (char *)st;
                for (int i = 0; i < (int)sizeof(struct stat); i++)
                    p[i] = 0;
                st->st_dev = 1;
                st->st_ino = path_to_ino(dirpath);
                st->st_mode = mode;
                st->st_size = size;
                st->st_blksize = 4096;
                st->st_nlink = 1;
                return 0;
            }
        }
    }

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

/* statx -- full implementation for musl AArch64 (no fstatat fallback) */
long aios_sys_statx(va_list ap) {
    int dirfd = va_arg(ap, int);
    const char *pathname = va_arg(ap, const char *);
    int flags = va_arg(ap, int);
    unsigned int mask = va_arg(ap, unsigned int);
    void *buf = va_arg(ap, void *);
    (void)flags; (void)mask;

    if (!buf) return -EINVAL;

    /* Handle AT_EMPTY_PATH: pathname is empty, dirfd is the target */
    if (pathname && pathname[0] == 0 && dirfd >= 0) {
        /* stat by fd -- handle stdin/stdout/stderr */
        uint8_t *stx = (uint8_t *)buf;
        for (int i = 0; i < 256; i++) stx[i] = 0;
        if (dirfd < 3) {
            uint32_t m = 0x07FF;
            stx[0]=m&0xFF; stx[1]=(m>>8)&0xFF;
            stx[4]=0; stx[5]=0x10; /* blksize=4096 */
            stx[16]=1; /* nlink */
            stx[28]=0x36; stx[29]=0x10; /* mode=020666 */
            uint32_t ino = dirfd + 1;
            stx[32]=ino&0xFF;
            stx[136]=2; /* dev_major=2 (tty) */
            return 0;
        }
        if (dirfd >= AIOS_FD_BASE && dirfd < AIOS_FD_BASE + AIOS_MAX_FDS) {
            aios_fd_t *f = &aios_fds[dirfd - AIOS_FD_BASE];
            if (!f->active) return -EBADF;
            uint32_t m = 0x07FF;
            stx[0]=m&0xFF; stx[1]=(m>>8)&0xFF;
            stx[4]=0; stx[5]=0x10;
            stx[16]=1;
            uint16_t mode = f->is_dir ? 0x41ED : 0x81A4;
            stx[28]=mode&0xFF; stx[29]=(mode>>8)&0xFF;
            uint32_t ino = path_to_ino(f->path);
            stx[32]=ino&0xFF; stx[33]=(ino>>8)&0xFF;
            stx[34]=(ino>>16)&0xFF; stx[35]=(ino>>24)&0xFF;
            uint32_t sz = (uint32_t)f->size;
            stx[40]=sz&0xFF; stx[41]=(sz>>8)&0xFF;
            stx[42]=(sz>>16)&0xFF; stx[43]=(sz>>24)&0xFF;
            stx[136]=1; /* dev=1 (ext2) */
            return 0;
        }
        return -EBADF;
    }

    if (!pathname) return -EINVAL;

    /* Resolve path */
    char resolved[256];
    const char *lookup = pathname;
    if (pathname[0] != '/') {
        /* Check for real dirfd (not AT_FDCWD) */
        if (dirfd != -100 && dirfd >= AIOS_FD_BASE
            && dirfd < AIOS_FD_BASE + AIOS_MAX_FDS) {
            aios_fd_t *df = &aios_fds[dirfd - AIOS_FD_BASE];
            if (df->active && df->is_dir) {
                int di = 0;
                while (df->path[di] && di < 250) {
                    resolved[di] = df->path[di]; di++;
                }
                if (di > 1) resolved[di++] = '/';
                const char *s = pathname;
                while (*s && di < 255) resolved[di++] = *s++;
                resolved[di] = 0;
                lookup = resolved;
            } else {
                resolve_path(pathname, resolved, sizeof(resolved));
                lookup = resolved;
            }
        } else {
            resolve_path(pathname, resolved, sizeof(resolved));
            lookup = resolved;
        }
    }

    uint32_t mode, size;
    if (fetch_stat(lookup, &mode, &size) != 0) return -ENOENT;

    /* Fill struct statx at ABI byte offsets */
    uint8_t *stx = (uint8_t *)buf;
    for (int i = 0; i < 256; i++) stx[i] = 0;
    /* stx_mask = STATX_BASIC_STATS (0x07FF) */
    uint32_t sm = 0x07FF;
    stx[0]=sm&0xFF; stx[1]=(sm>>8)&0xFF;
    /* stx_blksize = 4096 */
    stx[4]=0; stx[5]=0x10;
    /* stx_nlink = 1 */
    stx[16]=1;
    /* stx_uid */
    stx[20]=aios_uid&0xFF; stx[21]=(aios_uid>>8)&0xFF;
    /* stx_gid */
    stx[24]=aios_gid&0xFF; stx[25]=(aios_gid>>8)&0xFF;
    /* stx_mode (u16 at offset 28) */
    stx[28]=mode&0xFF; stx[29]=(mode>>8)&0xFF;
    /* stx_ino (u64 at offset 32) */
    uint32_t ino = path_to_ino(lookup);
    stx[32]=ino&0xFF; stx[33]=(ino>>8)&0xFF;
    stx[34]=(ino>>16)&0xFF; stx[35]=(ino>>24)&0xFF;
    /* stx_size (u64 at offset 40) */
    stx[40]=size&0xFF; stx[41]=(size>>8)&0xFF;
    stx[42]=(size>>16)&0xFF; stx[43]=(size>>24)&0xFF;
    /* stx_blocks (u64 at offset 48) */
    uint32_t blks = (size + 511) / 512;
    stx[48]=blks&0xFF; stx[49]=(blks>>8)&0xFF;
    /* stx_dev_major (offset 136) = 1 */
    stx[136]=1;

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
