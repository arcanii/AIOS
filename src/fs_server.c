/*
 * AIOS Filesystem Server – FAT16 (synchronous PPC version)
 *
 * Block I/O is now synchronous via microkit_ppcall to blk_driver.
 * This eliminates the entire async state machine.
 *
 * Provides: OPEN, READ, CLOSE, CREATE, WRITE, DELETE, LIST
 * Receives commands via shared memory (fs_data) + notification from orchestrator.
 * Replies via shared memory + notification back.
 *
 * Copyright (c) 2025 AIOS Project
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <microkit.h>
#include "aios/channels.h"
#include "aios/ipc.h"

/* ── Memory regions ────────────────────────────────────── */
uintptr_t blk_data;
uintptr_t fs_data;

/* ── Helpers ───────────────────────────────────────────── */
static inline uint8_t my_toupper(uint8_t c) {
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}
static void my_memset(void *dst, int c, int n) {
    uint8_t *d = (uint8_t *)dst;
    for (int i = 0; i < n; i++) d[i] = (uint8_t)c;
}
static void my_memcpy(void *dst, const void *src, int n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}
static int my_memcmp(const void *a, const void *b, int n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (int i = 0; i < n; i++) {
        if (x[i] != y[i]) return x[i] - y[i];
    }
    return 0;
}
static uint16_t rd16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void wr32(uint8_t *p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }

/* ── Sector buffer ─────────────────────────────────────── */
static uint8_t sector_buf[512];

/* Read multiple consecutive sectors into a buffer */
static void blk_read_sectors(uint32_t sector, uint32_t count, uint8_t *dst) {
    while (count > 0) {
        uint32_t batch = count;
        if (batch > BLK_DATA_MAX / 512) batch = BLK_DATA_MAX / 512;
        WR32(blk_data, BLK_CMD, BLK_CMD_READ);
        WR32(blk_data, BLK_SECTOR, sector);
        WR32(blk_data, BLK_COUNT, batch);
        microkit_ppcall(CH_FS_BLK, microkit_msginfo_new(0, 0));
        volatile uint8_t *src = (volatile uint8_t *)(blk_data + BLK_DATA);
        for (uint32_t i = 0; i < batch * 512; i++) dst[i] = src[i];
        dst += batch * 512;
        sector += batch;
        count -= batch;
    }
}
static void blk_read_sector(uint32_t sector) {
    blk_read_sectors(sector, 1, sector_buf);
}

static void blk_write_sector(uint32_t sector) {
    volatile uint8_t *dst = (volatile uint8_t *)(blk_data + BLK_DATA);
    for (int i = 0; i < 512; i++) dst[i] = sector_buf[i];
    WR32(blk_data, BLK_CMD, BLK_CMD_WRITE);
    WR32(blk_data, BLK_SECTOR, sector);
    microkit_ppcall(CH_FS_BLK, microkit_msginfo_new(0, 0));
}



/* ── FAT16 geometry ────────────────────────────────────── */
static uint16_t bytes_per_sector;
static uint8_t  sectors_per_cluster;
static uint16_t reserved_sectors;
static uint8_t  num_fats;
static uint16_t root_entry_count;
static uint16_t sectors_per_fat;
static uint32_t root_dir_start;
static uint32_t root_dir_sectors;
static uint32_t data_start;
static uint32_t total_clusters;

static void parse_bpb(void) {
    blk_read_sector(0);
    bytes_per_sector    = rd16(sector_buf + 11);
    sectors_per_cluster = sector_buf[13];
    reserved_sectors    = rd16(sector_buf + 14);
    num_fats            = sector_buf[16];
    root_entry_count    = rd16(sector_buf + 17);
    sectors_per_fat     = rd16(sector_buf + 22);
    root_dir_sectors    = ((uint32_t)root_entry_count * 32 + 511) / 512;
    root_dir_start      = reserved_sectors + (uint32_t)num_fats * sectors_per_fat;
    data_start          = root_dir_start + root_dir_sectors;
    uint16_t total_sec  = rd16(sector_buf + 19);
    if (total_sec == 0) total_sec = (uint16_t)rd32(sector_buf + 32);
    total_clusters      = (total_sec - data_start) / sectors_per_cluster;
}

static uint32_t cluster_to_sector(uint16_t cluster) {
    return data_start + ((uint32_t)(cluster - 2)) * sectors_per_cluster;
}

static uint32_t cached_fat_sector = 0xFFFFFFFF;
static uint8_t fat_cache[512];

