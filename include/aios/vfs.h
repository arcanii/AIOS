#ifndef AIOS_VFS_H
#define AIOS_VFS_H

#include <stdint.h>

/* Filesystem operations — each mounted fs implements these */
typedef struct {
    int (*fs_list)(void *ctx, uint32_t dir_ino, char *buf, int bufsize);
    int (*fs_read)(void *ctx, const char *path, char *buf, int bufsize);
    int (*fs_stat)(void *ctx, const char *path, uint32_t *mode, uint32_t *size);
    int (*fs_resolve)(void *ctx, const char *path, uint32_t *ino);
    int (*fs_mkdir)(void *ctx, const char *path);
    int (*fs_create)(void *ctx, const char *path, const void *data, int len);
    int (*fs_pread)(void *ctx, const char *path, int offset, char *buf, int bufsize);
    int (*fs_pwrite)(void *ctx, const char *path, int offset, const void *data, int len);
    int (*fs_unlink)(void *ctx, const char *path);
} fs_ops_t;

/* Mount entry */
typedef struct {
    const char *path;       /* mount point: "/", "/proc", "/dev" */
    int         path_len;
    fs_ops_t   *ops;
    void       *ctx;        /* filesystem-specific context (ext2_ctx_t*, etc.) */
    int         active;
} mount_entry_t;

#define VFS_MAX_MOUNTS 8

/* Initialize VFS */
void vfs_init(void);

/* Mount a filesystem at path */
int vfs_mount(const char *path, fs_ops_t *ops, void *ctx);

/* VFS operations — resolve mount, dispatch to correct fs */
int vfs_list(const char *path, char *buf, int bufsize);
int vfs_read(const char *path, char *buf, int bufsize);
int vfs_stat(const char *path, uint32_t *mode, uint32_t *size);
int vfs_pread(const char *path, int offset, char *buf, int bufsize);
int vfs_pwrite(const char *path, int offset, const void *data, int len);
int vfs_mkdir(const char *path);
int vfs_create(const char *path, const void *data, int len);
int vfs_unlink(const char *path);
int vfs_rename(const char *oldpath, const char *newpath);

#endif
