/*
 * AIOS FAT16 Filesystem Backend
 *
 * Extracted from the monolithic fs_server.c.
 * Implements aios_fs_ops for FAT16 volumes.
 */
#include <stdint.h>
#include <microkit.h>
#include "aios/vfs.h"

/* ── Block I/O (provided at mount time) ───────────────── */
static const blk_io_t *blk;
static uint8_t sector_buf[512];

/* ── Helpers ──────────────────────────────────────────── */
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
static uint32_t rd32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF;
}

/* ── FAT16 geometry ───────────────────────────────────── */
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

static uint32_t cluster_to_sector(uint16_t cluster) {
    return data_start + (cluster - 2) * sectors_per_cluster;
}

/* ── FAT cache ────────────────────────────────────────── */
static uint32_t cached_fat_sector = 0xFFFFFFFF;
static uint8_t fat_cache[512];

static uint16_t fat_read(uint16_t cluster) {
    uint32_t offset = cluster * 2;
    uint32_t sec = reserved_sectors + (offset / 512);
    if (sec != cached_fat_sector) {
        blk->read_sector(sec, fat_cache);
        cached_fat_sector = sec;
    }
    return rd16(fat_cache + (offset % 512));
}

static void fat_write(uint16_t cluster, uint16_t value) {
    uint32_t offset = cluster * 2;
    uint32_t sec = reserved_sectors + (offset / 512);
    if (sec != cached_fat_sector) {
        blk->read_sector(sec, fat_cache);
        cached_fat_sector = sec;
    }
    wr16(fat_cache + (offset % 512), value);
    blk->write_sector(sec, fat_cache);
    /* Mirror to FAT2 */
    if (num_fats > 1)
        blk->write_sector(sec + sectors_per_fat, fat_cache);
}

/* ── 8.3 name conversion ─────────────────────────────── */
static void to_name83(const char *name, uint8_t *out) {
    my_memset(out, ' ', 11);
    int i = 0, o = 0;
    while (name[i] && name[i] != '.' && o < 8)
        out[o++] = my_toupper(name[i++]);
    if (name[i] == '.') {
        i++;
        o = 8;
        while (name[i] && o < 11)
            out[o++] = my_toupper(name[i++]);
    }
}

/* ── Find entry in root directory ─────────────────────── */
static int find_entry(const char *filename, uint16_t *cluster_out,
                      uint32_t *size_out, uint8_t *dir_sec_out,
                      uint8_t *dir_idx_out) {
    uint8_t name83[11];
    to_name83(filename, name83);

    for (uint32_t sec = 0; sec < root_dir_sectors; sec++) {
        blk->read_sector(root_dir_start + sec, sector_buf);
        int entries = 512 / 32;
        for (int i = 0; i < entries; i++) {
            uint8_t *ent = sector_buf + i * 32;
            if (ent[0] == 0x00) return -1;
            if (ent[0] == 0xE5) continue;
            if (ent[11] == 0x0F) continue;
            if (my_memcmp(ent, name83, 11) == 0) {
                *cluster_out = rd16(ent + 26);
                *size_out = rd32(ent + 28);
                if (dir_sec_out) *dir_sec_out = (uint8_t)sec;
                if (dir_idx_out) *dir_idx_out = (uint8_t)i;
                return 0;
            }
        }
    }
    return -1;
}

/* ── Allocate a free cluster ──────────────────────────── */
static uint16_t alloc_cluster(void) {
    for (uint16_t c = 2; c < total_clusters + 2; c++) {
        if (fat_read(c) == 0x0000) {
            fat_write(c, 0xFFFF);
            return c;
        }
    }
    return 0;
}

/* ── Free a cluster chain ─────────────────────────────── */
static void free_chain(uint16_t cluster) {
    while (cluster >= 2 && cluster < 0xFFF8) {
        uint16_t next = fat_read(cluster);
        fat_write(cluster, 0x0000);
        cluster = next;
    }
}

/* ═══════════════════════════════════════════════════════
 *  VFS ops implementation
 * ═══════════════════════════════════════════════════════ */

static int fat16_mount(const blk_io_t *b) {
    blk = b;
    blk->read_sector(0, sector_buf);

    bytes_per_sector    = rd16(sector_buf + 11);
    sectors_per_cluster = sector_buf[13];
    reserved_sectors    = rd16(sector_buf + 14);
    num_fats            = sector_buf[16];
    root_entry_count    = rd16(sector_buf + 17);
    sectors_per_fat     = rd16(sector_buf + 22);


    if (bytes_per_sector != 512 || sectors_per_cluster == 0) {
        microkit_dbg_puts("FAIL bps/spc\n");
        return -1;
    }

    root_dir_start  = reserved_sectors + num_fats * sectors_per_fat;
    root_dir_sectors = (root_entry_count * 32 + 511) / 512;
    data_start = root_dir_start + root_dir_sectors;

    uint32_t total_sectors = rd16(sector_buf + 19);
    if (total_sectors == 0) total_sectors = rd32(sector_buf + 32);
    total_clusters = (total_sectors - data_start) / sectors_per_cluster;

    /* Sanity: FAT16 has < 65525 clusters */
    if (total_clusters >= 65525) return -1;

    cached_fat_sector = 0xFFFFFFFF;
    return 0;
}