static uint16_t fat_read(uint16_t cluster) {
    uint32_t fat_offset = cluster * 2;
    uint32_t fat_sector = reserved_sectors + fat_offset / 512;
    if (fat_sector != cached_fat_sector) {
        blk_read_sector(fat_sector);
        for (int i = 0; i < 512; i++) fat_cache[i] = sector_buf[i];
        cached_fat_sector = fat_sector;
    }
    return rd16(fat_cache + (fat_offset % 512));
}

static void fat_write(uint16_t cluster, uint16_t value) {
    cached_fat_sector = 0xFFFFFFFF;  /* invalidate cache */
    for (int f = 0; f < num_fats; f++) {
        uint32_t fat_offset = cluster * 2;
        uint32_t fat_sector = reserved_sectors + f * sectors_per_fat + fat_offset / 512;
        blk_read_sector(fat_sector);
        wr16(sector_buf + (fat_offset % 512), value);
        blk_write_sector(fat_sector);
    }
}

/* ── 8.3 filename conversion ───────────────────────────── */
static void to_name83(const char *name, uint8_t *out) {
    my_memset(out, ' ', 11);
    int i = 0, o = 0;
    while (name[i] && name[i] != '.' && o < 8)
        out[o++] = my_toupper(name[i++]);
    if (name[i] == '.') i++;
    o = 8;
    while (name[i] && o < 11)
        out[o++] = my_toupper(name[i++]);
}

/* ── Open file table ───────────────────────────────────── */
#define MAX_OPEN 4
typedef struct {
    int      active;
    uint8_t  name83[11];
    uint16_t start_cluster;
    uint32_t file_size;
    uint32_t file_pos;
    uint16_t cur_cluster;
    uint32_t cur_cluster_idx;
    uint16_t dir_entry_idx;
    int      dirty;
    int      writable;
} open_file_t;

static open_file_t open_files[MAX_OPEN];

/* ── Reply helpers ─────────────────────────────────────── */
static void reply_status(int status) {
    WR32(fs_data, FS_STATUS, (uint32_t)status);
    microkit_notify(CH_FS);
}

/* ── OPEN ──────────────────────────────────────────────── */
static void handle_open(void) {
    char *name = (char *)(fs_data + FS_FILENAME);
    uint8_t name83[11];
    to_name83(name, name83);

    /* Find free slot */
    int fd = -1;
    for (int i = 0; i < MAX_OPEN; i++) {
        if (!open_files[i].active) { fd = i; break; }
    }
    if (fd < 0) { reply_status(-1); return; }

    /* Scan root directory */
    for (uint32_t sec = 0; sec < root_dir_sectors; sec++) {
        blk_read_sector(root_dir_start + sec);
        int entries_per_sector = 512 / 32;
        for (int i = 0; i < entries_per_sector; i++) {
            uint8_t *ent = sector_buf + i * 32;
            if (ent[0] == 0x00) { reply_status(-2); return; }     /* end */
            if (ent[0] == 0xE5) continue;                          /* deleted */
            if (ent[11] == 0x0F) continue;                         /* LFN */
            if (ent[11] & 0x08) continue;                          /* volume */

            if (my_memcmp(ent, name83, 11) == 0) {
                open_file_t *f = &open_files[fd];
                f->active          = 1;
                my_memcpy(f->name83, ent, 11);
                f->start_cluster   = rd16(ent + 26);
                f->file_size       = rd32(ent + 28);
                f->file_pos        = 0;
                f->cur_cluster     = f->start_cluster;
                f->cur_cluster_idx = 0;
                f->dir_entry_idx   = sec * entries_per_sector + i;
                f->dirty           = 0;
                f->writable        = 0;

                WR32(fs_data, FS_FD, (uint32_t)fd);
                WR32(fs_data, FS_FILESIZE, f->file_size);
                reply_status(0);
                return;
            }
        }
    }
    reply_status(-2);  /* not found */
}

