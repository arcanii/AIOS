/*
 * AIOS Virtual Filesystem Interface
 *
 * Each filesystem backend implements this ops table.
 * The fs_server dispatches IPC commands through whichever
 * backend is mounted.
 */
#ifndef AIOS_VFS_H
#define AIOS_VFS_H

#include <stdint.h>

/* ── Forward declarations ─────────────────────────────── */
typedef struct aios_fs_ops aios_fs_ops_t;

/* ── Open file descriptor (shared across all backends) ── */
#define MAX_OPEN_FILES 8

typedef struct {
    int      in_use;
    uint16_t start_cluster;
    uint32_t file_size;
    uint32_t offset;
    uint8_t  dir_sector;     /* which root-dir sector holds this entry */
    uint8_t  dir_index;      /* index within that sector               */
    uint8_t  name83[11];     /* cached 8.3 name for write-back         */
} open_file_t;

/* ── Block I/O interface (provided by fs_server to backends) ── */
typedef struct {
    void (*read_sector)(uint32_t sector, uint8_t *buf);
    void (*read_sectors)(uint32_t sector, uint32_t count, uint8_t *dst);
    void (*write_sector)(uint32_t sector, const uint8_t *buf);
} blk_io_t;

/* ── Filesystem operations vtable ─────────────────────── */
struct aios_fs_ops {
    const char *name;        /* "FAT16", "FAT32", etc.                */

    /* Mount: parse superblock/BPB, return 0 on success              */
    int (*mount)(const blk_io_t *blk);

    /* File operations                                                */
    int (*open)(const char *filename, open_file_t *fd);
    int (*read)(open_file_t *fd, uint8_t *buf, uint32_t offset,
                uint32_t len, uint32_t *bytes_read);
    int (*close)(open_file_t *fd);
    int (*create)(const char *filename, open_file_t *fd);
    int (*write)(open_file_t *fd, const uint8_t *data,
                 uint32_t len, uint32_t *bytes_written);
    int (*delete)(const char *filename);

    /* Directory operations                                           */
    int (*list)(uint8_t *buf, uint32_t buf_size, uint32_t *count);

    /* Optional: sync/flush                                           */
    int (*sync)(void);

    /* Optional: stat                                                 */
    int (*stat)(const char *filename, uint32_t *size_out);
};

/* ── Filesystem registration ──────────────────────────── */
#define MAX_FS_BACKENDS 4

/* Probe function: given blk I/O, return non-NULL ops if recognized */
typedef const aios_fs_ops_t *(*fs_probe_fn)(const blk_io_t *blk);

#endif /* AIOS_VFS_H */