static int fat16_open(const char *filename, open_file_t *fd) {
    uint16_t cluster;
    uint32_t size;
    uint8_t dsec, didx;
    if (find_entry(filename, &cluster, &size, &dsec, &didx) != 0)
        return -1;
    fd->in_use = 1;
    fd->start_cluster = cluster;
    fd->file_size = size;
    fd->offset = 0;
    fd->dir_sector = dsec;
    fd->dir_index = didx;
    to_name83(filename, fd->name83);
    return 0;
}

static int fat16_read(open_file_t *fd, uint8_t *buf, uint32_t offset,
                      uint32_t len, uint32_t *bytes_read) {
    if (offset >= fd->file_size) { *bytes_read = 0; return 0; }
    if (offset + len > fd->file_size) len = fd->file_size - offset;

    uint32_t cluster_size = sectors_per_cluster * 512;
    uint16_t cluster = fd->start_cluster;

    /* Skip to the cluster containing 'offset' */
    uint32_t skip = offset / cluster_size;
    for (uint32_t i = 0; i < skip; i++) {
        cluster = fat_read(cluster);
        if (cluster >= 0xFFF8) { *bytes_read = 0; return 0; }
    }

    uint32_t pos = 0;
    uint32_t off_in_cluster = offset % cluster_size;

    while (pos < len && cluster >= 2 && cluster < 0xFFF8) {
        uint32_t sec_base = cluster_to_sector(cluster);
        uint32_t sec_off = off_in_cluster / 512;
        uint32_t byte_off = off_in_cluster % 512;

        while (sec_off < sectors_per_cluster && pos < len) {
            blk->read_sector(sec_base + sec_off, sector_buf);
            uint32_t chunk = 512 - byte_off;
            if (chunk > len - pos) chunk = len - pos;
            my_memcpy(buf + pos, sector_buf + byte_off, chunk);
            pos += chunk;
            byte_off = 0;
            sec_off++;
        }
        off_in_cluster = 0;
        cluster = fat_read(cluster);
    }
    *bytes_read = pos;
    return 0;
}

static int fat16_close(open_file_t *fd) {
    fd->in_use = 0;
    return 0;
}

static int fat16_create(const char *filename, open_file_t *fd) {
    uint8_t name83[11];
    to_name83(filename, name83);

    /* Find free directory entry */
    for (uint32_t sec = 0; sec < root_dir_sectors; sec++) {
        blk->read_sector(root_dir_start + sec, sector_buf);
        int entries = 512 / 32;
        for (int i = 0; i < entries; i++) {
            uint8_t *ent = sector_buf + i * 32;
            if (ent[0] == 0x00 || ent[0] == 0xE5) {
                /* Allocate first cluster */
                uint16_t cluster = alloc_cluster();
                if (cluster == 0) return -1;

                my_memset(ent, 0, 32);
                my_memcpy(ent, name83, 11);
                ent[11] = 0x20; /* archive */
                wr16(ent + 26, cluster);
                wr32(ent + 28, 0);
                blk->write_sector(root_dir_start + sec, sector_buf);

                fd->in_use = 1;
                fd->start_cluster = cluster;
                fd->file_size = 0;
                fd->offset = 0;
                fd->dir_sector = (uint8_t)sec;
                fd->dir_index = (uint8_t)i;
                my_memcpy(fd->name83, name83, 11);
                return 0;
            }
        }
    }
    return -1; /* no space */
}

static int fat16_write(open_file_t *fd, const uint8_t *data,
                       uint32_t len, uint32_t *bytes_written) {
    uint32_t cluster_size = sectors_per_cluster * 512;
    uint16_t cluster = fd->start_cluster;
    uint16_t prev = 0;
    uint32_t pos = 0;

    /* Walk to end of chain */
    while (cluster >= 2 && cluster < 0xFFF8 && fd->file_size > 0) {
        uint32_t remaining = fd->file_size - (pos > fd->file_size ? fd->file_size : pos);
        if (remaining == 0) break;
        prev = cluster;
        cluster = fat_read(cluster);
        pos += cluster_size;
    }

    /* Append data */
    uint32_t written = 0;
    cluster = (prev && fat_read(prev) >= 0xFFF8) ? prev : fd->start_cluster;
    uint32_t off_in_cluster = fd->file_size % cluster_size;
    uint32_t sec_in_cluster = off_in_cluster / 512;
    uint32_t byte_in_sec = off_in_cluster % 512;

    while (written < len) {
        if (off_in_cluster >= cluster_size || (fd->file_size > 0 && off_in_cluster == 0 && written > 0)) {
            uint16_t next = alloc_cluster();
            if (next == 0) break;
            fat_write(cluster, next);
            cluster = next;
            off_in_cluster = 0;
            sec_in_cluster = 0;
            byte_in_sec = 0;
        }

        uint32_t sector = cluster_to_sector(cluster) + sec_in_cluster;
        if (byte_in_sec > 0) {
            blk->read_sector(sector, sector_buf);
        } else {
            my_memset(sector_buf, 0, 512);
        }

        uint32_t chunk = 512 - byte_in_sec;
        if (chunk > len - written) chunk = len - written;
        my_memcpy(sector_buf + byte_in_sec, data + written, chunk);
        blk->write_sector(sector, sector_buf);

        written += chunk;
        byte_in_sec = 0;
        sec_in_cluster++;
        off_in_cluster += chunk;
    }

    /* Update directory entry with new size */
    fd->file_size += written;
    blk->read_sector(root_dir_start + fd->dir_sector, sector_buf);
    uint8_t *ent = sector_buf + fd->dir_index * 32;
    wr32(ent + 28, fd->file_size);
    blk->write_sector(root_dir_start + fd->dir_sector, sector_buf);

    *bytes_written = written;
    return 0;
}