/* ── READ (optimized: reads multiple sectors per PPC) ──── */
static void handle_read(void) {
    uint32_t fd     = RD32(fs_data, FS_FD);
    uint32_t offset = RD32(fs_data, FS_OFFSET);
    uint32_t len    = RD32(fs_data, FS_LENGTH);

    if (fd >= MAX_OPEN || !open_files[fd].active) {
        WR32(fs_data, FS_LENGTH, 0);
        reply_status(-1);
        return;
    }

    open_file_t *f = &open_files[fd];
    if (offset >= f->file_size) {
        WR32(fs_data, FS_LENGTH, 0);
        reply_status(FS_ST_EOF);
        return;
    }

    if (offset + len > f->file_size) len = f->file_size - offset;
    if (len > FS_DATA_MAX) len = FS_DATA_MAX;

    uint32_t bytes_per_cluster = (uint32_t)sectors_per_cluster * 512;

    /* Navigate to starting cluster */
    uint32_t target_cluster_idx = offset / bytes_per_cluster;
    uint16_t cluster = f->start_cluster;
    for (uint32_t ci = 0; ci < target_cluster_idx; ci++) {
        cluster = fat_read(cluster);
        if (cluster >= 0xFFF8 || cluster < 2) {
            WR32(fs_data, FS_LENGTH, 0);
            reply_status(FS_ST_EOF);
            return;
        }
    }

    volatile uint8_t *dst = (volatile uint8_t *)(fs_data + FS_DATA);
    uint32_t copied = 0;
    uint32_t off_in_cluster = offset % bytes_per_cluster;

    while (copied < len && cluster >= 2 && cluster < 0xFFF8) {
        /* How many sectors remain in this cluster? */
        uint32_t sec_in_cluster = off_in_cluster / 512;
        uint32_t byte_in_sector = off_in_cluster % 512;
        uint32_t secs_left = sectors_per_cluster - sec_in_cluster;
        uint32_t start_sector = cluster_to_sector(cluster) + sec_in_cluster;

        /* Check if next clusters are consecutive — but limit scan */
        uint32_t max_extra_secs = BLK_DATA_MAX / 512 - secs_left;
        uint16_t scan = cluster;
        uint16_t next = fat_read(scan);
        while (max_extra_secs >= sectors_per_cluster &&
               next == scan + 1 && next >= 2 && next < 0xFFF8) {
            secs_left += sectors_per_cluster;
            max_extra_secs -= sectors_per_cluster;
            scan = next;
            next = fat_read(next);
        }

        /* Cap to what fits in blk_data */
        if (secs_left > BLK_DATA_MAX / 512)
            secs_left = BLK_DATA_MAX / 512;

        /* Read sectors */
        WR32(blk_data, BLK_CMD, BLK_CMD_READ);
        WR32(blk_data, BLK_SECTOR, start_sector);
        WR32(blk_data, BLK_COUNT, secs_left);
        microkit_ppcall(CH_FS_BLK, microkit_msginfo_new(0, 0));

        volatile uint8_t *src = (volatile uint8_t *)(blk_data + BLK_DATA);
        uint32_t avail = secs_left * 512 - byte_in_sector;
        uint32_t need = len - copied;
        uint32_t chunk = (avail < need) ? avail : need;

        for (uint32_t i = 0; i < chunk; i++)
            dst[copied + i] = src[byte_in_sector + i];
        copied += chunk;

        /* Advance cluster pointer past what we just read */
        uint32_t bytes_consumed = off_in_cluster + chunk;
        uint32_t clusters_consumed = bytes_consumed / bytes_per_cluster;
        off_in_cluster = bytes_consumed % bytes_per_cluster;

        /* Walk the FAT chain forward */
        for (uint32_t ci = 0; ci < clusters_consumed; ci++) {
            uint16_t nx = fat_read(cluster);
            cluster = nx;
            if (cluster >= 0xFFF8 || cluster < 2) break;
        }
    }

    WR32(fs_data, FS_LENGTH, copied);
    reply_status((copied < len) ? FS_ST_EOF : FS_ST_OK);
}

/* ── CLOSE ─────────────────────────────────────────────── */
static void handle_close(void) {
    uint32_t fd = RD32(fs_data, FS_FD);
    if (fd < MAX_OPEN) {
        open_files[fd].active = 0;
    }
    reply_status(0);
}

