/*
 * AIOS FAT32 Filesystem Backend
 *
 * Implements aios_fs_ops for FAT32 volumes.
 * Key differences from FAT16:
 *   - 32-bit FAT entries (28 bits used)
 *   - No fixed root directory; root is a normal cluster chain
 *   - BPB extended fields at offset 36+
 *   - Sectors per FAT from 32-bit field (offset 36)
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

/* ── FAT32 geometry ───────────────────────────────────── */
static uint16_t bytes_per_sector;
static uint8_t  sectors_per_cluster;
static uint16_t reserved_sectors;
static uint8_t  num_fats;
static uint32_t sectors_per_fat;
static uint32_t root_cluster;
static uint32_t data_start;
static uint32_t total_clusters;

static uint32_t cluster_to_sector(uint32_t cluster) {
    return data_start + (cluster - 2) * sectors_per_cluster;
}

/* ── FAT cache ────────────────────────────────────────── */
static uint32_t cached_fat_sector = 0xFFFFFFFF;
static uint8_t fat_cache[512];
static int fat_cache_dirty = 0;

static void fat_flush(void) {
    if (fat_cache_dirty && cached_fat_sector != 0xFFFFFFFF) {
        blk->write_sector(cached_fat_sector, fat_cache);
        if (num_fats > 1)
            blk->write_sector(cached_fat_sector + sectors_per_fat, fat_cache);
        fat_cache_dirty = 0;
    }
}

static void fat_load(uint32_t sec) {
    if (sec != cached_fat_sector) {
        fat_flush();
        blk->read_sector(sec, fat_cache);
        cached_fat_sector = sec;
    }
}

static uint32_t fat_read(uint32_t cluster) {
    uint32_t offset = cluster * 4;
    uint32_t sec = reserved_sectors + (offset / 512);
    fat_load(sec);
    return rd32(fat_cache + (offset % 512)) & 0x0FFFFFFF;
}

static void fat_write(uint32_t cluster, uint32_t value) {
    uint32_t offset = cluster * 4;
    uint32_t sec = reserved_sectors + (offset / 512);
    fat_load(sec);
    /* Preserve top 4 bits */
    uint32_t existing = rd32(fat_cache + (offset % 512));
    value = (existing & 0xF0000000) | (value & 0x0FFFFFFF);
    wr32(fat_cache + (offset % 512), value);
    fat_cache_dirty = 1;
    fat_flush();
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

/* ── Directory traversal (cluster chain) ──────────────── */

/* Callback: return 0 to continue, non-zero to stop */
typedef int (*dir_walk_fn)(uint8_t *entry, uint32_t dir_sector,
                           int entry_index, void *ctx);

static int walk_dir(uint32_t start_cluster, dir_walk_fn fn, void *ctx) {
    uint32_t cluster = start_cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t sec_base = cluster_to_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            blk->read_sector(sec_base + s, sector_buf);
            int entries = 512 / 32;
            for (int i = 0; i < entries; i++) {
                uint8_t *ent = sector_buf + i * 32;
                if (ent[0] == 0x00) return 0; /* end of directory */
                int rc = fn(ent, sec_base + s, i, ctx);
                if (rc != 0) return rc;
            }
        }
        cluster = fat_read(cluster);
    }
    return 0;
}

/* ── Find entry context ───────────────────────────────── */
typedef struct {
    uint8_t  name83[11];
    uint16_t cluster_hi;
    uint16_t cluster_lo;
    uint32_t file_size;
    uint32_t dir_sector;
    int      dir_index;
    int      found;
} find_ctx_t;

static int find_cb(uint8_t *ent, uint32_t dir_sector, int idx, void *ctx) {
    find_ctx_t *fc = (find_ctx_t *)ctx;
    if (ent[0] == 0xE5) return 0;
    if (ent[11] == 0x0F) return 0; /* LFN */
    if (my_memcmp(ent, fc->name83, 11) == 0) {
        fc->cluster_hi = rd16(ent + 20);
        fc->cluster_lo = rd16(ent + 26);
        fc->file_size  = rd32(ent + 28);
        fc->dir_sector = dir_sector;
        fc->dir_index  = idx;
        fc->found = 1;
        return 1; /* stop */
    }
    return 0;
}

