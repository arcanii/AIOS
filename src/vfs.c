#include "aios/vfs.h"
#include <stdio.h>

static mount_entry_t mounts[VFS_MAX_MOUNTS];
static int mount_count = 0;

static int str_len(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

void vfs_init(void) {
    mount_count = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) mounts[i].active = 0;
}

int vfs_mount(const char *path, fs_ops_t *ops, void *ctx) {
    if (mount_count >= VFS_MAX_MOUNTS) return -1;
    mount_entry_t *m = &mounts[mount_count++];
    m->path = path;
    m->path_len = str_len(path);
    m->ops = ops;
    m->ctx = ctx;
    m->active = 1;
    printf("[vfs] Mounted %s\n", path);
    return 0;
}

/* Find the longest matching mount for a path */
static mount_entry_t *find_mount(const char *path, const char **remainder) {
    mount_entry_t *best = 0;
    int best_len = 0;

    for (int i = 0; i < mount_count; i++) {
        mount_entry_t *m = &mounts[i];
        if (!m->active) continue;

        /* Check if path starts with mount path */
        if (m->path_len == 1 && m->path[0] == '/') {
            /* Root mount — always matches, lowest priority */
            if (!best || m->path_len > best_len) {
                best = m;
                best_len = m->path_len;
            }
            continue;
        }

        int match = 1;
        for (int j = 0; j < m->path_len; j++) {
            if (!path[j] || path[j] != m->path[j]) { match = 0; break; }
        }
        /* Must match fully: /proc matches /proc and /proc/1, not /process */
        if (match && (path[m->path_len] == '/' || path[m->path_len] == '\0')) {
            if (m->path_len > best_len) {
                best = m;
                best_len = m->path_len;
            }
        }
    }

    if (best && remainder) {
        const char *r = path + best->path_len;
        if (*r == '/') r++;
        *remainder = r;
    }
    return best;
}

int vfs_list(const char *path, char *buf, int bufsize) {
    const char *remainder;
    mount_entry_t *m = find_mount(path, &remainder);
    if (!m || !m->ops->fs_list) return -1;

    /* For root mount, pass original path.
     * For sub-mounts, resolve and list within that fs */
    if (m->path_len == 1) {
        /* Root mount — resolve to inode and list */
        uint32_t ino = 2; /* root inode */
        if (path[0] == '/' && path[1] != '\0') {
            if (m->ops->fs_resolve && m->ops->fs_resolve(m->ctx, path, &ino) != 0)
                return -1;
        }
        return m->ops->fs_list(m->ctx, ino, buf, bufsize);
    } else {
        /* Sub-mount — remainder is the path within this fs */
        uint32_t ino = 2;
        if (remainder[0] != '\0') {
            if (m->ops->fs_resolve && m->ops->fs_resolve(m->ctx, remainder, &ino) != 0)
                return -1;
        }
        return m->ops->fs_list(m->ctx, ino, buf, bufsize);
    }
}

int vfs_read(const char *path, char *buf, int bufsize) {
    const char *remainder;
    mount_entry_t *m = find_mount(path, &remainder);
    if (!m || !m->ops->fs_read) return -1;

    if (m->path_len == 1) {
        return m->ops->fs_read(m->ctx, path, buf, bufsize);
    } else {
        /* Pass remainder path within sub-mount */
        char sub_path[256];
        sub_path[0] = '/';
        int i = 1;
        while (remainder[i-1] && i < 255) { sub_path[i] = remainder[i-1]; i++; }
        sub_path[i] = '\0';
        return m->ops->fs_read(m->ctx, sub_path, buf, bufsize);
    }
}

int vfs_stat(const char *path, uint32_t *mode, uint32_t *size) {
    const char *remainder;
    mount_entry_t *m = find_mount(path, &remainder);
    if (!m || !m->ops->fs_stat) return -1;

    if (m->path_len == 1) {
        return m->ops->fs_stat(m->ctx, path, mode, size);
    } else {
        char sub_path[256];
        sub_path[0] = '/';
        int i = 1;
        while (remainder[i-1] && i < 255) { sub_path[i] = remainder[i-1]; i++; }
        sub_path[i] = '\0';
        return m->ops->fs_stat(m->ctx, sub_path, mode, size);
    }
}

int vfs_mkdir(const char *path) {
    const char *remainder;
    mount_entry_t *m = find_mount(path, &remainder);
    if (!m || !m->ops->fs_mkdir) return -1;
    if (m->path_len == 1) return m->ops->fs_mkdir(m->ctx, path);
    char sub[256]; sub[0] = '/'; int i = 1;
    while (remainder[i-1] && i < 255) { sub[i] = remainder[i-1]; i++; }
    sub[i] = '\0';
    return m->ops->fs_mkdir(m->ctx, sub);
}

int vfs_create(const char *path, const void *data, int len) {
    const char *remainder;
    mount_entry_t *m = find_mount(path, &remainder);
    if (!m || !m->ops->fs_create) return -1;
    if (m->path_len == 1) return m->ops->fs_create(m->ctx, path, data, len);
    char sub[256]; sub[0] = '/'; int i = 1;
    while (remainder[i-1] && i < 255) { sub[i] = remainder[i-1]; i++; }
    sub[i] = '\0';
    return m->ops->fs_create(m->ctx, sub, data, len);
}

int vfs_unlink(const char *path) {
    const char *remainder;
    mount_entry_t *m = find_mount(path, &remainder);
    if (!m || !m->ops->fs_unlink) return -1;
    if (m->path_len == 1) return m->ops->fs_unlink(m->ctx, path);
    char sub[256]; sub[0] = '/'; int i = 1;
    while (remainder[i-1] && i < 255) { sub[i] = remainder[i-1]; i++; }
    sub[i] = '\0';
    return m->ops->fs_unlink(m->ctx, sub);
}