static int fat16_delete(const char *filename) {
    uint8_t name83[11];
    to_name83(filename, name83);

    for (uint32_t sec = 0; sec < root_dir_sectors; sec++) {
        blk->read_sector(root_dir_start + sec, sector_buf);
        int entries = 512 / 32;
        for (int i = 0; i < entries; i++) {
            uint8_t *ent = sector_buf + i * 32;
            if (ent[0] == 0x00) return -1;
            if (ent[0] == 0xE5) continue;
            if (my_memcmp(ent, name83, 11) == 0) {
                uint16_t cluster = rd16(ent + 26);
                free_chain(cluster);
                ent[0] = 0xE5;
                blk->write_sector(root_dir_start + sec, sector_buf);
                return 0;
            }
        }
    }
    return -1;
}

static int fat16_list(uint8_t *buf, uint32_t buf_size, uint32_t *count) {
    uint32_t pos = 0;
    uint32_t file_count = 0;

    for (uint32_t sec = 0; sec < root_dir_sectors; sec++) {
        blk->read_sector(root_dir_start + sec, sector_buf);
        int entries = 512 / 32;
        for (int i = 0; i < entries; i++) {
            uint8_t *ent = sector_buf + i * 32;
            if (ent[0] == 0x00) goto done;
            if (ent[0] == 0xE5) continue;
            if (ent[11] == 0x0F) continue;
            if (ent[11] & 0x08) continue;

            if (pos + 16 > buf_size) goto done;
            my_memcpy(buf + pos, ent, 11);     /* 8.3 name */
            buf[pos + 11] = ent[11];            /* attr     */
            uint32_t sz = rd32(ent + 28);
            buf[pos+12] = sz & 0xFF;
            buf[pos+13] = (sz >> 8) & 0xFF;
            buf[pos+14] = (sz >> 16) & 0xFF;
            buf[pos+15] = (sz >> 24) & 0xFF;
            pos += 16;
            file_count++;
        }
    }
done:
    *count = file_count;
    return 0;
}

static int fat16_sync(void) { return 0; }

static int fat16_stat(const char *filename, uint32_t *size_out) {
    uint16_t cluster;
    uint8_t dsec, didx;
    return find_entry(filename, &cluster, size_out, &dsec, &didx);
}

/* ── Export the ops table ─────────────────────────────── */
const aios_fs_ops_t fat16_ops = {
    .name   = "FAT16",
    .mount  = fat16_mount,
    .open   = fat16_open,
    .read   = fat16_read,
    .close  = fat16_close,
    .create = fat16_create,
    .write  = fat16_write,
    .delete = fat16_delete,
    .list   = fat16_list,
    .sync   = fat16_sync,
    .stat   = fat16_stat,
};

/* ── Probe function: check if volume is FAT16 ────────── */
const aios_fs_ops_t *fat16_probe(const blk_io_t *b) {
    uint8_t tmp[512];
    b->read_sector(0, tmp);


    uint16_t bps = rd16(tmp + 11);
    uint8_t spc = tmp[13];
    if (bps != 512 || spc == 0) return 0;

    /* Check for FAT16 signature at offset 54 */
    if (tmp[54]=='F' && tmp[55]=='A' && tmp[56]=='T' &&
        tmp[57]=='1' && tmp[58]=='6') {
        return &fat16_ops;
    }

    /* Heuristic: count clusters */
    uint16_t nfats = tmp[16];
    uint16_t spf = rd16(tmp + 22);
    uint16_t rsvd = rd16(tmp + 14);
    uint16_t rde = rd16(tmp + 17);
    uint32_t rds = (rde * 32 + 511) / 512;
    uint32_t ds = rsvd + nfats * spf + rds;
    uint16_t ts = rd16(tmp + 19);
    if (ts == 0) ts = (uint16_t)rd32(tmp + 32);
    uint32_t tc = (ts - ds) / spc;
    if (tc >= 4085 && tc < 65525) return &fat16_ops;

    return 0;
}