static int find_entry(const char *filename, find_ctx_t *fc) {
    to_name83(filename, fc->name83);
    fc->found = 0;
    walk_dir(root_cluster, find_cb, fc);
    return fc->found ? 0 : -1;
}

static uint32_t fc_cluster(const find_ctx_t *fc) {
    return ((uint32_t)fc->cluster_hi << 16) | fc->cluster_lo;
}

/* ── Allocate a free cluster ──────────────────────────── */
static uint32_t alloc_cluster(void) {
    for (uint32_t c = 2; c < total_clusters + 2; c++) {
        if (fat_read(c) == 0) {
            fat_write(c, 0x0FFFFFFF);
            return c;
        }
    }
    return 0;
}

/* ── Free a cluster chain ─────────────────────────────── */
static void free_chain(uint32_t cluster) {
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t next = fat_read(cluster);
        fat_write(cluster, 0);
        cluster = next;
    }
}

/* ── Find free directory entry context ────────────────── */
typedef struct {
    uint32_t dir_sector;
    int      dir_index;
    int      found;
} free_slot_ctx_t;

static int free_slot_cb(uint8_t *ent, uint32_t dir_sector, int idx, void *ctx) {
    free_slot_ctx_t *fs = (free_slot_ctx_t *)ctx;
    if (ent[0] == 0x00 || ent[0] == 0xE5) {
        fs->dir_sector = dir_sector;
        fs->dir_index = idx;
        fs->found = 1;
        return 1;
    }
    return 0;
}

/* ── List context ─────────────────────────────────────── */
typedef struct {
    uint8_t  *buf;
    uint32_t  buf_size;
    uint32_t  pos;
    uint32_t  count;
} list_ctx_t;

static int list_cb(uint8_t *ent, uint32_t dir_sector, int idx, void *ctx) {
    (void)dir_sector; (void)idx;
    list_ctx_t *lc = (list_ctx_t *)ctx;
    if (ent[0] == 0xE5) return 0;
    if (ent[11] == 0x0F) return 0;
    if (ent[11] & 0x08) return 0; /* volume label */

    if (lc->pos + 16 > lc->buf_size) return 1;
    my_memcpy(lc->buf + lc->pos, ent, 11);
    lc->buf[lc->pos + 11] = ent[11];
    uint32_t sz = rd32(ent + 28);
    lc->buf[lc->pos + 12] = sz & 0xFF;
    lc->buf[lc->pos + 13] = (sz >> 8) & 0xFF;
    lc->buf[lc->pos + 14] = (sz >> 16) & 0xFF;
    lc->buf[lc->pos + 15] = (sz >> 24) & 0xFF;
    lc->pos += 16;
    lc->count++;
    return 0;
}

/* ═══════════════════════════════════════════════════════
 *  VFS ops implementation
 * ═══════════════════════════════════════════════════════ */

static int fat32_mount(const blk_io_t *b) {
    blk = b;
    blk->read_sector(0, sector_buf);

    bytes_per_sector    = rd16(sector_buf + 11);
    sectors_per_cluster = sector_buf[13];
    reserved_sectors    = rd16(sector_buf + 14);
    num_fats            = sector_buf[16];
    sectors_per_fat     = rd32(sector_buf + 36);
    root_cluster        = rd32(sector_buf + 44);

    if (bytes_per_sector != 512 || sectors_per_cluster == 0 || sectors_per_fat == 0)
        return -1;

    data_start = reserved_sectors + num_fats * sectors_per_fat;

    uint32_t total_sectors = rd32(sector_buf + 32);
    if (total_sectors == 0) total_sectors = rd16(sector_buf + 19);
    total_clusters = (total_sectors - data_start) / sectors_per_cluster;

    /* FAT32 detected by probe; accept any cluster count */

    cached_fat_sector = 0xFFFFFFFF;
    fat_cache_dirty = 0;
    return 0;
}

