/*
 * posix_dir.c -- AIOS POSIX directory syscall handlers
 * mkdirat, unlinkat, chdir, getcwd, getdents64, renameat
 */
#include "posix_internal.h"

long aios_sys_mkdirat(va_list ap) {
    int dirfd = va_arg(ap, int);
    const char *pathname = va_arg(ap, const char *);
    int mode = va_arg(ap, int);
    (void)dirfd; (void)mode;
    if (!fs_ep_cap) return -ENOSYS;

    /* Resolve relative path */
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
    seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap,
        seL4_MessageInfo_new(14 /* FS_MKDIR */, 0, 0, mr));
    return ((int)(long)seL4_GetMR(0) == 0) ? 0 : -EIO;
}

long aios_sys_unlinkat(va_list ap) {
    int dirfd = va_arg(ap, int);
    const char *pathname = va_arg(ap, const char *);
    int flags = va_arg(ap, int);
    (void)dirfd; (void)flags;
    if (!fs_ep_cap) return -ENOSYS;

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
    seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap,
        seL4_MessageInfo_new(16 /* FS_UNLINK */, 0, 0, mr));
    return ((int)(long)seL4_GetMR(0) == 0) ? 0 : -EIO;
}

long aios_sys_chdir(va_list ap) {
    const char *path = va_arg(ap, const char *);
    if (!path) return -ENOENT;
    if (path[0] == '.' && path[1] == 0)
        return 0;  /* chdir(".") is a no-op */

    /* Resolve to absolute path */
    char resolved[256];
    if (path[0] == '/') {
        int i = 0;
        while (path[i] && i < 255) { resolved[i] = path[i]; i++; }
        resolved[i] = 0;
    } else {
        int ci = 0;
        while (aios_cwd[ci] && ci < 250) { resolved[ci] = aios_cwd[ci]; ci++; }
        if (ci > 1) resolved[ci++] = '/';
        while (*path && ci < 255) resolved[ci++] = *path++;
        resolved[ci] = 0;
    }

    /* Validate: must exist and be a directory */
    uint32_t mode = 0, size = 0;
    if (fetch_stat(resolved, &mode, &size) != 0)
        return -ENOENT;
    if ((mode & 0170000) != 0040000)   /* S_IFMT / S_IFDIR */
        return -ENOTDIR;

    aios_set_cwd(resolved);
    return 0;
}

long aios_sys_getcwd(va_list ap) {
    char *buf = va_arg(ap, char *);
    size_t size = va_arg(ap, size_t);
    int len = str_len(aios_cwd);
    if ((int)size <= len) return -ERANGE;
    for (int i = 0; i <= len; i++) buf[i] = aios_cwd[i];
    return (long)buf;
}

long aios_sys_getdents64(va_list ap) {
    int fd = va_arg(ap, int);
    void *dirp = va_arg(ap, void *);
    size_t count = va_arg(ap, size_t);

    if (fd < AIOS_FD_BASE || fd >= AIOS_FD_BASE + AIOS_MAX_FDS)
        return -EBADF;

    aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
    if (!f->active || !f->is_dir) return -ENOTDIR;

    int avail = f->size - f->pos;
    if (avail <= 0) return 0;

    /* Copy dirent records that fit */
    int copied = 0;
    char *out = (char *)dirp;
    while (f->pos < f->size && copied + 24 <= (int)count) {
        /* Read reclen from current position */
        uint16_t reclen = (uint8_t)f->data[f->pos + 16] | ((uint8_t)f->data[f->pos + 17] << 8);
        if (reclen == 0 || copied + reclen > (int)count) break;
        for (int i = 0; i < reclen; i++) out[copied + i] = f->data[f->pos + i];
        f->pos += reclen;
        copied += reclen;
    }
    return copied;
}

long aios_sys_renameat(va_list ap) {
    int olddirfd = va_arg(ap, int);
    const char *oldpath = va_arg(ap, const char *);
    int newdirfd = va_arg(ap, int);
    const char *newpath = va_arg(ap, const char *);
    (void)olddirfd; (void)newdirfd;
    if (!fs_ep_cap || !oldpath || !newpath) return -ENOSYS;

    /* Resolve paths */
    char old_full[256], new_full[256];
    if (oldpath[0] != '/') {
        int i = 0;
        const char *c = aios_cwd;
        while (*c && i < 250) old_full[i++] = *c++;
        if (i > 1) old_full[i++] = '/';
        while (*oldpath && i < 255) old_full[i++] = *oldpath++;
        old_full[i] = '\0';
    } else {
        int i = 0; while (*oldpath && i < 255) old_full[i++] = *oldpath++; old_full[i] = '\0';
    }
    if (newpath[0] != '/') {
        int i = 0;
        const char *c = aios_cwd;
        while (*c && i < 250) new_full[i++] = *c++;
        if (i > 1) new_full[i++] = '/';
        while (*newpath && i < 255) new_full[i++] = *newpath++;
        new_full[i] = '\0';
    } else {
        int i = 0; while (*newpath && i < 255) new_full[i++] = *newpath++; new_full[i] = '\0';
    }

    /* Pack both paths: MR0=oldlen, MR1..=old, then newlen, new.. */
    int olen = 0; while (old_full[olen]) olen++;
    int nlen = 0; while (new_full[nlen]) nlen++;
    seL4_SetMR(0, (seL4_Word)olen);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < olen; i++) {
        w |= ((seL4_Word)(uint8_t)old_full[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == olen - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_SetMR(mr++, (seL4_Word)nlen);
    w = 0;
    for (int i = 0; i < nlen; i++) {
        w |= ((seL4_Word)(uint8_t)new_full[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == nlen - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap, seL4_MessageInfo_new(18 /* FS_RENAME */, 0, 0, mr));
    long result = (long)seL4_GetMR(0);
    return result;
}
