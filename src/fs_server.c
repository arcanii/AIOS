/*
 * AIOS FAT16 Filesystem Server
 *
 * Read-only FAT16 driver. Communicates with blk_driver for sector I/O
 * and orchestrator for file operations (open, read, close).
 *
 * Supports sequential and random-access reads with cached cluster
 * position tracking per open file.
 *
 * Copyright (c) 2025 AIOS Project
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <microkit.h>
#include "aios/channels.h"
#include "aios/ipc.h"
#include "fat16.h"

/* ── Memory regions (set by Microkit loader via setvar) ── */
uintptr_t fs_data;
uintptr_t blk_data;

/* ── Cached BPB fields ───────────────────────────────── */
static uint32_t bytes_per_sector;
static uint32_t sectors_per_cluster;
static uint32_t reserved_sectors;
static uint32_t num_fats;
static uint32_t root_entry_count;
static uint32_t fat_size_16;
static uint32_t root_dir_start;
static uint32_t root_dir_sectors;
static uint32_t data_start;
static int      fs_ready = 0;

/* ── Open file table ─────────────────────────────────── */
#define MAX_OPEN 4

typedef struct {
    int      in_use;
    uint16_t first_cluster;
    uint32_t file_size;
    uint16_t cur_cluster;       /* cached current cluster   */
    uint32_t cur_cluster_off;   /* byte offset of cur_cluster start */
} open_file_t;

static open_file_t open_files[MAX_OPEN];

/* ── State machine ───────────────────────────────────── */
typedef enum {
    IDLE,
    INIT_READ_BPB,
    OPEN_SCANNING,
    READ_READING,
} fsm_state_t;

static fsm_state_t state = IDLE;

/* ── Pending operation data ──────────────────────────── */
static char     pending_name[11];
static uint32_t pending_dir_sec;
static int      pending_fd;
static uint32_t pending_len;
static uint32_t pending_done;
static uint32_t pending_pos_in_cluster;
static uint16_t pending_cluster;
static int      reading_fat_sector = 0;

/* Skip-mode state (for non-zero offset reads) */
static uint32_t pending_target_offset;
static uint32_t skip_cluster_start;

/* ── Sector buffer ───────────────────────────────────── */
static uint8_t sector_buf[512];

/* ── FAT16 end-of-chain marker ───────────────────────── */
#define FAT16_EOC 0xFFF8

/* ── Helper functions ────────────────────────────────── */

static void start_blk_read(uint32_t lba) {
    WR32(blk_data, BLK_CMD, BLK_CMD_READ);
    WR32(blk_data, BLK_SECTOR, lba);
    WR32(blk_data, BLK_STATUS, 0xFF);
    microkit_notify(CH_FS_BLK);
}

static int finish_blk_read(void) {
    uint32_t st = RD32(blk_data, BLK_STATUS);
    if (st != 0) return -1;
    volatile uint8_t *src = (volatile uint8_t *)(blk_data + BLK_DATA);
    for (int i = 0; i < 512; i++)
        sector_buf[i] = src[i];
    return 0;
}

static uint32_t cluster_to_sector(uint16_t c) {
    return data_start + (c - 2) * sectors_per_cluster;
}

static void to_83(const char *name, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int b = 0, i = 0;
    for (; name[i] && name[i] != '.' && b < 8; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[b++] = c;
    }
    if (name[i] == '.') {
        i++;
        int e = 0;
        for (; name[i] && e < 3; i++) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[8 + e++] = c;
        }
    }
}

static int alloc_fd(uint16_t cluster, uint32_t fsize) {
    for (int i = 0; i < MAX_OPEN; i++) {
        if (!open_files[i].in_use) {
            open_files[i].in_use          = 1;
            open_files[i].first_cluster   = cluster;
            open_files[i].file_size       = fsize;
            open_files[i].cur_cluster     = cluster;
            open_files[i].cur_cluster_off = 0;
            return i;
        }
    }
    return -1;
}