static int fat32_open(const char *filename, open_file_t *fd) {
    find_ctx_t fc;
    if (find_entry(filename, &fc) != 0) return -1;

    fd->in_use = 1;
    fd->start_cluster = (uint16_t)(fc_cluster(&fc) & 0xFFFF);
    fd->file_size = fc.file_size;
    fd->offset = 0;
    fd->dir_sector = (uint8_t)(fc.dir_sector & 0xFF);
    fd->dir_index = (uint8_t)(fc.dir_index & 0xFF);
    to_name83(filename, fd->name83);
    /* Store high cluster bits in a reserved area */
    /* We reuse dir_sector high bits — but for FAT32 we need
       the full 32-bit cluster. Store high 16 bits via offset field. */
    fd->offset = fc_cluster(&fc); /* temporarily store full start cluster */
    return 0;
}

static int fat32_read(open_file_t *fd, uint8_t *buf, uint32_t offset,
                      uint32_t len, uint32_t *bytes_read) {
    if (offset >= fd->file_size) { *bytes_read = 0; return 0; }
    if (offset + len > fd->file_size) len = fd->file_size - offset;

    uint32_t cluster_size = sectors_per_cluster * 512;
    uint32_t cluster = fd->offset; /* full 32-bit start cluster */

    /* Skip to the cluster containing 'offset' */
    uint32_t skip = offset / cluster_size;
    for (uint32_t i = 0; i < skip; i++) {
        cluster = fat_read(cluster);
        if (cluster >= 0x0FFFFFF8) { *bytes_read = 0; return 0; }
    }

    uint32_t pos = 0;
    uint32_t off_in_cluster = offset % cluster_size;

    while (pos < len && cluster >= 2 && cluster < 0x0FFFFFF8) {
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

static int fat32_close(open_file_t *fd) {
    fd->in_use = 0;
    return 0;
}

static int fat32_create(const char *filename, open_file_t *fd) {
    uint8_t name83[11];
    to_name83(filename, name83);

    /* Find free directory entry */
    free_slot_ctx_t fsc = {0, 0, 0};
    walk_dir(root_cluster, free_slot_cb, &fsc);

    if (!fsc.found) {
        /* TODO: extend root directory cluster chain */
        return -1;
    }

    uint32_t cluster = alloc_cluster();
    if (cluster == 0) return -1;

    /* Write directory entry */
    blk->read_sector(fsc.dir_sector, sector_buf);
    uint8_t *ent = sector_buf + fsc.dir_index * 32;
    my_memset(ent, 0, 32);
    my_memcpy(ent, name83, 11);
    ent[11] = 0x20;
    wr16(ent + 20, (uint16_t)(cluster >> 16));
    wr16(ent + 26, (uint16_t)(cluster & 0xFFFF));
    wr32(ent + 28, 0);
    blk->write_sector(fsc.dir_sector, sector_buf);

    fd->in_use = 1;
    fd->start_cluster = (uint16_t)(cluster & 0xFFFF);
    fd->file_size = 0;
    fd->offset = cluster; /* full 32-bit start cluster */
    fd->dir_sector = (uint8_t)(fsc.dir_sector & 0xFF);
    fd->dir_index = (uint8_t)fsc.dir_index;
    my_memcpy(fd->name83, name83, 11);
    return 0;
}

static int fat32_write(open_file_t *fd, const uint8_t *data,
                       uint32_t len, uint32_t *bytes_written) {
    uint32_t cluster_size = sectors_per_cluster * 512;
    uint32_t cluster = fd->offset; /* full 32-bit start cluster */
    uint32_t prev = 0;

    /* Walk to last cluster */
    uint32_t walked = 0;
    uint32_t cur = cluster;
    while (cur >= 2 && cur < 0x0FFFFFF8) {
        prev = cur;
        walked += cluster_size;
        if (walked >= fd->file_size) break;
        cur = fat_read(cur);
    }
    if (prev) cluster = prev;

    uint32_t written = 0;
    uint32_t off_in_cluster = fd->file_size % cluster_size;
    uint32_t sec_in_cluster = off_in_cluster / 512;
    uint32_t byte_in_sec = off_in_cluster % 512;

    while (written < len) {
        if (off_in_cluster >= cluster_size ||
            (fd->file_size > 0 && off_in_cluster == 0 && written > 0)) {
            uint32_t next = alloc_cluster();
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

    /* Update directory entry */
    fd->file_size += written;
    blk->read_sector(fd->dir_sector, sector_buf);
    uint8_t *ent = sector_buf + fd->dir_index * 32;
    wr32(ent + 28, fd->file_size);
    blk->write_sector(fd->dir_sector, sector_buf);

    *bytes_written = written;
    return 0;
}

static int fat32_delete(const char *filename) {
    find_ctx_t fc;
    if (find_entry(filename, &fc) != 0) return -1;

    free_chain(fc_cluster(&fc));

    blk->read_sector(fc.dir_sector, sector_buf);
    uint8_t *ent = sector_buf + fc.dir_index * 32;
    ent[0] = 0xE5;
    blk->write_sector(fc.dir_sector, sector_buf);
    return 0;
}

static int fat32_list(uint8_t *buf, uint32_t buf_size, uint32_t *count) {
    list_ctx_t lc = { buf, buf_size, 0, 0 };
    walk_dir(root_cluster, list_cb, &lc);
    *count = lc.count;
    return 0;
}

static int fat32_sync(void) {
    fat_flush();
    return 0;
}

static int fat32_stat(const char *filename, uint32_t *size_out) {
    find_ctx_t fc;
    if (find_entry(filename, &fc) != 0) return -1;
    *size_out = fc.file_size;
    return 0;
}

/* ── Export the ops table ─────────────────────────────── */
const aios_fs_ops_t fat32_ops = {
    .name   = "FAT32",
    .mount  = fat32_mount,
    .open   = fat32_open,
    .read   = fat32_read,
    .close  = fat32_close,
    .create = fat32_create,
    .write  = fat32_write,
    .delete = fat32_delete,
    .list   = fat32_list,
    .sync   = fat32_sync,
    .stat   = fat32_stat,
};

/* ── Probe function: check if volume is FAT32 ────────── */
const aios_fs_ops_t *fat32_probe(const blk_io_t *b) {
    uint8_t tmp[512];
    b->read_sector(0, tmp);

    uint16_t bps = rd16(tmp + 11);
    uint8_t spc = tmp[13];
    if (bps != 512 || spc == 0) return 0;

    /* Check for FAT32 signature at offset 82 */
    if (tmp[82]=='F' && tmp[83]=='A' && tmp[84]=='T' &&
        tmp[85]=='3' && tmp[86]=='2') {
        return &fat32_ops;
    }

    /* Heuristic: FAT32 has sectors_per_fat_16 == 0 and
       sectors_per_fat_32 > 0 */
    uint16_t spf16 = rd16(tmp + 22);
    uint32_t spf32 = rd32(tmp + 36);
    if (spf16 == 0 && spf32 > 0) {
        /* Verify cluster count is FAT32 range */
        uint16_t rsvd = rd16(tmp + 14);
        uint8_t nfats = tmp[16];
        uint32_t ds = rsvd + nfats * spf32;
        uint32_t ts = rd32(tmp + 32);
        if (ts == 0) ts = rd16(tmp + 19);
        uint32_t tc = (ts - ds) / spc;
        if (tc >= 65525) return &fat32_ops;
    }

    return 0;
}
