/*
 * AIOS Filesystem Server – VFS Layer
 *
 * Thin dispatcher that auto-detects and mounts the appropriate
 * filesystem backend, then routes IPC commands through it.
 *
 * Copyright (c) 2025 AIOS Project
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <microkit.h>
#include "aios/channels.h"
#include "aios/ipc.h"
#include "aios/vfs.h"

/* ── Memory regions ────────────────────────────────────── */
uintptr_t blk_data;
uintptr_t fs_data;

/* ── Open file table ───────────────────────────────────── */
static open_file_t open_files[MAX_OPEN_FILES];

/* ── Active filesystem ─────────────────────────────────── */
static const aios_fs_ops_t *active_fs;

/* ── Helpers ───────────────────────────────────────────── */
static void my_memcpy(void *dst, const void *src, int n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}
static void my_memset(void *dst, int c, int n) {
    uint8_t *d = (uint8_t *)dst;
    for (int i = 0; i < n; i++) d[i] = (uint8_t)c;
}

/* ── Block I/O via PPC to blk_driver ───────────────────── */
static void vfs_read_sector(uint32_t sector, uint8_t *buf) {

    WR32(blk_data, BLK_CMD, BLK_CMD_READ);
    WR32(blk_data, BLK_SECTOR, sector);
    WR32(blk_data, BLK_COUNT, 1);
    WR32(blk_data, BLK_STATUS, 0);     /* status = pending */
    microkit_notify(10);
    /* Poll for completion */
    while (RD32(blk_data, BLK_STATUS) == 0) {
        __asm__ volatile("yield" ::: "memory");
    }
    my_memcpy(buf, (void *)(blk_data + 512), 512);

}

static void vfs_read_sectors(uint32_t sector, uint32_t count, uint8_t *dst) {
    for (uint32_t i = 0; i < count; i++) {
        vfs_read_sector(sector + i, dst + i * 512);
    }
}

static void vfs_write_sector(uint32_t sector, const uint8_t *buf) {
    my_memcpy((void *)(blk_data + 512), buf, 512);
    WR32(blk_data, BLK_CMD, BLK_CMD_WRITE);
    WR32(blk_data, BLK_SECTOR, sector);
    WR32(blk_data, BLK_COUNT, 1);
    WR32(blk_data, BLK_STATUS, 0);     /* status = pending */
    microkit_notify(10);
    /* Poll for completion */
    while (RD32(blk_data, BLK_STATUS) == 0) {
        __asm__ volatile("yield" ::: "memory");
    }
}

static const blk_io_t vfs_blk = {
    .read_sector  = vfs_read_sector,
    .read_sectors = vfs_read_sectors,
    .write_sector = vfs_write_sector,
};

/* ── Reply helper ──────────────────────────────────────── */
static void reply_status(int status) {
    WR32(fs_data, FS_STATUS, (uint32_t)status);
}

/* ── Get filename from IPC buffer ──────────────────────── */
static void get_filename(char *buf, int max) {
    volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_FILENAME);
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) buf[i] = src[i];
    buf[i] = '\0';
}

/* ── Registered filesystem backends ────────────────────── */
extern const aios_fs_ops_t *fat16_probe(const blk_io_t *b);
extern const aios_fs_ops_t *fat32_probe(const blk_io_t *b);
extern const aios_fs_ops_t *ext2_probe(const blk_io_t *b);

static fs_probe_fn probes[] = {
    ext2_probe,
    fat32_probe,
    fat16_probe,
    0
};

/* ── Auto-detect and mount ─────────────────────────────── */
static int vfs_mount(void) {
    for (int i = 0; probes[i]; i++) {
        const aios_fs_ops_t *ops = probes[i](&vfs_blk);
        if (ops) {
            int rc = ops->mount(&vfs_blk);
            if (rc == 0) {
                active_fs = ops;
                microkit_dbg_puts("FS: mounted ");
                microkit_dbg_puts(ops->name);
                microkit_dbg_puts("\n");
                return 0;
            }
        }
    }
    microkit_dbg_puts("FS: no filesystem detected\n");
    return -1;
}

/* ═══════════════════════════════════════════════════════
 *  Command handlers — dispatch through active_fs
 * ═══════════════════════════════════════════════════════ */

static void handle_open(void) {
    char name[64];
    get_filename(name, sizeof(name));

    int fd_idx = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_files[i].in_use) { fd_idx = i; break; }
    }
    if (fd_idx < 0) { reply_status(-1); return; }

    int rc = active_fs->open(name, &open_files[fd_idx]);
    if (rc == 0) {
        WR32(fs_data, FS_FD, fd_idx);
        WR32(fs_data, FS_FILESIZE, open_files[fd_idx].file_size);
        reply_status(0);
    } else {
        reply_status(-1);
    }
}

static void handle_read(void) {
    uint32_t fd_idx = RD32(fs_data, FS_FD);
    uint32_t offset = RD32(fs_data, FS_OFFSET);
    uint32_t len    = RD32(fs_data, FS_LENGTH);

    if (fd_idx >= MAX_OPEN_FILES || !open_files[fd_idx].in_use) {
        reply_status(-1); return;
    }
    if (len > FS_DATA_MAX) len = FS_DATA_MAX;

    volatile uint8_t *dst = (volatile uint8_t *)(fs_data + FS_DATA);
    uint32_t bytes_read = 0;
    int rc = active_fs->read(&open_files[fd_idx], (uint8_t *)dst,
                             offset, len, &bytes_read);
    WR32(fs_data, FS_READLEN, bytes_read);
    WR32(fs_data, FS_LENGTH, bytes_read);
    reply_status(rc);
}