/* ── State: INIT_READ_BPB ────────────────────────────── */

static void resume_init_bpb(void) {
    if (finish_blk_read() < 0) {
        microkit_dbg_puts("FS: BPB read failed\n");
        state = IDLE;
        return;
    }

    fat16_bpb_t *bpb = (fat16_bpb_t *)sector_buf;
    bytes_per_sector   = bpb->bytes_per_sector;
    sectors_per_cluster = bpb->sectors_per_cluster;
    reserved_sectors   = bpb->reserved_sectors;
    num_fats           = bpb->num_fats;
    root_entry_count   = bpb->root_entry_count;
    fat_size_16        = bpb->fat_size_16;

    root_dir_start   = reserved_sectors + num_fats * fat_size_16;
    root_dir_sectors = ((root_entry_count * 32) + 511) / 512;
    data_start       = root_dir_start + root_dir_sectors;

    if (bytes_per_sector == 512) {
        fs_ready = 1;
        microkit_dbg_puts("FS: FAT16 ready\n");
    } else {
        microkit_dbg_puts("FS: unsupported sector size\n");
    }
    state = IDLE;
}

/* ── State: OPEN_SCANNING ────────────────────────────── */

static void resume_open_scan(void) {
    if (finish_blk_read() < 0) {
        WR32(fs_data, FS_STATUS, FS_ST_IO_ERR);
        microkit_notify(CH_FS);
        state = IDLE;
        return;
    }

    fat16_dirent_t *entries = (fat16_dirent_t *)sector_buf;
    int count = 512 / 32;

    for (int e = 0; e < count; e++) {
        uint8_t first = (uint8_t)entries[e].name[0];

        /* End of directory */
        if (first == 0x00) goto not_found;

        /* Deleted entry */
        if (first == 0xE5) continue;

        /* Skip long filename entries */
        if (entries[e].attr & FAT16_ATTR_LONG_NAME) continue;

        /* Skip volume labels and subdirectories */
        if (entries[e].attr & (FAT16_ATTR_VOLUME_ID | FAT16_ATTR_DIRECTORY)) continue;

        /* Compare 8.3 name */
        int match = 1;
        for (int j = 0; j < 11; j++) {
            if (entries[e].name[j] != pending_name[j]) {
                match = 0;
                break;
            }
        }

        if (match) {
            uint16_t cluster = entries[e].first_cluster_lo;
            uint32_t fsize   = entries[e].file_size;
            int fd = alloc_fd(cluster, fsize);
            if (fd < 0) {
                WR32(fs_data, FS_STATUS, FS_ST_IO_ERR);
            } else {
                WR32(fs_data, FS_FD, (uint32_t)fd);
                WR32(fs_data, FS_FILESIZE, fsize);
                WR32(fs_data, FS_STATUS, FS_ST_OK);
            }
            microkit_notify(CH_FS);
            state = IDLE;
            return;
        }
    }

    /* Try next directory sector */
    pending_dir_sec++;
    if (pending_dir_sec < root_dir_sectors) {
        start_blk_read(root_dir_start + pending_dir_sec);
        return;
    }

not_found:
    WR32(fs_data, FS_STATUS, FS_ST_NOT_FOUND);
    microkit_notify(CH_FS);
    state = IDLE;
}

/* ── State: READ_READING ─────────────────────────────── */

