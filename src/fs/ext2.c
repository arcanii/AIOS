/*
 * AIOS ext2 Filesystem Backend
 *
 * Implements aios_fs_ops for ext2 volumes.
 *
 * ext2 layout (1K block size):
 *   Block 0: boot block (unused)
 *   Block 1: superblock
 *   Block 2: block group descriptor table
 *   Per group: block bitmap, inode bitmap, inode table, data blocks
 *
 * Inode addressing: 12 direct, 1 indirect, 1 double-indirect, 1 triple-indirect
 */
#include <stdint.h>
#include <microkit.h>
#include "aios/vfs.h"

/* ── Block I/O ────────────────────────────────────────── */
static const blk_io_t *blk;
static __attribute__((unused)) uint8_t blk_buf[1024];  /* one filesystem block */

/* ── Helpers ──────────────────────────────────────────── */
static void my_memset(void *dst, int c, int n) {
    uint8_t *d = (uint8_t *)dst;
    for (int i = 0; i < n; i++) d[i] = (uint8_t)c;
}
static void my_memcpy(void *dst, const void *src, int n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}
static __attribute__((unused)) int my_memcmp(const void *a, const void *b, int n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (int i = 0; i < n; i++) {
        if (x[i] != y[i]) return x[i] - y[i];
    }
    return 0;
}
static __attribute__((unused)) int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}
static inline __attribute__((unused)) uint8_t my_toupper(uint8_t c) {
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static uint16_t rd16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static __attribute__((unused)) void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static __attribute__((unused)) void wr32(uint8_t *p, uint32_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF;
}

/* ── ext2 constants ───────────────────────────────────── */
#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2
#define EXT2_NDIR_BLOCKS    12
#define EXT2_IND_BLOCK      12
#define EXT2_DIND_BLOCK     13
#define EXT2_TIND_BLOCK     14

/* Directory file types */
#define EXT2_FT_REG_FILE    1
#define EXT2_FT_DIR         2

/* Inode mode bits */
#define EXT2_S_IFREG        0x8000
#define EXT2_S_IFDIR        0x4000

/* ── Filesystem geometry ──────────────────────────────── */
static uint32_t block_size;
static uint16_t ext2_creator_uid = 0;
static uint16_t ext2_creator_gid = 0;
static uint32_t log_block_size;
static uint32_t inodes_per_group;
static uint32_t blocks_per_group;
static uint32_t inode_size;
static uint32_t total_inodes;
static uint32_t total_blocks;
static uint32_t first_data_block;
static uint32_t num_groups;

/* Block group descriptor cache */
#define MAX_GROUPS 128
static struct {
    uint32_t inode_table;
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint16_t free_blocks;
    uint16_t free_inodes;
} bg_cache[MAX_GROUPS];

/* ── Block I/O helpers ────────────────────────────────── */
static void read_block(uint32_t block, uint8_t *buf) {
    /* A filesystem block may span multiple 512-byte sectors */
    uint32_t sectors_per_block = block_size / 512;
    uint32_t start_sector = block * sectors_per_block;
    for (uint32_t i = 0; i < sectors_per_block; i++) {
        blk->read_sector(start_sector + i, buf + i * 512);
    }
}

static void write_block(uint32_t block, const uint8_t *buf) {
    uint32_t sectors_per_block = block_size / 512;
    uint32_t start_sector = block * sectors_per_block;
    for (uint32_t i = 0; i < sectors_per_block; i++) {
        blk->write_sector(start_sector + i, buf + i * 512);
    }
}

/* ── Inode operations ─────────────────────────────────── */

/* Read raw inode data into buf (inode_size bytes) */
static int read_inode(uint32_t ino, uint8_t *buf) {
    if (ino == 0 || ino > total_inodes) return -1;

    uint32_t group = (ino - 1) / inodes_per_group;
    uint32_t index = (ino - 1) % inodes_per_group;

    if (group >= num_groups) return -1;

    uint32_t inode_table_block = bg_cache[group].inode_table;
    uint32_t byte_offset = index * inode_size;
    uint32_t blk_in_table = byte_offset / block_size;
    uint32_t off_in_block = byte_offset % block_size;

    uint8_t tmp[1024];
    read_block(inode_table_block + blk_in_table, tmp);
    my_memcpy(buf, tmp + off_in_block, inode_size > 128 ? 128 : inode_size);
    return 0;
}

static void write_inode(uint32_t ino, const uint8_t *buf) {
    uint32_t group = (ino - 1) / inodes_per_group;
    uint32_t index = (ino - 1) % inodes_per_group;
    uint32_t inode_table_block = bg_cache[group].inode_table;
    uint32_t byte_offset = index * inode_size;
    uint32_t blk_in_table = byte_offset / block_size;
    uint32_t off_in_block = byte_offset % block_size;

    uint8_t tmp[1024];
    read_block(inode_table_block + blk_in_table, tmp);
    my_memcpy(tmp + off_in_block, buf, inode_size > 128 ? 128 : inode_size);
    write_block(inode_table_block + blk_in_table, tmp);
}

/* Parse inode fields */
static __attribute__((unused)) uint16_t inode_mode(const uint8_t *ino)  { return rd16(ino + 0); }
static uint16_t inode_uid(const uint8_t *ino)   { return rd16(ino + 2); }
static uint16_t inode_gid(const uint8_t *ino)   { return rd16(ino + 24); }
static uint32_t inode_size_field(const uint8_t *ino) { return rd32(ino + 4); }
static __attribute__((unused)) uint16_t inode_links(const uint8_t *ino)  { return rd16(ino + 26); }
static uint32_t inode_blocks(const uint8_t *ino) { return rd32(ino + 28); }
static uint32_t inode_block(const uint8_t *ino, int idx) {
    return rd32(ino + 40 + idx * 4);
}

/* ── Block addressing (handles indirect blocks) ──────── */
static uint32_t get_block_num(const uint8_t *inode_buf, uint32_t logical_block) {
    uint32_t ptrs_per_block = block_size / 4;

    /* Direct blocks (0-11) */
    if (logical_block < EXT2_NDIR_BLOCKS) {
        return inode_block(inode_buf, logical_block);
    }
    logical_block -= EXT2_NDIR_BLOCKS;

    /* Indirect block */
    if (logical_block < ptrs_per_block) {
        uint32_t ind = inode_block(inode_buf, EXT2_IND_BLOCK);
        if (ind == 0) return 0;
        uint8_t tmp[1024];
        read_block(ind, tmp);
        return rd32(tmp + logical_block * 4);
    }
    logical_block -= ptrs_per_block;

    /* Double indirect block */
    if (logical_block < ptrs_per_block * ptrs_per_block) {
        uint32_t dind = inode_block(inode_buf, EXT2_DIND_BLOCK);
        if (dind == 0) return 0;
        uint8_t tmp[1024];
        read_block(dind, tmp);
        uint32_t ind = rd32(tmp + (logical_block / ptrs_per_block) * 4);
        if (ind == 0) return 0;
        read_block(ind, tmp);
        return rd32(tmp + (logical_block % ptrs_per_block) * 4);
    }

    /* Triple indirect — unlikely for our use case */
    return 0;
}

/* ── Directory lookup ─────────────────────────────────── */
static int dir_lookup(uint32_t dir_ino, const char *name, uint32_t *ino_out) {
    uint8_t inode_buf[128];
    if (read_inode(dir_ino, inode_buf) != 0) return -1;

    uint32_t dir_size = inode_size_field(inode_buf);
    uint32_t name_len = my_strlen(name);
    uint32_t pos = 0;

    while (pos < dir_size) {
        uint32_t lb = pos / block_size;
        uint32_t off = pos % block_size;
        uint32_t phys = get_block_num(inode_buf, lb);
        if (phys == 0) break;

        uint8_t tmp[1024];
        read_block(phys, tmp);

        while (off < block_size && pos < dir_size) {
            uint32_t d_ino = rd32(tmp + off);
            uint16_t rec_len = rd16(tmp + off + 4);
            uint8_t  d_name_len = tmp[off + 6];

            if (rec_len == 0) break;

            if (d_ino != 0 && d_name_len == name_len) {
                /* Case-insensitive comparison for compatibility */
                int match = 1;
                for (uint32_t i = 0; i < name_len; i++) {
                    if (my_toupper(tmp[off + 8 + i]) != my_toupper((uint8_t)name[i])) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    *ino_out = d_ino;
                    return 0;
                }
            }
            off += rec_len;
            pos += rec_len;
        }
    }
    return -1;
}

/* ── Block allocation ─────────────────────────────────── */
static uint32_t alloc_block(void) {
    for (uint32_t g = 0; g < num_groups; g++) {
        if (bg_cache[g].free_blocks == 0) continue;

        uint8_t bmp[1024];
        read_block(bg_cache[g].block_bitmap, bmp);

        uint32_t blocks_in_group = blocks_per_group;
        if (g == num_groups - 1)
            blocks_in_group = total_blocks - g * blocks_per_group;

        /* Skip overhead: superblock, BGDT, bitmaps, inode table */
        uint32_t inode_table_blocks = (inodes_per_group * inode_size + block_size - 1) / block_size;
        uint32_t overhead = (bg_cache[g].inode_table - (first_data_block + g * blocks_per_group))
                            + inode_table_blocks;

        for (uint32_t b = overhead; b < blocks_in_group; b++) {
            if (!(bmp[b / 8] & (1 << (b % 8)))) {
                bmp[b / 8] |= (1 << (b % 8));
                write_block(bg_cache[g].block_bitmap, bmp);
                bg_cache[g].free_blocks--;
                return first_data_block + g * blocks_per_group + b;
            }
        }
    }
    return 0;
}

/* ── Inode allocation ─────────────────────────────────── */
static uint32_t alloc_inode(void) {
    for (uint32_t g = 0; g < num_groups; g++) {
        if (bg_cache[g].free_inodes == 0) continue;

        uint8_t bmp[1024];
        read_block(bg_cache[g].inode_bitmap, bmp);

        for (uint32_t i = 0; i < inodes_per_group; i++) {
            if (!(bmp[i / 8] & (1 << (i % 8)))) {
                bmp[i / 8] |= (1 << (i % 8));
                write_block(bg_cache[g].inode_bitmap, bmp);
                bg_cache[g].free_inodes--;
                return g * inodes_per_group + i + 1;
            }
        }
    }
    return 0;
}

/* ── Free a block ─────────────────────────────────────── */
static void free_block(uint32_t block) {
    uint32_t g = (block - first_data_block) / blocks_per_group;
    uint32_t b = (block - first_data_block) % blocks_per_group;
    if (g >= num_groups) return;

    uint8_t bmp[1024];
    read_block(bg_cache[g].block_bitmap, bmp);
    bmp[b / 8] &= ~(1 << (b % 8));
    write_block(bg_cache[g].block_bitmap, bmp);
    bg_cache[g].free_blocks++;
}

/* ── Free an inode ────────────────────────────────────── */
static void free_inode(uint32_t ino) {
    uint32_t g = (ino - 1) / inodes_per_group;
    uint32_t i = (ino - 1) % inodes_per_group;
    if (g >= num_groups) return;

    uint8_t bmp[1024];
    read_block(bg_cache[g].inode_bitmap, bmp);
    bmp[i / 8] &= ~(1 << (i % 8));
    write_block(bg_cache[g].inode_bitmap, bmp);
    bg_cache[g].free_inodes++;
}

/* ── Free all data blocks of an inode ─────────────────── */
static void free_inode_blocks(const uint8_t *inode_buf) {
    uint32_t file_blocks = inode_blocks(inode_buf) / (block_size / 512);

    for (uint32_t i = 0; i < file_blocks && i < EXT2_NDIR_BLOCKS; i++) {
        uint32_t b = inode_block(inode_buf, i);
        if (b) free_block(b);
    }

    /* Free indirect block and its children */
    uint32_t ind = inode_block(inode_buf, EXT2_IND_BLOCK);
    if (ind) {
        uint8_t tmp[1024];
        read_block(ind, tmp);
        uint32_t ptrs = block_size / 4;
        for (uint32_t i = 0; i < ptrs; i++) {
            uint32_t b = rd32(tmp + i * 4);
            if (b) free_block(b);
        }
        free_block(ind);
    }
    /* Skip double/triple indirect for now */
}

/* ── Add directory entry ──────────────────────────────── */
static int dir_add_entry(uint32_t dir_ino, const char *name, uint32_t child_ino, uint8_t ftype) {
    uint8_t dir_inode[128];
    if (read_inode(dir_ino, dir_inode) != 0) return -1;

    uint32_t dir_size = inode_size_field(dir_inode);
    uint32_t name_len = my_strlen(name);
    uint32_t needed = ((8 + name_len + 3) / 4) * 4;  /* align to 4 */
    uint32_t pos = 0;

    while (pos < dir_size) {
        uint32_t lb = pos / block_size;
        uint32_t phys = get_block_num(dir_inode, lb);
        if (phys == 0) break;

        uint8_t tmp[1024];
        read_block(phys, tmp);

        uint32_t off = 0;
        while (off < block_size) {
            uint16_t rec_len = rd16(tmp + off + 4);
            if (rec_len == 0) break;

            uint32_t d_ino = rd32(tmp + off);
            uint8_t d_name_len = tmp[off + 6];
            uint32_t actual = ((8 + d_name_len + 3) / 4) * 4;

            if (d_ino == 0 && rec_len >= needed) {
                /* Reuse deleted entry */
                wr32(tmp + off, child_ino);
                tmp[off + 6] = (uint8_t)name_len;
                tmp[off + 7] = ftype;
                my_memcpy(tmp + off + 8, name, name_len);
                write_block(phys, tmp);
                return 0;
            }

            if (rec_len - actual >= needed) {
                /* Split this entry */
                wr16(tmp + off + 4, (uint16_t)actual);
                uint32_t new_off = off + actual;
                wr32(tmp + new_off, child_ino);
                wr16(tmp + new_off + 4, (uint16_t)(rec_len - actual));
                tmp[new_off + 6] = (uint8_t)name_len;
                tmp[new_off + 7] = ftype;
                my_memcpy(tmp + new_off + 8, name, name_len);
                write_block(phys, tmp);
                return 0;
            }
            off += rec_len;
        }
        pos += block_size;
    }

    /* TODO: allocate a new directory block if full */
    return -1;
}

/* ── Remove directory entry ───────────────────────────── */
static int dir_remove_entry(uint32_t dir_ino, const char *name) {
    uint8_t dir_inode[128];
    if (read_inode(dir_ino, dir_inode) != 0) return -1;

    uint32_t dir_size = inode_size_field(dir_inode);
    uint32_t name_len = my_strlen(name);
    uint32_t pos = 0;

    while (pos < dir_size) {
        uint32_t lb = pos / block_size;
        uint32_t phys = get_block_num(dir_inode, lb);
        if (phys == 0) break;

        uint8_t tmp[1024];
        read_block(phys, tmp);

        uint32_t off = 0;
        uint32_t prev_off = 0;
        int first = 1;

        while (off < block_size) {
            uint32_t d_ino = rd32(tmp + off);
            uint16_t rec_len = rd16(tmp + off + 4);
            uint8_t d_name_len = tmp[off + 6];

            if (rec_len == 0) break;

            if (d_ino != 0 && d_name_len == name_len) {
                int match = 1;
                for (uint32_t i = 0; i < name_len; i++) {
                    if (my_toupper(tmp[off + 8 + i]) != my_toupper((uint8_t)name[i])) {
                        match = 0; break;
                    }
                }
                if (match) {
                    if (first) {
                        /* First entry: just zero the inode */
                        wr32(tmp + off, 0);
                    } else {
                        /* Merge with previous entry */
                        uint16_t prev_rec = rd16(tmp + prev_off + 4);
                        wr16(tmp + prev_off + 4, prev_rec + rec_len);
                    }
                    write_block(phys, tmp);
                    return 0;
                }
            }
            prev_off = off;
            first = 0;
            off += rec_len;
        }
        pos += block_size;
    }
    return -1;
}

/* ═══════════════════════════════════════════════════════
 *  VFS ops implementation
 * ═══════════════════════════════════════════════════════ */

/* ── Extended open_file: store inode number in offset temporarily ── */
/* We reuse open_file_t fields:
 *   start_cluster -> inode number (low 16)
 *   offset        -> inode number (full 32)
 *   file_size     -> file size
 *   dir_sector    -> unused
 *   dir_index     -> unused
 */

static int ext2_mount(const blk_io_t *b) {
    blk = b;

    /* Superblock is at byte offset 1024 (sector 2-3 for 512-byte sectors) */
    uint8_t sb[1024];
    blk->read_sector(2, sb);
    blk->read_sector(3, sb + 512);

    uint16_t magic = rd16(sb + 56);
    if (magic != EXT2_MAGIC) return -1;

    total_inodes    = rd32(sb + 0);
    total_blocks    = rd32(sb + 4);
    first_data_block = rd32(sb + 20);
    log_block_size  = rd32(sb + 24);
    block_size      = 1024 << log_block_size;
    blocks_per_group = rd32(sb + 32);
    inodes_per_group = rd32(sb + 40);
    inode_size       = rd16(sb + 88);

    if (inode_size == 0) inode_size = 128;
    if (block_size > 1024) return -1;  /* only support 1K blocks for now */

    num_groups = (total_blocks + blocks_per_group - 1) / blocks_per_group;
    if (num_groups > MAX_GROUPS) num_groups = MAX_GROUPS;

    /* Read block group descriptor table (block 2 for 1K blocks) */
    uint8_t bgdt[1024];
    read_block(first_data_block + 1, bgdt);

    for (uint32_t g = 0; g < num_groups; g++) {
        uint32_t off = g * 32;
        bg_cache[g].block_bitmap = rd32(bgdt + off + 0);
        bg_cache[g].inode_bitmap = rd32(bgdt + off + 4);
        bg_cache[g].inode_table  = rd32(bgdt + off + 8);
        bg_cache[g].free_blocks  = rd16(bgdt + off + 12);
        bg_cache[g].free_inodes  = rd16(bgdt + off + 14);
    }

    return 0;
}


/* ── Path resolution: walk /dir1/dir2/file ──────────── */
static int resolve_path(const char *path, uint32_t *parent_ino, char *basename, int basename_max) {
    uint32_t cur_ino = EXT2_ROOT_INO;
    
    /* Skip leading / */
    int i = 0;
    if (path[0] == '/') i = 1;
    
    /* Walk each component */
    while (path[i]) {
        /* Extract next component */
        char comp[64];
        int ci = 0;
        while (path[i] && path[i] != '/' && ci < 63) {
            comp[ci++] = path[i++];
        }
        comp[ci] = '\0';
        if (ci == 0) { i++; continue; } /* skip double slashes */
        
        /* Is there more path after this? */
        int more = 0;
        int j = i;
        while (path[j] == '/') j++;
        if (path[j]) more = 1;
        
        if (more) {
            /* This component must be a directory — look it up */
            uint32_t child_ino;
            if (dir_lookup(cur_ino, comp, &child_ino) != 0)
                return -1; /* directory not found */
            
            /* Verify it's a directory */
            uint8_t ib[128];
            if (read_inode(child_ino, ib) != 0) return -1;
            if ((rd16(ib + 0) & 0xF000) != 0x4000) return -1; /* not a dir */
            
            cur_ino = child_ino;
            if (path[i] == '/') i++;
        } else {
            /* Last component — this is the basename */
            int bi = 0;
            while (bi < basename_max - 1 && comp[bi]) { basename[bi] = comp[bi]; bi++; }
            basename[bi] = '\0';
            *parent_ino = cur_ino;
            return 0;
        }
    }
    
    /* Path ended with / or was just "/" — basename is empty, parent is cur_ino */
    basename[0] = '\0';
    *parent_ino = cur_ino;
    return 0;
}

static int ext2_open(const char *filename, open_file_t *fd) {
    uint32_t ino;
    uint32_t parent_ino;
    char base[64];
    if (resolve_path(filename, &parent_ino, base, sizeof(base)) != 0) return -1;
    if (base[0] == '\0') return -1; /* can't open a directory as file */
    if (dir_lookup(parent_ino, base, &ino) != 0)
        return -1;

    uint8_t inode_buf[128];
    if (read_inode(ino, inode_buf) != 0) return -1;

    fd->in_use = 1;
    fd->start_cluster = (uint16_t)(ino & 0xFFFF);
    fd->offset = ino;  /* full inode number */
    fd->file_size = inode_size_field(inode_buf);
    return 0;
}

static int ext2_read(open_file_t *fd, uint8_t *buf, uint32_t offset,
                     uint32_t len, uint32_t *bytes_read) {
    if (offset >= fd->file_size) { *bytes_read = 0; return 0; }
    if (offset + len > fd->file_size) len = fd->file_size - offset;

    uint32_t ino = fd->offset;
    uint8_t inode_buf[128];
    if (read_inode(ino, inode_buf) != 0) { *bytes_read = 0; return -1; }

    uint32_t pos = 0;
    while (pos < len) {
        uint32_t lb = (offset + pos) / block_size;
        uint32_t off_in_block = (offset + pos) % block_size;
        uint32_t phys = get_block_num(inode_buf, lb);
        if (phys == 0) break;

        uint8_t tmp[1024];
        read_block(phys, tmp);

        uint32_t chunk = block_size - off_in_block;
        if (chunk > len - pos) chunk = len - pos;
        my_memcpy(buf + pos, tmp + off_in_block, chunk);
        pos += chunk;
    }
    *bytes_read = pos;
    return 0;
}

static int ext2_close(open_file_t *fd) {
    fd->in_use = 0;
    return 0;
}

static int ext2_create(const char *filename, open_file_t *fd) {
    uint32_t parent_ino;
    char base[64];
    if (resolve_path(filename, &parent_ino, base, sizeof(base)) != 0) return -1;
    if (base[0] == '\0') return -1;
    
    /* Allocate inode */
    uint32_t ino = alloc_inode();
    if (ino == 0) return -1;

    /* Allocate first data block */
    uint32_t data_blk = alloc_block();
    if (data_blk == 0) { free_inode(ino); return -1; }

    /* Zero the data block */
    uint8_t zeros[1024];
    my_memset(zeros, 0, block_size);
    write_block(data_blk, zeros);

    /* Initialize inode */
    uint8_t inode_buf[128];
    my_memset(inode_buf, 0, 128);
    wr16(inode_buf + 0, 0x81A4);      /* mode: regular file, 0644 */
    wr16(inode_buf + 2, ext2_creator_uid);  /* i_uid */
    wr16(inode_buf + 24, ext2_creator_gid); /* i_gid */
    wr32(inode_buf + 4, 0);            /* size = 0 */
    wr16(inode_buf + 26, 1);           /* links_count */
    wr32(inode_buf + 28, block_size / 512); /* blocks in 512-byte units */
    wr32(inode_buf + 40, data_blk);    /* i_block[0] */
    write_inode(ino, inode_buf);

    /* Add directory entry */
    if (dir_add_entry(parent_ino, base, ino, EXT2_FT_REG_FILE) != 0) {
        free_block(data_blk);
        free_inode(ino);
        return -1;
    }

    fd->in_use = 1;
    fd->start_cluster = (uint16_t)(ino & 0xFFFF);
    fd->offset = ino;
    fd->file_size = 0;
    return 0;
}

static int ext2_write(open_file_t *fd, const uint8_t *data_in,
                      uint32_t len, uint32_t *bytes_written) {
    uint32_t ino = fd->offset;
    uint8_t inode_buf[128];
    if (read_inode(ino, inode_buf) != 0) { *bytes_written = 0; return -1; }

    uint32_t file_size = inode_size_field(inode_buf);
    uint32_t written = 0;

    while (written < len) {
        uint32_t lb = (file_size + written) / block_size;
        uint32_t off_in_block = (file_size + written) % block_size;

        uint32_t phys = 0;
        if (lb < EXT2_NDIR_BLOCKS) {
            phys = inode_block(inode_buf, lb);
            if (phys == 0) {
                phys = alloc_block();
                if (phys == 0) break;
                wr32(inode_buf + 40 + lb * 4, phys);
                uint8_t zeros[1024];
                my_memset(zeros, 0, block_size);
                write_block(phys, zeros);
            }
        } else {
            /* TODO: indirect block allocation */
            break;
        }

        uint8_t tmp[1024];
        read_block(phys, tmp);

        uint32_t chunk = block_size - off_in_block;
        if (chunk > len - written) chunk = len - written;
        my_memcpy(tmp + off_in_block, data_in + written, chunk);
        write_block(phys, tmp);
        written += chunk;
    }

    /* Update inode */
    file_size += written;
    wr32(inode_buf + 4, file_size);
    uint32_t total_blks = (file_size + block_size - 1) / block_size;
    wr32(inode_buf + 28, total_blks * (block_size / 512));
    write_inode(ino, inode_buf);

    fd->file_size = file_size;
    *bytes_written = written;
    return 0;
}

static int ext2_delete(const char *filename) {
    uint32_t ino;
    uint32_t parent_ino_d;
    char base_d[64];
    if (resolve_path(filename, &parent_ino_d, base_d, sizeof(base_d)) != 0) return -1;
    if (dir_lookup(parent_ino_d, base_d, &ino) != 0) return -1;

    uint8_t inode_buf[128];
    if (read_inode(ino, inode_buf) != 0) return -1;

    /* Free data blocks */
    free_inode_blocks(inode_buf);

    /* Clear inode */
    my_memset(inode_buf, 0, 128);
    write_inode(ino, inode_buf);

    /* Free inode */
    free_inode(ino);

    /* Remove directory entry */
    dir_remove_entry(parent_ino_d, base_d);

    return 0;
}

static int ext2_list(uint8_t *buf, uint32_t buf_size, uint32_t *count, uint32_t *total_bytes_out) {
    uint8_t inode_buf[128];
    if (read_inode(EXT2_ROOT_INO, inode_buf) != 0) { *count = 0; return -1; }

    uint32_t dir_size = inode_size_field(inode_buf);
    uint32_t pos = 0;
    uint32_t out_pos = 0;
    uint32_t file_count = 0;

    while (pos < dir_size) {
        uint32_t lb = pos / block_size;
        uint32_t phys = get_block_num(inode_buf, lb);
        if (phys == 0) break;

        uint8_t tmp[1024];
        read_block(phys, tmp);

        uint32_t off = 0;
        while (off < block_size && pos < dir_size) {
            uint32_t d_ino = rd32(tmp + off);
            uint16_t rec_len = rd16(tmp + off + 4);
            uint8_t  d_name_len = tmp[off + 6];
            uint8_t  d_ftype = tmp[off + 7];

            if (rec_len == 0) goto done;

            /* Skip . and .. and deleted entries */
            if (d_ino != 0 && d_name_len > 0 && d_ftype != EXT2_FT_DIR) {
                /* size check moved below */

                /* Variable-length entry: [2B total_len][1B name_len][1B type][4B size][name][NUL][pad] */
                uint8_t child_inode[128];
                uint32_t fsize = 0;
                if (read_inode(d_ino, child_inode) == 0) {
                    fsize = inode_size_field(child_inode);
                }
                int nlen = d_name_len < 255 ? d_name_len : 255;
                uint16_t elen = (uint16_t)((8 + nlen + 1 + 3) & ~3);  /* align to 4 */
                if (out_pos + elen > buf_size) goto done;

                buf[out_pos + 0] = elen & 0xFF;
                buf[out_pos + 1] = (elen >> 8) & 0xFF;
                buf[out_pos + 2] = (uint8_t)nlen;
                buf[out_pos + 3] = d_ftype;
                buf[out_pos + 4] = fsize & 0xFF;
                buf[out_pos + 5] = (fsize >> 8) & 0xFF;
                buf[out_pos + 6] = (fsize >> 16) & 0xFF;
                buf[out_pos + 7] = (fsize >> 24) & 0xFF;
                my_memcpy(buf + out_pos + 8, tmp + off + 8, nlen);
                buf[out_pos + 8 + nlen] = 0;

                out_pos += elen;
                file_count++;
            }
            off += rec_len;
            pos += rec_len;
        }
    }
done:
    *count = file_count;
    if (total_bytes_out) *total_bytes_out = out_pos;
    return 0;
}

static int ext2_sync(void) { return 0; }

static int ext2_stat(const char *filename, uint32_t *size_out) {
    uint32_t ino;
    uint32_t parent_ino_s;
    char base_s[64];
    if (resolve_path(filename, &parent_ino_s, base_s, sizeof(base_s)) != 0) return -1;
    if (dir_lookup(parent_ino_s, base_s, &ino) != 0) return -1;

    uint8_t inode_buf[128];
    if (read_inode(ino, inode_buf) != 0) return -1;

    *size_out = inode_size_field(inode_buf);
    return 0;
}

/* ── Export the ops table ─────────────────────────────── */

/* ── mkdir: create a subdirectory ────────────────────── */
static int ext2_mkdir(const char *dirname) {
    uint32_t mkdir_parent;
    char mkdir_base[64];
    if (resolve_path(dirname, &mkdir_parent, mkdir_base, sizeof(mkdir_base)) != 0)
        return -1;
    if (mkdir_base[0] == '\0') return -1;

    /* Allocate inode for new directory */
    uint32_t ino = alloc_inode();
    if (ino == 0) return -1;

    /* Allocate data block for directory entries (. and ..) */
    uint32_t data_blk = alloc_block();
    if (data_blk == 0) { free_inode(ino); return -1; }

    /* Build directory block with . and .. entries */
    uint8_t dir_data[1024];
    my_memset(dir_data, 0, block_size);

    /* "." entry — points to self */
    uint32_t off = 0;
    wr32(dir_data + off + 0, ino);           /* inode */
    wr16(dir_data + off + 4, 12);            /* rec_len */
    dir_data[off + 6] = 1;                   /* name_len */
    dir_data[off + 7] = EXT2_FT_DIR;        /* file_type */
    dir_data[off + 8] = '.';

    /* ".." entry — points to parent (root for now) */
    off = 12;
    wr32(dir_data + off + 0, mkdir_parent); /* parent inode */
    wr16(dir_data + off + 4, block_size - 12); /* rec_len fills rest */
    dir_data[off + 6] = 2;                   /* name_len */
    dir_data[off + 7] = EXT2_FT_DIR;        /* file_type */
    dir_data[off + 8] = '.';
    dir_data[off + 9] = '.';

    write_block(data_blk, dir_data);

    /* Initialize inode as directory */
    uint8_t inode_buf[128];
    my_memset(inode_buf, 0, 128);
    wr16(inode_buf + 0, 0x41ED);        /* mode: directory, 0755 */
    wr16(inode_buf + 2, ext2_creator_uid);  /* i_uid */
    wr16(inode_buf + 24, ext2_creator_gid); /* i_gid */
    wr32(inode_buf + 4, block_size);    /* size = one block */
    wr16(inode_buf + 26, 2);            /* links_count (. and parent) */
    wr32(inode_buf + 28, block_size / 512); /* blocks in 512-byte units */
    wr32(inode_buf + 40, data_blk);     /* i_block[0] */
    write_inode(ino, inode_buf);

    /* Add entry in parent (root) directory */
    if (dir_add_entry(mkdir_parent, mkdir_base, ino, EXT2_FT_DIR) != 0) {
        free_block(data_blk);
        free_inode(ino);
        return -1;
    }

    /* Increment parent link count (for ..) */
    uint8_t parent_buf[128];
    if (read_inode(mkdir_parent, parent_buf) == 0) {
        uint16_t links = rd16(parent_buf + 26);
        wr16(parent_buf + 26, links + 1);
        write_inode(mkdir_parent, parent_buf);
    }

    return 0;
}

/* ── rmdir: remove an empty subdirectory ────────────── */
static int ext2_rmdir(const char *dirname) {
    uint32_t parent_ino;
    char base[64];
    if (resolve_path(dirname, &parent_ino, base, sizeof(base)) != 0)
        return -1;

    uint32_t ino;
    if (dir_lookup(parent_ino, base, &ino) != 0)
        return -1;

    /* Verify it is a directory */
    uint8_t inode_buf[128];
    if (read_inode(ino, inode_buf) != 0) return -1;
    uint16_t mode = rd16(inode_buf + 0);
    if ((mode & 0xF000) != 0x4000) return -1; /* not a directory */

    /* Check directory is empty (only . and ..) */
    uint32_t blk = rd32(inode_buf + 40);
    uint8_t dir_data[1024];
    read_block(blk, dir_data);

    uint32_t off = 0;
    int entry_count = 0;
    while (off < block_size) {
        uint32_t d_ino = rd32(dir_data + off);
        uint16_t rec_len = rd16(dir_data + off + 4);
        if (rec_len == 0) break;
        if (d_ino != 0) {
            uint8_t name_len = dir_data[off + 6];
            /* Skip . and .. */
            if (name_len == 1 && dir_data[off + 8] == '.') { off += rec_len; continue; }
            if (name_len == 2 && dir_data[off + 8] == '.' && dir_data[off + 9] == '.') { off += rec_len; continue; }
            entry_count++;
        }
        off += rec_len;
    }
    if (entry_count > 0) return -1; /* not empty */

    /* Free data block and inode */
    free_block(blk);
    free_inode(ino);

    /* Remove directory entry from parent */
    dir_remove_entry(parent_ino, base);

    /* Decrement parent link count */
    uint8_t parent_buf[128];
    if (read_inode(parent_ino, parent_buf) == 0) {
        uint16_t links = rd16(parent_buf + 26);
        if (links > 1) wr16(parent_buf + 26, links - 1);
        write_inode(parent_ino, parent_buf);
    }

    return 0;
}

/* ── rename: rename a file (flat, root-dir only) ──── */
static int ext2_rename(const char *oldname, const char *newname) {
    uint32_t ino;
    if (dir_lookup(EXT2_ROOT_INO, oldname, &ino) != 0)
        return -1;

    /* Read inode to get file type */
    uint8_t inode_buf[128];
    if (read_inode(ino, inode_buf) != 0) return -1;
    uint16_t mode = rd16(inode_buf + 0);
    uint8_t ftype = ((mode & 0xF000) == 0x4000) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;

    /* Remove old entry, add new one */
    dir_remove_entry(EXT2_ROOT_INO, oldname);
    if (dir_add_entry(EXT2_ROOT_INO, newname, ino, ftype) != 0)
        return -1;

    return 0;
}


/* Extended stat: returns size, uid, gid, mode */
static int ext2_stat_ex(const char *filename, uint32_t *size_out,
                        uint16_t *uid_out, uint16_t *gid_out, uint16_t *mode_out) {
    uint32_t parent_ino, ino;
    char base[64];
    if (resolve_path(filename, &parent_ino, base, sizeof(base)) != 0) return -1;
    if (dir_lookup(parent_ino, base, &ino) != 0) return -1;
    uint8_t inode_buf[128];
    if (read_inode(ino, inode_buf) != 0) return -1;
    *size_out = inode_size_field(inode_buf);
    *uid_out  = inode_uid(inode_buf);
    *gid_out  = inode_gid(inode_buf);
    *mode_out = inode_mode(inode_buf);
    return 0;
}

/* chmod: change file mode */
static int ext2_chmod(const char *filename, uint16_t new_mode) {
    uint32_t parent_ino, ino;
    char base[64];
    if (resolve_path(filename, &parent_ino, base, sizeof(base)) != 0) return -1;
    if (dir_lookup(parent_ino, base, &ino) != 0) return -1;
    uint8_t inode_buf[128];
    if (read_inode(ino, inode_buf) != 0) return -1;
    uint16_t old_mode = rd16(inode_buf + 0);
    uint16_t updated = (old_mode & 0xF000) | (new_mode & 0x0FFF);
    wr16(inode_buf + 0, updated);
    write_inode(ino, inode_buf);
    return 0;
}

/* chown: change file owner */
static int ext2_chown(const char *filename, uint16_t new_uid, uint16_t new_gid) {
    uint32_t parent_ino, ino;
    char base[64];
    if (resolve_path(filename, &parent_ino, base, sizeof(base)) != 0) return -1;
    if (dir_lookup(parent_ino, base, &ino) != 0) return -1;
    uint8_t inode_buf[128];
    if (read_inode(ino, inode_buf) != 0) return -1;
    wr16(inode_buf + 2, new_uid);
    wr16(inode_buf + 24, new_gid);
    write_inode(ino, inode_buf);
    return 0;
}

/* Set creator uid/gid for next create/mkdir */
static void ext2_set_creator(uint16_t uid, uint16_t gid) {
    ext2_creator_uid = uid;
    ext2_creator_gid = gid;
}

const aios_fs_ops_t ext2_ops = {
    .name   = "ext2",
    .mount  = ext2_mount,
    .open   = ext2_open,
    .read   = ext2_read,
    .close  = ext2_close,
    .create = ext2_create,
    .write  = ext2_write,
    .delete = ext2_delete,
    .list   = ext2_list,
    .sync   = ext2_sync,
    .stat   = ext2_stat,
    .mkdir  = ext2_mkdir,
    .rmdir  = ext2_rmdir,
    .rename = ext2_rename,
    .stat_ex = ext2_stat_ex,
    .chmod = ext2_chmod,
    .chown = ext2_chown,
    .set_creator = ext2_set_creator,
};

/* ── Probe: check for ext2 magic ──────────────────────── */
const aios_fs_ops_t *ext2_probe(const blk_io_t *b) {
    /* Superblock at byte offset 1024 = sectors 2-3 */
    uint8_t sb[1024];
    b->read_sector(2, sb);
    b->read_sector(3, sb + 512);

    uint16_t magic = rd16(sb + 56);
    if (magic == EXT2_MAGIC) return &ext2_ops;
    return 0;
}
