/*
 * AIOS FAT16 Filesystem Server (read/write)
 *
 * Protection domain that implements FAT16 over the block driver.
 * Supports: OPEN, READ, CLOSE, CREATE, WRITE, DELETE, STAT, SYNC.
 *
 * Copyright (c) 2025 AIOS Project
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <microkit.h>
#include "aios/channels.h"
#include "aios/ipc.h"
#include "fat16.h"

/* ── Memory regions (patched by Microkit loader) ───────── */
uintptr_t blk_data;   /* shared with blk_driver       */
uintptr_t fs_data;     /* shared with orchestrator      */

/* ── Helpers ───────────────────────────────────────────── */
static void my_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
}
static void my_memset(void *dst, int val, uint32_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)val;
}
static int my_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *p = a, *q = b;
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != q[i]) return p[i] - q[i];
    return 0;
}
static char my_toupper(char c) {
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

/* ── BPB cached fields ─────────────────────────────────── */
static uint16_t bytes_per_sector;
static uint8_t  sectors_per_cluster;
static uint16_t reserved_sectors;
static uint8_t  num_fats;
static uint16_t root_entry_count;
static uint16_t total_sectors_16;
static uint16_t sectors_per_fat;
static uint32_t total_sectors_32;

/* Derived */
static uint32_t fat1_start;        /* first sector of FAT1      */
static uint32_t fat2_start;        /* first sector of FAT2      */
static uint32_t root_dir_start;    /* first sector of root dir  */
static uint32_t root_dir_sectors;  /* sectors in root dir       */
static uint32_t data_start;        /* first sector of data area */
static uint32_t total_clusters;

/* ── Sector buffer (512 bytes) ─────────────────────────── */
static uint8_t sector_buf[512];

/* ── Open file table ───────────────────────────────────── */
#define MAX_OPEN 4
typedef struct {
    uint8_t  active;
    uint8_t  name83[11];
    uint16_t start_cluster;
    uint32_t file_size;
    uint32_t file_pos;       /* read/write cursor           */
    uint16_t cur_cluster;    /* cluster at file_pos         */
    uint32_t cur_cluster_idx;/* which cluster in chain      */
    uint16_t dir_entry_idx;  /* index in root dir (for writes) */
    uint8_t  dirty;          /* file was written to         */
    uint8_t  writable;       /* opened for write            */
} open_file_t;
static open_file_t open_files[MAX_OPEN];

/* ── State machine ─────────────────────────────────────── */
typedef enum {
    IDLE,
    INIT_READ_BPB,

    /* Read path */
    OPEN_SCANNING,
    READ_READING,
    READ_FAT_NEXT,     /* reading FAT sector to follow chain */

    /* Write path */
    CREATE_SCAN_DIR,       /* scanning root dir for free entry  */
    CREATE_ALLOC_FAT_R,    /* reading FAT sector for free clust */
    CREATE_WRITE_FAT,      /* writing FAT sector (mark cluster) */
    CREATE_WRITE_FAT2,     /* writing FAT copy 2                */
    CREATE_WRITE_DIR,      /* writing directory entry            */
    CREATE_DONE,

    WRITE_READ_FAT,        /* read FAT to find/alloc cluster    */
    WRITE_ALLOC_FAT_W,     /* write FAT (new cluster link)      */
    WRITE_ALLOC_FAT2_W,    /* write FAT copy 2                  */
    WRITE_DATA,            /* writing data sector               */
    WRITE_UPDATE_DIR_R,    /* read dir sector to update size    */
    WRITE_UPDATE_DIR_W,    /* write dir sector with new size    */
    WRITE_DONE,

    DELETE_READ_DIR,       /* read dir sector                   */
    DELETE_CLEAR_DIR,      /* write dir sector (mark deleted)   */
    DELETE_READ_FAT,       /* read FAT sector for chain walk    */
    DELETE_WRITE_FAT,      /* write FAT sector (free cluster)   */
    DELETE_WRITE_FAT2,     /* write FAT2 sector                 */
    DELETE_DONE,
} fs_state_t;
static fs_state_t fs_state = IDLE;

/* ── Pending operation context ─────────────────────────── */
static int     pending_fd;
static uint32_t pending_bytes_left;
static uint32_t pending_data_off;   /* offset into fs_data+FS_DATA */
static uint16_t pending_cluster;
static uint16_t pending_fat_sector; /* which FAT sector is cached  */
static uint16_t pending_dir_idx;    /* root dir entry index        */
static uint8_t  pending_name83[11];

/* ── Block I/O wrappers ───────────────────────────────── */
static void blk_read_sector(uint32_t sector) {
    WR32(blk_data, BLK_CMD, BLK_CMD_READ);
    WR32(blk_data, BLK_SECTOR, sector);
    microkit_notify(CH_FS_BLK);
}

static void blk_write_sector(uint32_t sector) {
    /* data must already be at blk_data+BLK_DATA */
    WR32(blk_data, BLK_CMD, BLK_CMD_WRITE);
    WR32(blk_data, BLK_SECTOR, sector);
    microkit_notify(CH_FS_BLK);
}

/* Copy 512 bytes between sector_buf and blk_data+BLK_DATA */
static void blk_to_buf(void) {
    my_memcpy(sector_buf, (void *)(blk_data + BLK_DATA), 512);
}
static void buf_to_blk(void) {
    my_memcpy((void *)(blk_data + BLK_DATA), sector_buf, 512);
}

/* ── FAT16 helpers ─────────────────────────────────────── */
static uint32_t cluster_to_sector(uint16_t cluster) {
    return data_start + (uint32_t)(cluster - 2) * sectors_per_cluster;
}

/* Read 16-bit LE from buffer */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}
static void wr32_le(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

/* Convert "MYFILE.TXT" → "MYFILE  TXT" (8.3 padded, uppercase) */
static void to_name83(const char *name, uint8_t *out) {
    my_memset(out, ' ', 11);
    int i = 0, o = 0;
    /* Base name (up to 8 chars) */
    while (name[i] && name[i] != '.' && o < 8) {
        out[o++] = my_toupper(name[i++]);
    }
    /* Skip dot */
    if (name[i] == '.') i++;
    /* Extension (up to 3 chars) */
    o = 8;
    while (name[i] && o < 11) {
        out[o++] = my_toupper(name[i++]);
    }
}

/* Read a FAT16 entry from sector_buf (assumes FAT sector is loaded) */

/* ── Reply to orchestrator ─────────────────────────────── */
static void reply_status(int status) {
    WR32(fs_data, FS_STATUS, (uint32_t)status);
    microkit_notify(CH_FS);
}

static void reply_status_size(int status, uint32_t size) {
    WR32(fs_data, FS_STATUS, (uint32_t)status);
    WR32(fs_data, FS_FILESIZE, size);
    microkit_notify(CH_FS);
}

/* ────────────────────────────────────────────────────────
 *  INIT: parse BPB
 * ──────────────────────────────────────────────────────── */
static void parse_bpb(void) {
    blk_to_buf();
    bytes_per_sector    = rd16(sector_buf + 11);
    sectors_per_cluster = sector_buf[13];
    reserved_sectors    = rd16(sector_buf + 14);
    num_fats            = sector_buf[16];
    root_entry_count    = rd16(sector_buf + 17);
    total_sectors_16    = rd16(sector_buf + 19);
    sectors_per_fat     = rd16(sector_buf + 22);
    total_sectors_32    = rd32(sector_buf + 32);

    fat1_start       = reserved_sectors;
    fat2_start       = reserved_sectors + sectors_per_fat;
    root_dir_sectors = ((uint32_t)root_entry_count * 32 + 511) / 512;
    root_dir_start   = reserved_sectors + (uint32_t)num_fats * sectors_per_fat;
    data_start       = root_dir_start + root_dir_sectors;

    uint32_t total = total_sectors_16 ? total_sectors_16 : total_sectors_32;
    uint32_t data_secs = total - data_start;
    total_clusters = data_secs / sectors_per_cluster;

    microkit_dbg_puts("FS: FAT16 init OK, clusters=");
    /* (optional: print total_clusters) */
    microkit_dbg_puts("\n");
}

/* ────────────────────────────────────────────────────────
 *  OPEN (read or write)
 * ──────────────────────────────────────────────────────── */
static void handle_open(void) {
    char name[64];
    int len = 0;
    while (len < 63) {
        char c = *(char *)(fs_data + FS_FILENAME + len);
        if (!c) break;
        name[len++] = c;
    }
    name[len] = 0;

    uint8_t name83[11];
    to_name83(name, name83);
    my_memcpy(pending_name83, name83, 11);

    /* Find free slot */
    pending_fd = -1;
    for (int i = 0; i < MAX_OPEN; i++) {
        if (!open_files[i].active) { pending_fd = i; break; }
    }
    if (pending_fd < 0) {
        reply_status(-1); /* no free slots */
        return;
    }

    /* Start scanning root directory */
    pending_dir_idx = 0;
    fs_state = OPEN_SCANNING;
    blk_read_sector(root_dir_start);
}

static void handle_open_scan_reply(void) {
    blk_to_buf();
    int entries_per_sector = 512 / 32;
    for (int i = 0; i < entries_per_sector; i++) {
        uint8_t *ent = sector_buf + i * 32;
        if (ent[0] == 0x00) {
            /* End of directory — file not found */
            reply_status(-2);
            fs_state = IDLE;
            return;
        }
        if (ent[0] == 0xE5) { pending_dir_idx++; continue; } /* deleted */
        if (ent[11] == 0x0F) { pending_dir_idx++; continue; } /* LFN */
        if (ent[11] & 0x08) { pending_dir_idx++; continue; }  /* volume label */

        if (my_memcmp(ent, pending_name83, 11) == 0) {
            /* Found it */
            open_file_t *f = &open_files[pending_fd];
            f->active = 1;
            my_memcpy(f->name83, ent, 11);
            f->start_cluster  = rd16(ent + 26);
            f->file_size      = rd32(ent + 28);
            f->file_pos       = 0;
            f->cur_cluster    = f->start_cluster;
            f->cur_cluster_idx = 0;
            f->dir_entry_idx  = pending_dir_idx;
            f->dirty          = 0;
            f->writable       = 0;

            WR32(fs_data, FS_FD, (uint32_t)pending_fd);
            reply_status_size(0, f->file_size);
            fs_state = IDLE;
            return;
        }
        pending_dir_idx++;
    }

    /* Check next sector */
    uint32_t sec_in_dir = (pending_dir_idx * 32) / 512;
    if (sec_in_dir + 1 < root_dir_sectors) {
        blk_read_sector(root_dir_start + sec_in_dir + 1);
        /* stay in OPEN_SCANNING */
    } else {
        /* Not found */
        reply_status(-2);
        fs_state = IDLE;
    }
}

/* ────────────────────────────────────────────────────────
 *  READ
 * ──────────────────────────────────────────────────────── */
static void handle_read(void) {
    int fd = (int)RD32(fs_data, FS_FD);
    uint32_t req_len = RD32(fs_data, FS_LENGTH);

    if (fd < 0 || fd >= MAX_OPEN || !open_files[fd].active) {
        WR32(fs_data, FS_LENGTH, 0);
        reply_status(-1);
        return;
    }

    open_file_t *f = &open_files[fd];
    if (f->file_pos >= f->file_size) {
        WR32(fs_data, FS_LENGTH, 0);
        reply_status(FS_ST_EOF);
        return;
    }

    uint32_t remain = f->file_size - f->file_pos;
    if (req_len > remain) req_len = remain;
    if (req_len > FS_DATA_MAX) req_len = FS_DATA_MAX;

    pending_fd = fd;
    pending_bytes_left = req_len;
    pending_data_off = 0;

    uint32_t cluster_bytes = (uint32_t)sectors_per_cluster * 512;

    /* If file_pos is exactly on a cluster boundary (and not at start
       of file), we need to advance to the next cluster first */
    if (f->file_pos > 0 && (f->file_pos % cluster_bytes) == 0) {
        uint32_t fat_byte = (uint32_t)f->cur_cluster * 2;
        pending_fat_sector = fat1_start + fat_byte / 512;
        fs_state = READ_FAT_NEXT;
        blk_read_sector(pending_fat_sector);
        return;
    }

    uint32_t off_in_cluster = f->file_pos % cluster_bytes;
    uint32_t sec_in_cluster = off_in_cluster / 512;
    uint32_t sector = cluster_to_sector(f->cur_cluster) + sec_in_cluster;
    fs_state = READ_READING;
    blk_read_sector(sector);
}

static void handle_read_reply(void) {
    open_file_t *f = &open_files[pending_fd];
    blk_to_buf();

    uint32_t off_in_sec = f->file_pos % 512;
    uint32_t avail = 512 - off_in_sec;
    if (avail > pending_bytes_left) avail = pending_bytes_left;

    my_memcpy((void *)(fs_data + FS_DATA + pending_data_off),
              sector_buf + off_in_sec, avail);

    f->file_pos += avail;
    pending_data_off += avail;
    pending_bytes_left -= avail;

    if (pending_bytes_left == 0) {
        WR32(fs_data, FS_LENGTH, pending_data_off);
        uint32_t st = (f->file_pos >= f->file_size) ? FS_ST_EOF : FS_ST_OK;
        reply_status(st);
        fs_state = IDLE;
        return;
    }

    /* Check if we crossed a cluster boundary */
    uint32_t cluster_bytes = (uint32_t)sectors_per_cluster * 512;
    if ((f->file_pos % cluster_bytes) == 0) {
        /* Read FAT sector to get next cluster */
        uint32_t fat_byte = (uint32_t)f->cur_cluster * 2;
        pending_fat_sector = fat1_start + fat_byte / 512;
        fs_state = READ_FAT_NEXT;
        blk_read_sector(pending_fat_sector);
        return;
    }

    /* Next sector in same cluster */
    uint32_t off_in_cl = f->file_pos % cluster_bytes;
    uint32_t sec_in_cl = off_in_cl / 512;
    uint32_t sector = cluster_to_sector(f->cur_cluster) + sec_in_cl;
    blk_read_sector(sector);
}

static void handle_read_fat_next(void) {
    blk_to_buf();
    open_file_t *f = &open_files[pending_fd];
    uint32_t off = ((uint32_t)f->cur_cluster * 2) % 512;
    uint16_t next = rd16(sector_buf + off);

    if (next >= 0xFFF8 || next < 2) {
        /* End of chain — return what we have */
        WR32(fs_data, FS_LENGTH, pending_data_off);
        reply_status(FS_ST_EOF);
        fs_state = IDLE;
        return;
    }

    f->cur_cluster = next;
    f->cur_cluster_idx++;

    /* Continue reading from new cluster */
    uint32_t sector = cluster_to_sector(f->cur_cluster);
    fs_state = READ_READING;
    blk_read_sector(sector);
}

/* ────────────────────────────────────────────────────────
 *  CLOSE
 * ──────────────────────────────────────────────────────── */
static void handle_close(void) {
    int fd = (int)RD32(fs_data, FS_FD);
    if (fd >= 0 && fd < MAX_OPEN) {
        /* If file was written, update dir entry size on disk */
        if (open_files[fd].dirty) {
            pending_fd = fd;
            uint32_t dir_sec = root_dir_start +
                               (open_files[fd].dir_entry_idx * 32) / 512;
            fs_state = WRITE_UPDATE_DIR_R;
            blk_read_sector(dir_sec);
            return;
        }
        open_files[fd].active = 0;
    }
    reply_status(0);
}

/* ────────────────────────────────────────────────────────
 *  CREATE — allocate first cluster + root dir entry
 * ──────────────────────────────────────────────────────── */
static void handle_create(void) {
    char name[64];
    int len = 0;
    while (len < 63) {
        char c = *(char *)(fs_data + FS_FILENAME + len);
        if (!c) break;
        name[len++] = c;
    }
    name[len] = 0;

    to_name83(name, pending_name83);

    /* Find free open slot */
    pending_fd = -1;
    for (int i = 0; i < MAX_OPEN; i++) {
        if (!open_files[i].active) { pending_fd = i; break; }
    }
    if (pending_fd < 0) {
        reply_status(-1);
        return;
    }

    /* Scan root dir for free entry */
    pending_dir_idx = 0;
    fs_state = CREATE_SCAN_DIR;
    blk_read_sector(root_dir_start);
}

static void handle_create_scan_reply(void) {
    blk_to_buf();
    int entries_per_sector = 512 / 32;
    for (int i = 0; i < entries_per_sector; i++) {
        uint32_t abs_idx = ((pending_dir_idx / entries_per_sector) *
                             entries_per_sector) + i;
        uint8_t *ent = sector_buf + i * 32;

        /* Check for existing file with same name — overwrite */
        if (ent[0] != 0x00 && ent[0] != 0xE5 &&
            my_memcmp(ent, pending_name83, 11) == 0) {
            /* File exists: reuse this entry (truncate) */
            pending_dir_idx = abs_idx;
            /* Free old cluster chain first — for simplicity,
               we'll just reuse the first cluster and zero the rest later */
            pending_cluster = rd16(ent + 26);
            goto found_entry;
        }

        /* Free entry? (0x00 = end, 0xE5 = deleted) */
        if (ent[0] == 0x00 || ent[0] == 0xE5) {
            pending_dir_idx = abs_idx;
            pending_cluster = 0; /* need to allocate */
            goto found_entry;
        }
    }

    /* Next sector of root dir */
    uint32_t next_sec = (pending_dir_idx / entries_per_sector) + 1;
    pending_dir_idx = next_sec * entries_per_sector;
    if (next_sec < root_dir_sectors) {
        blk_read_sector(root_dir_start + next_sec);
        return;
    }

    /* Root directory full */
    reply_status(-3);
    fs_state = IDLE;
    return;

found_entry:
    if (pending_cluster >= 2 && pending_cluster < 0xFFF0) {
        /* Reuse existing cluster — skip allocation */
        fs_state = CREATE_WRITE_DIR;
        /* Read dir sector to patch entry */
        uint32_t dir_sec = root_dir_start + (pending_dir_idx * 32) / 512;
        blk_read_sector(dir_sec);
    } else {
        /* Allocate a cluster: read first FAT sector */
        pending_fat_sector = fat1_start;
        fs_state = CREATE_ALLOC_FAT_R;
        blk_read_sector(fat1_start);
    }
}

static void handle_create_alloc_fat_reply(void) {
    blk_to_buf();
    /* Scan FAT sector for a free cluster (entry == 0x0000) */
    uint16_t base_cluster = (pending_fat_sector - fat1_start) * 256;
    /* 256 entries per 512-byte sector (each entry = 2 bytes) */
    for (int i = 0; i < 256; i++) {
        uint16_t cl = base_cluster + i;
        if (cl < 2) continue; /* clusters 0,1 reserved */
        uint16_t val = rd16(sector_buf + i * 2);
        if (val == 0x0000) {
            /* Found free cluster — mark as end-of-chain */
            pending_cluster = cl;
            wr16(sector_buf + i * 2, 0xFFF8);
            buf_to_blk();
            fs_state = CREATE_WRITE_FAT;
            blk_write_sector(pending_fat_sector);
            return;
        }
    }
    /* Try next FAT sector */
    pending_fat_sector++;
    if (pending_fat_sector < fat1_start + sectors_per_fat) {
        blk_read_sector(pending_fat_sector);
        /* stay in CREATE_ALLOC_FAT_R */
    } else {
        /* Disk full */
        reply_status(-4);
        fs_state = IDLE;
    }
}

static void handle_create_write_fat_reply(void) {
    /* Write same data to FAT2 */
    uint32_t fat2_sec = pending_fat_sector - fat1_start + fat2_start;
    /* sector_buf still has the modified FAT data — reuse it */
    buf_to_blk();
    fs_state = CREATE_WRITE_FAT2;
    blk_write_sector(fat2_sec);
}

static void handle_create_write_fat2_reply(void) {
    /* Now write the directory entry */
    uint32_t dir_sec = root_dir_start + (pending_dir_idx * 32) / 512;
    fs_state = CREATE_WRITE_DIR;
    blk_read_sector(dir_sec);
}

static void handle_create_write_dir_reply(void) {
    /* Received the dir sector — patch the entry */
    blk_to_buf();
    uint32_t off = (pending_dir_idx * 32) % 512;
    uint8_t *ent = sector_buf + off;

    my_memcpy(ent, pending_name83, 11);
    ent[11] = 0x20; /* ARCHIVE attribute */
    my_memset(ent + 12, 0, 14); /* zero timestamps etc */
    wr16(ent + 26, pending_cluster);
    wr32_le(ent + 28, 0); /* file size = 0 */

    buf_to_blk();
    fs_state = CREATE_DONE;
    blk_write_sector(root_dir_start + (pending_dir_idx * 32) / 512);
}

static void handle_create_done(void) {
    /* Set up the open file */
    open_file_t *f = &open_files[pending_fd];
    f->active = 1;
    my_memcpy(f->name83, pending_name83, 11);
    f->start_cluster   = pending_cluster;
    f->file_size       = 0;
    f->file_pos        = 0;
    f->cur_cluster     = pending_cluster;
    f->cur_cluster_idx = 0;
    f->dir_entry_idx   = pending_dir_idx;
    f->dirty           = 0;
    f->writable        = 1;

    WR32(fs_data, FS_FD, (uint32_t)pending_fd);
    reply_status_size(0, 0);
    fs_state = IDLE;
}

/* ────────────────────────────────────────────────────────
 *  WRITE — append data to an open file
 *
 *  Orchestrator places data at fs_data+FS_DATA,
 *  length in FS_REQLEN.
 *  We write one sector at a time.
 * ──────────────────────────────────────────────────────── */
static void handle_write(void) {
    int fd = (int)RD32(fs_data, FS_FD);
    uint32_t req_len = RD32(fs_data, FS_REQLEN);

    if (fd < 0 || fd >= MAX_OPEN || !open_files[fd].active) {
        reply_status(-1);
        return;
    }

    open_file_t *f = &open_files[fd];
    f->writable = 1;

    if (req_len > FS_DATA_MAX) req_len = FS_DATA_MAX;

    pending_fd = fd;
    pending_bytes_left = req_len;
    pending_data_off = 0;

    /* If file_pos is at a cluster boundary and we've used
       at least one cluster, we need to allocate a new one */
    uint32_t cluster_bytes = (uint32_t)sectors_per_cluster * 512;

    if (f->file_pos > 0 && (f->file_pos % cluster_bytes) == 0) {
        /* Need to allocate new cluster and link to chain */
        pending_fat_sector = fat1_start;
        fs_state = WRITE_READ_FAT;
        blk_read_sector(fat1_start);
        return;
    }

    /* Write into current cluster */
    fs_state = WRITE_DATA;
    uint32_t off_in_cl = f->file_pos % cluster_bytes;
    uint32_t sec_in_cl = off_in_cl / 512;
    uint32_t sec = cluster_to_sector(f->cur_cluster) + sec_in_cl;

    /* If we're not at a sector boundary, read-modify-write */
    if (f->file_pos % 512 != 0) {
        blk_read_sector(sec); /* will patch in WRITE_DATA handler */
    } else {
        /* Fresh sector — fill from fs_data directly */
        my_memset(sector_buf, 0, 512);
        uint32_t chunk = 512;
        if (chunk > pending_bytes_left) chunk = pending_bytes_left;
        my_memcpy(sector_buf, (void *)(fs_data + FS_DATA + pending_data_off),
                  chunk);
        buf_to_blk();
        blk_write_sector(sec);
    }
}

static void handle_write_fat_reply(void) {
    /* Scanning FAT for free cluster to extend the chain */
    blk_to_buf();
    uint16_t base_cluster = (pending_fat_sector - fat1_start) * 256;
    open_file_t *f = &open_files[pending_fd];

    for (int i = 0; i < 256; i++) {
        uint16_t cl = base_cluster + i;
        if (cl < 2) continue;
        uint16_t val = rd16(sector_buf + i * 2);
        if (val == 0x0000) {
            /* Found free cluster */
            uint16_t new_cluster = cl;

            /* Mark new cluster as end-of-chain */
            wr16(sector_buf + i * 2, 0xFFF8);

            /* Also link old cluster → new cluster if in same FAT sector */
            uint32_t old_fat_byte = (uint32_t)f->cur_cluster * 2;
            uint32_t old_fat_sec = fat1_start + old_fat_byte / 512;
            if (old_fat_sec == pending_fat_sector) {
                uint32_t old_off = old_fat_byte % 512;
                wr16(sector_buf + old_off, new_cluster);
            }
            /* else: would need a separate write — simplified for now */

            buf_to_blk();
            f->cur_cluster = new_cluster;
            f->cur_cluster_idx++;

            fs_state = WRITE_ALLOC_FAT_W;
            blk_write_sector(pending_fat_sector);
            return;
        }
    }

    /* Next FAT sector */
    pending_fat_sector++;
    if (pending_fat_sector < fat1_start + sectors_per_fat) {
        blk_read_sector(pending_fat_sector);
    } else {
        reply_status(-4); /* disk full */
        fs_state = IDLE;
    }
}

static void handle_write_alloc_fat_w_reply(void) {
    /* Write FAT2 copy */
    uint32_t fat2_sec = pending_fat_sector - fat1_start + fat2_start;
    buf_to_blk();
    fs_state = WRITE_ALLOC_FAT2_W;
    blk_write_sector(fat2_sec);
}

static void handle_write_alloc_fat2_w_reply(void) {
    /* FAT updated, now write the actual data sector */
    open_file_t *f = &open_files[pending_fd];
    uint32_t cluster_bytes = (uint32_t)sectors_per_cluster * 512;
    uint32_t off_in_cl = f->file_pos % cluster_bytes;
    uint32_t sec_in_cl = off_in_cl / 512;
    uint32_t sec = cluster_to_sector(f->cur_cluster) + sec_in_cl;

    my_memset(sector_buf, 0, 512);
    uint32_t chunk = 512;
    if (chunk > pending_bytes_left) chunk = pending_bytes_left;
    my_memcpy(sector_buf, (void *)(fs_data + FS_DATA + pending_data_off),
              chunk);
    buf_to_blk();
    fs_state = WRITE_DATA;
    blk_write_sector(sec);
}

static void handle_write_data_reply(void) {
    open_file_t *f = &open_files[pending_fd];

    /* If we got here from a read-modify-write (non-aligned), patch now */
    if (fs_state == WRITE_DATA && (f->file_pos % 512) != 0 &&
        pending_data_off == 0) {
        /* sector_buf has been read — patch it */
        blk_to_buf();
        uint32_t off_in_sec = f->file_pos % 512;
        uint32_t avail = 512 - off_in_sec;
        if (avail > pending_bytes_left) avail = pending_bytes_left;
        my_memcpy(sector_buf + off_in_sec,
                  (void *)(fs_data + FS_DATA + pending_data_off), avail);
        buf_to_blk();

        /* Now write the patched sector */
        uint32_t cluster_bytes = (uint32_t)sectors_per_cluster * 512;
        uint32_t off_in_cl = f->file_pos % cluster_bytes;
        uint32_t sec_in_cl = off_in_cl / 512;
        uint32_t sec = cluster_to_sector(f->cur_cluster) + sec_in_cl;
        blk_write_sector(sec);
        /* We'll re-enter this function when write completes */
        return;
    }

    /* A sector was written successfully */
    uint32_t off_in_sec = f->file_pos % 512;
    uint32_t written = 512 - off_in_sec;
    if (written > pending_bytes_left) written = pending_bytes_left;

    f->file_pos += written;
    if (f->file_pos > f->file_size) f->file_size = f->file_pos;
    f->dirty = 1;
    pending_data_off += written;
    pending_bytes_left -= written;

    if (pending_bytes_left == 0) {
        /* Done — update dir entry with new size */
        uint32_t dir_sec = root_dir_start +
                           (f->dir_entry_idx * 32) / 512;
        fs_state = WRITE_UPDATE_DIR_R;
        blk_read_sector(dir_sec);
        return;
    }

    /* More data to write — check if we crossed a cluster boundary */
    uint32_t cluster_bytes = (uint32_t)sectors_per_cluster * 512;
    if ((f->file_pos % cluster_bytes) == 0) {
        /* Allocate new cluster */
        pending_fat_sector = fat1_start;
        fs_state = WRITE_READ_FAT;
        blk_read_sector(fat1_start);
        return;
    }

    /* Next sector in same cluster */
    uint32_t off_in_cl = f->file_pos % cluster_bytes;
    uint32_t sec_in_cl = off_in_cl / 512;
    uint32_t sec = cluster_to_sector(f->cur_cluster) + sec_in_cl;

    my_memset(sector_buf, 0, 512);
    uint32_t chunk = 512;
    if (chunk > pending_bytes_left) chunk = pending_bytes_left;
    my_memcpy(sector_buf, (void *)(fs_data + FS_DATA + pending_data_off),
              chunk);
    buf_to_blk();
    blk_write_sector(sec);
}

static void handle_write_update_dir_r(void) {
    blk_to_buf();
    open_file_t *f = &open_files[pending_fd];
    uint32_t off = (f->dir_entry_idx * 32) % 512;
    /* Update file size in directory entry */
    wr32_le(sector_buf + off + 28, f->file_size);
    /* Update start cluster if it was changed */
    wr16(sector_buf + off + 26, f->start_cluster);
    buf_to_blk();
    fs_state = WRITE_UPDATE_DIR_W;
    blk_write_sector(root_dir_start + (f->dir_entry_idx * 32) / 512);
}

static void handle_write_update_dir_w(void) {
    open_files[pending_fd].dirty = 0;
    WR32(fs_data, FS_READLEN, pending_data_off); /* bytes written */
    reply_status(0);
    fs_state = IDLE;
}

/* ────────────────────────────────────────────────────────
 *  DELETE — mark dir entry as deleted, free cluster chain
 * ──────────────────────────────────────────────────────── */
static void handle_delete(void) {
    char name[64];
    int len = 0;
    while (len < 63) {
        char c = *(char *)(fs_data + FS_FILENAME + len);
        if (!c) break;
        name[len++] = c;
    }
    name[len] = 0;

    to_name83(name, pending_name83);
    pending_dir_idx = 0;
    fs_state = DELETE_READ_DIR;
    blk_read_sector(root_dir_start);
}

static void handle_delete_dir_reply(void) {
    blk_to_buf();
    int entries_per_sector = 512 / 32;
    for (int i = 0; i < entries_per_sector; i++) {
        uint8_t *ent = sector_buf + i * 32;
        if (ent[0] == 0x00) break;
        if (ent[0] == 0xE5) { pending_dir_idx++; continue; }
        if (my_memcmp(ent, pending_name83, 11) == 0) {
            /* Found it — save start cluster, mark as deleted */
            pending_cluster = rd16(ent + 26);
            ent[0] = 0xE5;
            buf_to_blk();
            fs_state = DELETE_CLEAR_DIR;
            blk_write_sector(root_dir_start + (pending_dir_idx * 32) / 512);
            return;
        }
        pending_dir_idx++;
    }

    uint32_t next_sec = (pending_dir_idx / entries_per_sector) + 1;
    pending_dir_idx = next_sec * entries_per_sector;
    if (next_sec < root_dir_sectors) {
        blk_read_sector(root_dir_start + next_sec);
    } else {
        reply_status(-2); /* not found */
        fs_state = IDLE;
    }
}

static void handle_delete_clear_dir_reply(void) {
    /* Dir entry cleared — now free the cluster chain */
    if (pending_cluster < 2 || pending_cluster >= 0xFFF0) {
        /* No clusters to free (empty file) */
        reply_status(0);
        fs_state = IDLE;
        return;
    }
    uint32_t fat_byte = (uint32_t)pending_cluster * 2;
    pending_fat_sector = fat1_start + fat_byte / 512;
    fs_state = DELETE_READ_FAT;
    blk_read_sector(pending_fat_sector);
}

static void handle_delete_fat_reply(void) {
    blk_to_buf();
    uint32_t fat_byte = (uint32_t)pending_cluster * 2;
    uint32_t off = fat_byte % 512;
    uint16_t next = rd16(sector_buf + off);

    /* Free this cluster */
    wr16(sector_buf + off, 0x0000);
    buf_to_blk();
    fs_state = DELETE_WRITE_FAT;
    blk_write_sector(pending_fat_sector);

    /* Save next cluster for chain walk */
    if (next >= 0xFFF8 || next < 2) {
        pending_cluster = 0; /* end of chain after this write */
    } else {
        pending_cluster = next;
    }
}

static void handle_delete_write_fat_reply(void) {
    /* Write FAT2 */
    uint32_t fat2_sec = pending_fat_sector - fat1_start + fat2_start;
    buf_to_blk();
    fs_state = DELETE_WRITE_FAT2;
    blk_write_sector(fat2_sec);
}

static void handle_delete_write_fat2_reply(void) {
    if (pending_cluster == 0 || pending_cluster >= 0xFFF0) {
        /* Done freeing chain */
        reply_status(0);
        fs_state = IDLE;
        return;
    }
    /* Continue chain walk */
    uint32_t fat_byte = (uint32_t)pending_cluster * 2;
    pending_fat_sector = fat1_start + fat_byte / 512;
    fs_state = DELETE_READ_FAT;
    blk_read_sector(pending_fat_sector);
}

/* ────────────────────────────────────────────────────────
 *  Command dispatcher (from orchestrator)
 * ──────────────────────────────────────────────────────── */
static void handle_orch(void) {
    uint32_t cmd = RD32(fs_data, FS_CMD);
    switch (cmd) {
    case FS_CMD_OPEN:   handle_open();   break;
    case FS_CMD_READ:   handle_read();   break;
    case FS_CMD_CLOSE:  handle_close();  break;
    case FS_CMD_CREATE: handle_create(); break;
    case FS_CMD_WRITE:  handle_write();  break;
    case FS_CMD_DELETE: handle_delete(); break;
    default:
        reply_status(-99);
        break;
    }
}

/* ────────────────────────────────────────────────────────
 *  Block driver reply dispatcher
 * ──────────────────────────────────────────────────────── */
static void handle_blk_reply(void) {
    switch (fs_state) {
    case INIT_READ_BPB:       parse_bpb(); fs_state = IDLE; break;

    /* Read path */
    case OPEN_SCANNING:       handle_open_scan_reply();       break;
    case READ_READING:        handle_read_reply();            break;
    case READ_FAT_NEXT:       handle_read_fat_next();         break;

    /* Create path */
    case CREATE_SCAN_DIR:     handle_create_scan_reply();     break;
    case CREATE_ALLOC_FAT_R:  handle_create_alloc_fat_reply();break;
    case CREATE_WRITE_FAT:    handle_create_write_fat_reply();break;
    case CREATE_WRITE_FAT2:   handle_create_write_fat2_reply();break;
    case CREATE_WRITE_DIR:    handle_create_write_dir_reply();break;
    case CREATE_DONE:         handle_create_done();           break;

    /* Write path */
    case WRITE_READ_FAT:      handle_write_fat_reply();       break;
    case WRITE_ALLOC_FAT_W:   handle_write_alloc_fat_w_reply();break;
    case WRITE_ALLOC_FAT2_W:  handle_write_alloc_fat2_w_reply();break;
    case WRITE_DATA:          handle_write_data_reply();      break;
    case WRITE_UPDATE_DIR_R:  handle_write_update_dir_r();    break;
    case WRITE_UPDATE_DIR_W:  handle_write_update_dir_w();    break;

    /* Delete path */
    case DELETE_READ_DIR:     handle_delete_dir_reply();      break;
    case DELETE_CLEAR_DIR:    handle_delete_clear_dir_reply();break;
    case DELETE_READ_FAT:     handle_delete_fat_reply();      break;
    case DELETE_WRITE_FAT:    handle_delete_write_fat_reply();break;
    case DELETE_WRITE_FAT2:   handle_delete_write_fat2_reply();break;

    default: break;
    }
}

/* ────────────────────────────────────────────────────────
 *  Microkit entry points
 * ──────────────────────────────────────────────────────── */
void init(void) {
    for (int i = 0; i < MAX_OPEN; i++)
        open_files[i].active = 0;

    microkit_dbg_puts("FS: reading BPB...\n");
    fs_state = INIT_READ_BPB;
    blk_read_sector(0);
}

void notified(microkit_channel ch) {
    if (ch == CH_FS) {
        handle_orch();
    }
    else if (ch == CH_FS_BLK) {
        handle_blk_reply();
    }
}