static void resume_read_v2(void) {
    if (finish_blk_read() < 0) {
        WR32(fs_data, FS_STATUS, FS_ST_IO_ERR);
        WR32(fs_data, FS_LENGTH, pending_done);
        microkit_notify(CH_FS);
        state = IDLE;
        reading_fat_sector = 0;
        return;
    }

    uint32_t cluster_bytes = sectors_per_cluster * bytes_per_sector;

    /* ── Skip mode: walking the FAT chain to reach target offset ── */
    if (reading_fat_sector == 2) {
        /* We just read a FAT sector to advance the chain during skip */
        uint32_t off = (pending_cluster * 2) % bytes_per_sector;
        pending_cluster = *(uint16_t *)(sector_buf + off);

        /* Update cached position */
        open_files[pending_fd].cur_cluster = pending_cluster;
        skip_cluster_start += cluster_bytes;
        open_files[pending_fd].cur_cluster_off = skip_cluster_start;

        if (pending_cluster >= FAT16_EOC) {
            WR32(fs_data, FS_STATUS, FS_ST_EOF);
            WR32(fs_data, FS_LENGTH, 0);
            microkit_notify(CH_FS);
            state = IDLE;
            reading_fat_sector = 0;
            return;
        }

        /* Have we reached the target cluster? */
        if (skip_cluster_start + cluster_bytes > pending_target_offset) {
            /* Yes — start reading data */
            pending_pos_in_cluster = pending_target_offset - skip_cluster_start;
            reading_fat_sector = 0;
            uint32_t sec = cluster_to_sector(pending_cluster) +
                           (pending_pos_in_cluster / 512);
            start_blk_read(sec);
            return;
        }

        /* Need to skip more clusters — read next FAT entry */
        uint32_t fat_off = pending_cluster * 2;
        uint32_t fat_sec = reserved_sectors + (fat_off / bytes_per_sector);
        start_blk_read(fat_sec);
        return;
    }

    /* ── Normal FAT chain follow (during data read) ──── */
    if (reading_fat_sector == 1) {
        uint32_t off = (pending_cluster * 2) % bytes_per_sector;
        pending_cluster = *(uint16_t *)(sector_buf + off);
        pending_pos_in_cluster = 0;
        reading_fat_sector = 0;

        /* Update cached position */
        open_files[pending_fd].cur_cluster = pending_cluster;
        open_files[pending_fd].cur_cluster_off += cluster_bytes;

        if (pending_cluster >= FAT16_EOC) {
            WR32(fs_data, FS_STATUS, pending_done ? FS_ST_OK : FS_ST_EOF);
            WR32(fs_data, FS_LENGTH, pending_done);
            microkit_notify(CH_FS);
            state = IDLE;
            return;
        }

        /* Continue reading from new cluster */
        start_blk_read(cluster_to_sector(pending_cluster));
        return;
    }

    /* ── Normal data sector read ─────────────────────── */
    uint32_t sec_off = pending_pos_in_cluster % 512;
    uint32_t avail   = 512 - sec_off;
    uint32_t want    = pending_len - pending_done;
    if (want > avail) want = avail;

    uint8_t *dst = (uint8_t *)(fs_data + FS_DATA) + pending_done;
    for (uint32_t i = 0; i < want; i++)
        dst[i] = sector_buf[sec_off + i];

    pending_done += want;
    pending_pos_in_cluster += want;

    /* Finished this request? */
    if (pending_done >= pending_len) {
        WR32(fs_data, FS_STATUS, FS_ST_OK);
        WR32(fs_data, FS_LENGTH, pending_done);
        microkit_notify(CH_FS);
        state = IDLE;
        return;
    }

    /* Crossed cluster boundary? Follow FAT chain */
    if (pending_pos_in_cluster >= cluster_bytes) {
        uint32_t fat_off = pending_cluster * 2;
        uint32_t fat_sec = reserved_sectors + (fat_off / bytes_per_sector);
        reading_fat_sector = 1;
        start_blk_read(fat_sec);
        return;
    }

    /* Read next sector within same cluster */
    uint32_t sec = cluster_to_sector(pending_cluster) +
                   (pending_pos_in_cluster / 512);
    start_blk_read(sec);
}

/* ── Command handling ────────────────────────────────── */