static void handle_close(void) {
    uint32_t fd_idx = RD32(fs_data, FS_FD);
    if (fd_idx >= MAX_OPEN_FILES || !open_files[fd_idx].in_use) {
        reply_status(-1); return;
    }
    active_fs->close(&open_files[fd_idx]);
    reply_status(0);
}

static void handle_create(void) {
    char name[64];
    get_filename(name, sizeof(name));

    int fd_idx = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_files[i].in_use) { fd_idx = i; break; }
    }
    if (fd_idx < 0) { reply_status(-1); return; }

    int rc = active_fs->create(name, &open_files[fd_idx]);
    if (rc == 0) {
        WR32(fs_data, FS_FD, fd_idx);
        WR32(fs_data, FS_FILESIZE, open_files[fd_idx].file_size);
        reply_status(0);
    } else {
        reply_status(-1);
    }
}

static void handle_write(void) {
    uint32_t fd_idx = RD32(fs_data, FS_FD);
    uint32_t len    = RD32(fs_data, FS_LENGTH);

    if (fd_idx >= MAX_OPEN_FILES || !open_files[fd_idx].in_use) {
        reply_status(-1); return;
    }

    volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
    uint32_t written = 0;
    int rc = active_fs->write(&open_files[fd_idx], (const uint8_t *)src,
                              len, &written);
    WR32(fs_data, FS_READLEN, written);
    WR32(fs_data, FS_LENGTH, written);
    reply_status(rc);
}

static void handle_delete(void) {
    char name[64];
    get_filename(name, sizeof(name));
    int rc = active_fs->delete(name);
    reply_status(rc);
}

static void handle_list(void) {
    volatile uint8_t *dst = (volatile uint8_t *)(fs_data + FS_DATA);
    uint32_t count = 0;
    int rc = active_fs->list((uint8_t *)dst, FS_DATA_MAX, &count);
    WR32(fs_data, FS_LENGTH, count);
    reply_status(rc);
}

static void handle_stat(void) {
    char name[64];
    get_filename(name, sizeof(name));
    uint32_t size = 0;
    int rc = active_fs->stat(name, &size);
    WR32(fs_data, FS_FILESIZE, size);
    reply_status(rc);
}

static void handle_sync(void) {
    int rc = active_fs->sync ? active_fs->sync() : 0;
    reply_status(rc);
}
static void handle_mkdir(void) {
    char name[64];
    get_filename(name, sizeof(name));
    int rc = active_fs->mkdir ? active_fs->mkdir(name) : -1;
    reply_status(rc);
}

static void handle_rmdir(void) {
    char name[64];
    get_filename(name, sizeof(name));
    int rc = active_fs->rmdir ? active_fs->rmdir(name) : -1;
    reply_status(rc);
}

static void handle_rename(void) {
    /* Filenames packed: old\0new\0 at FS_FILENAME */
    volatile char *p = (volatile char *)(fs_data + FS_FILENAME);
    char oldname[64], newname[64];
    int i = 0;
    while (p[i] && i < 63) { oldname[i] = p[i]; i++; }
    oldname[i] = '\0';
    i++; /* skip null */
    int j = 0;
    while (p[i] && j < 63) { newname[j] = p[i]; i++; j++; }
    newname[j] = '\0';
    int rc = active_fs->rename ? active_fs->rename(oldname, newname) : -1;
    reply_status(rc);
}


/* ═══════════════════════════════════════════════════════
 *  Microkit entry points
 * ═══════════════════════════════════════════════════════ */

void init(void) {
    my_memset(open_files, 0, sizeof(open_files));
    microkit_dbg_puts("FS: probing filesystems...\n");
    if (vfs_mount() != 0) {
        microkit_dbg_puts("FS: MOUNT FAILED\n");
    }
}

void notified(microkit_channel ch) {
    (void)ch;
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch; (void)msginfo;

    if (!active_fs) { reply_status(-1); return microkit_msginfo_new(0, 0); }

    uint32_t cmd = RD32(fs_data, FS_CMD);
    switch (cmd) {
    case FS_CMD_OPEN:   handle_open();   break;
    case FS_CMD_READ:   handle_read();   break;
    case FS_CMD_CLOSE:  handle_close();  break;
    case FS_CMD_CREATE: handle_create(); break;
    case FS_CMD_WRITE:  handle_write();  break;
    case FS_CMD_DELETE: handle_delete(); break;
    case FS_CMD_LIST:   handle_list();   break;
    case FS_CMD_STAT:   handle_stat();   break;
    case FS_CMD_FSINFO: {
        /* Return filesystem name */
        const char *fsname = active_fs ? active_fs->name : "none";
        char *dst = (char *)(fs_data + FS_FILENAME);
        int i = 0;
        while (fsname[i] && i < 63) { dst[i] = fsname[i]; i++; }
        dst[i] = 0;
        WR32(fs_data, FS_STATUS, 0);
        break;
    }
    case FS_CMD_SYNC:   handle_sync();   break;
    case FS_CMD_MKDIR:  handle_mkdir();  break;
    case FS_CMD_RMDIR:  handle_rmdir();  break;
    case FS_CMD_RENAME: handle_rename(); break;
    default: reply_status(-1); break;
    }

    return microkit_msginfo_new(0, 0);
}