/* ── CREATE ────────────────────────────────────────────── */
static void handle_create(void) {
    char *name = (char *)(fs_data + FS_FILENAME);
    uint8_t name83[11];
    to_name83(name, name83);

    /* Find a free directory entry */
    int found_slot = -1;
    uint32_t slot_sec = 0;
    int slot_idx = 0;

    for (uint32_t sec = 0; sec < root_dir_sectors; sec++) {
        blk_read_sector(root_dir_start + sec);
        int entries_per_sector = 512 / 32;
        for (int i = 0; i < entries_per_sector; i++) {
            uint8_t *ent = sector_buf + i * 32;
            /* Delete existing file with same name */
            if (ent[0] != 0x00 && ent[0] != 0xE5 && my_memcmp(ent, name83, 11) == 0) {
                /* Free its clusters */
                uint16_t cl = rd16(ent + 26);
                while (cl >= 2 && cl < 0xFFF8) {
                    uint16_t next = fat_read(cl);
                    fat_write(cl, 0x0000);
                    cl = next;
                }
                ent[0] = 0xE5;
                blk_write_sector(root_dir_start + sec);
            }
            if (found_slot < 0 && (ent[0] == 0x00 || ent[0] == 0xE5)) {
                found_slot = sec * entries_per_sector + i;
                slot_sec = sec;
                slot_idx = i;
            }
        }
    }

    if (found_slot < 0) { reply_status(-1); return; }

    /* Allocate first cluster */
    uint16_t new_cluster = 0;
    for (uint16_t c = 2; c < 2 + total_clusters; c++) {
        if (fat_read(c) == 0x0000) { new_cluster = c; break; }
    }
    if (new_cluster == 0) { reply_status(-1); return; }

    fat_write(new_cluster, 0xFFFF);

    /* Write directory entry */
    blk_read_sector(root_dir_start + slot_sec);
    uint8_t *ent = sector_buf + slot_idx * 32;
    my_memset(ent, 0, 32);
    my_memcpy(ent, name83, 11);
    ent[11] = 0x20;  /* archive */
    wr16(ent + 26, new_cluster);
    wr32(ent + 28, 0);  /* size = 0, will update on write */
    blk_write_sector(root_dir_start + slot_sec);

    /* Open the file */
    int fd = -1;
    for (int i = 0; i < MAX_OPEN; i++) {
        if (!open_files[i].active) { fd = i; break; }
    }
    if (fd < 0) { reply_status(-1); return; }

    open_file_t *f = &open_files[fd];
    f->active          = 1;
    my_memcpy(f->name83, name83, 11);
    f->start_cluster   = new_cluster;
    f->file_size       = 0;
    f->file_pos        = 0;
    f->cur_cluster     = new_cluster;
    f->cur_cluster_idx = 0;
    f->dir_entry_idx   = found_slot;
    f->dirty           = 0;
    f->writable        = 1;

    WR32(fs_data, FS_FD, (uint32_t)fd);
    reply_status(0);
}

/* ── WRITE ─────────────────────────────────────────────── */
static void handle_write(void) {
    uint32_t fd  = RD32(fs_data, FS_FD);
    uint32_t len = RD32(fs_data, FS_LENGTH);

    if (fd >= MAX_OPEN || !open_files[fd].active) {
        reply_status(-1);
        return;
    }

    open_file_t *f = &open_files[fd];
    if (len > FS_DATA_MAX) len = FS_DATA_MAX;

    volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
    uint32_t written = 0;
    uint16_t cluster = f->cur_cluster;
    uint32_t bytes_per_cluster = (uint32_t)sectors_per_cluster * 512;

    while (written < len) {
        if (cluster < 2 || cluster >= 0xFFF8) {
            /* Allocate new cluster */
            uint16_t new_cl = 0;
            for (uint16_t c = 2; c < 2 + total_clusters; c++) {
                if (fat_read(c) == 0x0000) { new_cl = c; break; }
            }
            if (new_cl == 0) break;  /* disk full */

            fat_write(new_cl, 0xFFFF);
            if (f->cur_cluster >= 2 && f->cur_cluster < 0xFFF8)
                fat_write(f->cur_cluster, new_cl);
            else
                f->start_cluster = new_cl;
            cluster = new_cl;
        }

        uint32_t off_in_cluster = f->file_pos % bytes_per_cluster;
        uint32_t sec_in_cluster = off_in_cluster / 512;
        uint32_t off_in_sector  = off_in_cluster % 512;
        uint32_t sector = cluster_to_sector(cluster) + sec_in_cluster;

        /* Read-modify-write if partial sector */
        if (off_in_sector != 0 || (len - written) < 512)
            blk_read_sector(sector);
        else
            my_memset(sector_buf, 0, 512);

        uint32_t space = 512 - off_in_sector;
        uint32_t chunk = (len - written < space) ? len - written : space;

        for (uint32_t i = 0; i < chunk; i++)
            sector_buf[off_in_sector + i] = src[written + i];

        blk_write_sector(sector);
        written += chunk;
        f->file_pos += chunk;

        if ((f->file_pos % bytes_per_cluster) == 0) {
            f->cur_cluster = cluster;
            cluster = fat_read(cluster);
        }
    }

    f->cur_cluster = cluster;
    f->file_size = f->file_pos;  /* update size */
    f->dirty = 1;

    /* Update directory entry with new size */
    uint32_t dir_sec = f->dir_entry_idx / (512 / 32);
    uint32_t dir_off = (f->dir_entry_idx % (512 / 32)) * 32;
    blk_read_sector(root_dir_start + dir_sec);
    wr16(sector_buf + dir_off + 26, f->start_cluster);
    wr32(sector_buf + dir_off + 28, f->file_size);
    blk_write_sector(root_dir_start + dir_sec);

    WR32(fs_data, FS_LENGTH, written);
    reply_status(0);
}

