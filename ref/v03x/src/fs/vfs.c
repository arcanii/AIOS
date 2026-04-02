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
#include "aios/util.h"

/* ── Memory regions ────────────────────────────────────── */
uintptr_t blk_data;
uintptr_t fs_data;

/* ── Logging backend ─────────────────────────────────── */
#define LOG_MODULE "FS"
#define LOG_LEVEL  LOG_LEVEL_INFO
#include "aios/log.h"

void _log_puts(const char *s) { (void)s; /* FS logs via orchestrator */ }
void _log_put_dec(unsigned long n) { (void)n; }
void _log_flush(void) { }
unsigned long _log_get_time(void) { return 0; }



/* ── Open file table ───────────────────────────────────── */
static open_file_t open_files[MAX_OPEN_FILES];

/* ── Active filesystem ─────────────────────────────────── */
static const aios_fs_ops_t *active_fs;


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
    /* VFS_BOUNDS_CHECK: safe filename extraction with guaranteed NUL */
    volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_FILENAME);
    int i;
    if (max <= 0) return;
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

    /* VFS_BOUNDS_CHECK: validate fd and clamp length */
    if (fd_idx >= MAX_OPEN_FILES || !open_files[fd_idx].in_use) {
        reply_status(-1); return;
    }
    if (len > FS_DATA_MAX) len = FS_DATA_MAX;
    if (len == 0) { WR32(fs_data, FS_READLEN, 0); reply_status(0); return; }

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

    /* Set file owner from creator fields */
    if (active_fs->set_creator) {
        uint16_t cuid = (uint16_t)RD32(fs_data, FS_CREAT_UID);
        uint16_t cgid = (uint16_t)RD32(fs_data, FS_CREAT_GID);
        active_fs->set_creator(cuid, cgid);
    }
    /* Pass creation timestamp */
    uint32_t create_time = RD32(fs_data, FS_MTIME);
    int rc = active_fs->create(name, &open_files[fd_idx]);
    if (rc == 0) {
        if (create_time && active_fs->update_mtime)
            active_fs->update_mtime(&open_files[fd_idx], create_time);
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
    /* Update mtime after write */
    if (rc == 0 && active_fs->update_mtime) {
        uint32_t mtime = RD32(fs_data, FS_MTIME);
        active_fs->update_mtime(&open_files[fd_idx], mtime);
    }
    reply_status(rc);
}

static void handle_delete(void) {
    char name[64];
    get_filename(name, sizeof(name));
    int rc = active_fs->delete(name);
    reply_status(rc);
}

static void handle_list(void) {
    char path[64];
    volatile char *fn = (volatile char *)(fs_data + FS_FILENAME);
    int i = 0;
    while (fn[i] && i < 63) { path[i] = fn[i]; i++; }
    path[i] = '\0';
    volatile uint8_t *dst = (volatile uint8_t *)(fs_data + FS_DATA);
    uint32_t count = 0;
    uint32_t total_bytes = 0;
        int rc = active_fs->list(path, (uint8_t *)dst, FS_DATA_MAX, &count, &total_bytes);
    WR32(fs_data, FS_LENGTH, count);
    WR32(fs_data, FS_FILESIZE, total_bytes);
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
    /* Set directory owner from creator fields */
    if (active_fs->set_creator) {
        uint16_t cuid = (uint16_t)RD32(fs_data, FS_CREAT_UID);
        uint16_t cgid = (uint16_t)RD32(fs_data, FS_CREAT_GID);
        active_fs->set_creator(cuid, cgid);
    }
    /* Pass creation timestamp */
        int rc = active_fs->mkdir ? active_fs->mkdir(name) : -1;
    reply_status(rc);
}

static void handle_rmdir(void) {
    char name[64];
    get_filename(name, sizeof(name));
    int rc = active_fs->rmdir ? active_fs->rmdir(name) : -1;
    reply_status(rc);
}


static void handle_stat_ex(void) {
    char name[64];
    get_filename(name, sizeof(name));
    uint32_t size = 0, mtime = 0;
    uint16_t uid = 0, gid = 0, mode = 0;
    int rc = -1;
    if (active_fs->stat_ex)
        rc = active_fs->stat_ex(name, &size, &uid, &gid, &mode, &mtime);
    else if (active_fs->stat)
        rc = active_fs->stat(name, &size);
    WR32(fs_data, FS_FILESIZE, size);
    WR32(fs_data, FS_UID, (uint32_t)uid);
    WR32(fs_data, FS_GID, (uint32_t)gid);
    WR32(fs_data, FS_MODE, (uint32_t)mode);
    WR32(fs_data, FS_MTIME, mtime);
    reply_status(rc);
}

static void handle_chmod(void) {
    char name[64];
    get_filename(name, sizeof(name));
    uint16_t mode = (uint16_t)RD32(fs_data, FS_MODE);
    int rc = (active_fs->chmod) ? active_fs->chmod(name, mode) : -1;
    reply_status(rc);
}

static void handle_chown(void) {
    char name[64];
    get_filename(name, sizeof(name));
    uint16_t uid = (uint16_t)RD32(fs_data, FS_UID);
    uint16_t gid = (uint16_t)RD32(fs_data, FS_GID);
    int rc = (active_fs->chown) ? active_fs->chown(name, uid, gid) : -1;
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
    case FS_CMD_CHMOD:    handle_chmod();   break;
    case FS_CMD_CHOWN:    handle_chown();   break;
    case FS_CMD_STAT_EX:  handle_stat_ex(); break;
    default: reply_status(-1); break;
    }

    return microkit_msginfo_new(0, 0);
}