static void handle_orch(void) {
    if (!fs_ready) {
        WR32(fs_data, FS_STATUS, FS_ST_IO_ERR);
        microkit_notify(CH_FS);
        return;
    }

    uint32_t cmd = RD32(fs_data, FS_CMD);

    switch (cmd) {

    case FS_CMD_OPEN: {
        char *fn = (char *)(fs_data + FS_FILENAME);
        to_83(fn, pending_name);
        pending_dir_sec = 0;
        state = OPEN_SCANNING;
        start_blk_read(root_dir_start);
        break;
    }

    case FS_CMD_READ: {
        pending_fd = RD32(fs_data, FS_FD);
        uint32_t off = RD32(fs_data, FS_OFFSET);
        pending_len = RD32(fs_data, FS_LENGTH);
        if (pending_len > FS_DATA_MAX) pending_len = FS_DATA_MAX;
        pending_done = 0;
        reading_fat_sector = 0;

        /* Validate file descriptor */
        if (pending_fd < 0 || pending_fd >= MAX_OPEN ||
            !open_files[pending_fd].in_use) {
            WR32(fs_data, FS_STATUS, FS_ST_IO_ERR);
            WR32(fs_data, FS_LENGTH, 0);
            microkit_notify(CH_FS);
            break;
        }

        open_file_t *f = &open_files[pending_fd];

        /* Check bounds */
        if (off >= f->file_size) {
            WR32(fs_data, FS_STATUS, FS_ST_EOF);
            WR32(fs_data, FS_LENGTH, 0);
            microkit_notify(CH_FS);
            break;
        }
        if (off + pending_len > f->file_size)
            pending_len = f->file_size - off;

        uint32_t cluster_bytes = sectors_per_cluster * bytes_per_sector;

        /* Determine starting cluster using cached position */
        if (off < f->cur_cluster_off) {
            /* Backward seek — must restart from beginning */
            f->cur_cluster     = f->first_cluster;
            f->cur_cluster_off = 0;
        }

        pending_cluster    = f->cur_cluster;
        skip_cluster_start = f->cur_cluster_off;

        /* Do we need to skip forward through the FAT chain? */
        if (off >= skip_cluster_start + cluster_bytes) {
            /* Need to walk the FAT chain */
            pending_target_offset = off;
            reading_fat_sector = 2;  /* skip mode */
            state = READ_READING;
            uint32_t fat_off = pending_cluster * 2;
            uint32_t fat_sec = reserved_sectors + (fat_off / bytes_per_sector);
            start_blk_read(fat_sec);
            break;
        }

        /* Target is within the current cluster */
        pending_pos_in_cluster = off - skip_cluster_start;

        if (pending_cluster >= FAT16_EOC) {
            WR32(fs_data, FS_STATUS, FS_ST_EOF);
            WR32(fs_data, FS_LENGTH, 0);
            microkit_notify(CH_FS);
            break;
        }

        state = READ_READING;
        uint32_t sec = cluster_to_sector(pending_cluster) +
                       (pending_pos_in_cluster / 512);
        start_blk_read(sec);
        break;
    }

    case FS_CMD_CLOSE: {
        int fd = RD32(fs_data, FS_FD);
        if (fd >= 0 && fd < MAX_OPEN)
            open_files[fd].in_use = 0;
        WR32(fs_data, FS_STATUS, FS_ST_OK);
        microkit_notify(CH_FS);
        break;
    }

    default: {
        WR32(fs_data, FS_STATUS, FS_ST_IO_ERR);
        microkit_notify(CH_FS);
        break;
    }

    } /* switch */
}

/* ── Microkit entry points ───────────────────────────── */

void init(void) {
    for (int i = 0; i < MAX_OPEN; i++)
        open_files[i].in_use = 0;

    /* Read BPB from sector 0 */
    state = INIT_READ_BPB;
    start_blk_read(0);
}

void notified(microkit_channel ch) {
    if (ch == CH_FS_BLK) {
        switch (state) {
        case INIT_READ_BPB:  resume_init_bpb();  break;
        case OPEN_SCANNING:  resume_open_scan();  break;
        case READ_READING:   resume_read_v2();    break;
        default: break;
        }
    } else if (ch == CH_FS) {
        if (state == IDLE)
            handle_orch();
    }
}