/* ── DELETE ─────────────────────────────────────────────── */
static void handle_delete(void) {
    char *name = (char *)(fs_data + FS_FILENAME);
    uint8_t name83[11];
    to_name83(name, name83);

    for (uint32_t sec = 0; sec < root_dir_sectors; sec++) {
        blk_read_sector(root_dir_start + sec);
        int entries_per_sector = 512 / 32;
        for (int i = 0; i < entries_per_sector; i++) {
            uint8_t *ent = sector_buf + i * 32;
            if (ent[0] == 0x00) { reply_status(-2); return; }
            if (ent[0] == 0xE5 || ent[11] == 0x0F || (ent[11] & 0x08)) continue;

            if (my_memcmp(ent, name83, 11) == 0) {
                /* Free cluster chain */
                uint16_t cl = rd16(ent + 26);
                while (cl >= 2 && cl < 0xFFF8) {
                    uint16_t next = fat_read(cl);
                    fat_write(cl, 0x0000);
                    cl = next;
                }
                /* Mark entry deleted */
                blk_read_sector(root_dir_start + sec);
                ent = sector_buf + i * 32;
                ent[0] = 0xE5;
                blk_write_sector(root_dir_start + sec);
                reply_status(0);
                return;
            }
        }
    }
    reply_status(-2);
}

/* ── LIST (new!) ───────────────────────────────────────── */
static void handle_list(void) {
    volatile uint8_t *dst = (volatile uint8_t *)(fs_data + FS_DATA);
    uint32_t pos = 0;
    uint32_t file_count = 0;

    for (uint32_t sec = 0; sec < root_dir_sectors; sec++) {
        blk_read_sector(root_dir_start + sec);
        int entries_per_sector = 512 / 32;
        for (int i = 0; i < entries_per_sector; i++) {
            uint8_t *ent = sector_buf + i * 32;
            if (ent[0] == 0x00) goto done;
            if (ent[0] == 0xE5) continue;
            if (ent[11] == 0x0F) continue;
            if (ent[11] & 0x08) continue;  /* skip volume label */

            /* Copy 8.3 name (11 bytes) + attr (1 byte) + size (4 bytes) = 16 bytes per entry */
            if (pos + 16 > FS_DATA_MAX) goto done;
            for (int j = 0; j < 11; j++) dst[pos++] = ent[j];
            dst[pos++] = ent[11]; /* attribute byte */
            uint32_t sz = rd32(ent + 28);
            dst[pos++] = sz & 0xFF;
            dst[pos++] = (sz >> 8) & 0xFF;
            dst[pos++] = (sz >> 16) & 0xFF;
            dst[pos++] = (sz >> 24) & 0xFF;
            file_count++;
        }
    }
done:
    WR32(fs_data, FS_LENGTH, file_count);
    reply_status(0);
}

/* ── Microkit entry points ─────────────────────────────── */
void init(void) {
    microkit_dbg_puts("FS: reading BPB...\n");
    parse_bpb();
    microkit_dbg_puts("FS: FAT16 init OK, clusters=");
    /* can't easily print number here without helper, just confirm */
    microkit_dbg_puts("\n");
}

void notified(microkit_channel ch) {
    (void)ch;
    /* All FS operations now handled via PPC */
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch;
    (void)msginfo;

    uint32_t cmd = RD32(fs_data, FS_CMD);
    switch (cmd) {
    case FS_CMD_OPEN:   handle_open();   break;
    case FS_CMD_READ:   handle_read();   break;
    case FS_CMD_CLOSE:  handle_close();  break;
    case FS_CMD_CREATE: handle_create(); break;
    case FS_CMD_WRITE:  handle_write();  break;
    case FS_CMD_DELETE: handle_delete(); break;
    case FS_CMD_LIST:   handle_list();   break;
    default: reply_status(-1); break;
    }

    return microkit_msginfo_new(0, 0);
}
