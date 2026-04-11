#include "aios/ext2.h"
#include "aios/vfs.h"
#include <stdio.h>

/* Read uint16/uint32 from raw buffer (little-endian) */
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

/* ---- Block cache: write-through, round-robin ----
 * 2048 entries (~2MB) needed: tcc output write involves ~1400
 * cache operations (159 data blocks + inode/bitmap/indirect
 * overhead). Block 0 (ELF header) must survive until exec reads. */
#define EXT2_CACHE_SIZE 2048

static struct {
    uint32_t block;
    uint8_t  data[1024];
    int      valid;
    int      dev_id;
} blk_cache[EXT2_CACHE_SIZE];

static int cache_next = 0;

static void cache_init(void) {
    for (int i = 0; i < EXT2_CACHE_SIZE; i++)
        blk_cache[i].valid = 0;
    cache_next = 0;
}

/* Return 0 on hit, -1 on miss */
static int cache_lookup(int dev_id, uint32_t block, void *buf) {
    for (int i = 0; i < EXT2_CACHE_SIZE; i++) {
        if (blk_cache[i].valid && blk_cache[i].dev_id == dev_id && blk_cache[i].block == block) {
            uint8_t *dst = (uint8_t *)buf;
            for (int j = 0; j < 1024; j++) dst[j] = blk_cache[i].data[j];
            return 0;
        }
    }
    return -1;
}

/* Store block in cache (update-in-place or evict round-robin) */
static void cache_store(int dev_id, uint32_t block, const void *buf) {
    for (int i = 0; i < EXT2_CACHE_SIZE; i++) {
        if (blk_cache[i].valid && blk_cache[i].dev_id == dev_id && blk_cache[i].block == block) {
            const uint8_t *src = (const uint8_t *)buf;
            for (int j = 0; j < 1024; j++) blk_cache[i].data[j] = src[j];
            return;
        }
    }
    int slot = cache_next;
    cache_next = (cache_next + 1) % EXT2_CACHE_SIZE;
    blk_cache[slot].block = block;
    blk_cache[slot].valid = 1;
    blk_cache[slot].dev_id = dev_id;
    const uint8_t *src = (const uint8_t *)buf;
    for (int j = 0; j < 1024; j++) blk_cache[slot].data[j] = src[j];
}

/* Invalidate a single block from the cache */
static void cache_invalidate(int dev_id, uint32_t block) {
    for (int i = 0; i < EXT2_CACHE_SIZE; i++) {
        if (blk_cache[i].valid && blk_cache[i].dev_id == dev_id && blk_cache[i].block == block) {
            blk_cache[i].valid = 0;
            return;
        }
    }
}


static int read_block(ext2_ctx_t *ctx, uint32_t block, void *buf) {
    if (cache_lookup(ctx->dev_id, block, buf) == 0) return 0;
    int sectors = ctx->block_size / 512;
    uint8_t *p = (uint8_t *)buf;
    for (int i = 0; i < sectors; i++) {
        if (ctx->read_sector((uint64_t)block * sectors + i, p + i * 512) != 0)
            return -1;
    }
    cache_store(ctx->dev_id, block, buf);
    return 0;
}

/* Forward declarations for static functions used by pwrite */
static int write_block(ext2_ctx_t *ctx, uint32_t block, const void *buf);
static int write_inode(ext2_ctx_t *ctx, uint32_t ino, struct ext2_inode *inode);

int ext2_init(ext2_ctx_t *ctx, blk_read_fn read, int dev_id) {
    ctx->read_sector = read;
    ctx->dev_id = dev_id;
    if (dev_id == 0) cache_init();

    /* Superblock at offset 1024 = sectors 2-3 */
    uint8_t sb[1024];
    if (read(2, sb) != 0) return -1;
    if (read(3, sb + 512) != 0) return -1;

    uint16_t magic = rd16(sb + 0x38);
    if (magic != EXT2_MAGIC) return -2;

    ctx->block_size = 1024 << rd32(sb + 24);
    ctx->inodes_per_group = rd32(sb + 40);
    ctx->blocks_per_group = rd32(sb + 32);
    ctx->first_data_block = rd32(sb + 20);

    /* Read inode size: rev 0 = always 128, rev 1+ = s_inode_size at offset 88 */
    uint32_t rev = rd32(sb + 76);
    if (rev >= 1) {
        ctx->inode_size = rd16(sb + 88);
        if (ctx->inode_size == 0) ctx->inode_size = 128;
    } else {
        ctx->inode_size = 128;
    }

    /* Group descriptor (block after superblock) */
    uint8_t gd[1024];
    if (read_block(ctx, ctx->first_data_block + 1, gd) != 0) return -3;
    ctx->inode_table_block = rd32(gd + 8);

    return 0;
}

int ext2_read_inode(ext2_ctx_t *ctx, uint32_t ino, struct ext2_inode *out) {
    uint32_t idx = ino - 1;
    uint32_t isz = ctx->inode_size;
    uint32_t inodes_per_block = ctx->block_size / isz;
    uint32_t block = ctx->inode_table_block + idx / inodes_per_block;
    uint32_t offset = (idx % inodes_per_block) * isz;

    uint8_t blk[1024];
    if (read_block(ctx, block, blk) != 0) return -1;

    uint8_t *src = blk + offset;
    uint8_t *dst = (uint8_t *)out;
    for (int i = 0; i < (int)sizeof(struct ext2_inode); i++) dst[i] = src[i];
    return 0;
}

int ext2_list_dir(ext2_ctx_t *ctx, uint32_t dir_ino, char *buf, int bufsize) {
    struct ext2_inode inode;
    if (ext2_read_inode(ctx, dir_ino, &inode) != 0) return -1;
    if (!(inode.i_mode & EXT2_S_IFDIR)) return -2;

    int written = 0;

    for (int bi = 0; bi < 12 && inode.i_block[bi]; bi++) {
        uint8_t blk[1024];
        if (read_block(ctx, inode.i_block[bi], blk) != 0) return -3;

        int off = 0;
        while (off < (int)ctx->block_size) {
            uint32_t d_ino = rd32(blk + off);
            uint16_t rec_len = rd16(blk + off + 4);
            uint8_t name_len = blk[off + 6];
            uint8_t ftype = blk[off + 7];
            if (rec_len == 0) break;

            if (d_ino && name_len > 0) {
                char tc = (ftype == EXT2_FT_DIR) ? 'd' : '-';
                if (written + name_len + 4 < bufsize) {
                    buf[written++] = tc;
                    buf[written++] = ' ';
                    for (int i = 0; i < name_len; i++)
                        buf[written++] = (char)blk[off + 8 + i];
                    buf[written++] = '\n';
                }
            }
            off += rec_len;
        }
    }
    if (written < bufsize) buf[written] = '\0';
    return written;
}

/* Get the Nth data block of an inode (handles direct + single indirect) */
static int ext2_get_block_num(ext2_ctx_t *ctx, struct ext2_inode *inode, int index) {
    int ptrs_per_block = (int)(ctx->block_size / 4);

    /* Direct blocks: 0-11 */
    if (index < 12) {
        return (int)inode->i_block[index];
    }

    /* Single indirect: 12 .. 12+ptrs_per_block-1 */
    index -= 12;
    if (index < ptrs_per_block) {
        uint32_t ind_block = inode->i_block[12];
        if (!ind_block) return 0;
        uint8_t ind_buf[1024];
        if (read_block(ctx, ind_block, ind_buf) != 0) return 0;
        return (int)rd32(ind_buf + index * 4);
    }

    /* Double indirect: ptrs_per_block .. ptrs_per_block + ptrs^2 - 1 */
    index -= ptrs_per_block;
    if (index < ptrs_per_block * ptrs_per_block) {
        uint32_t dind_block = inode->i_block[13];
        if (!dind_block) return 0;
        uint8_t dind_buf[1024];
        if (read_block(ctx, dind_block, dind_buf) != 0) return 0;
        uint32_t ind_block = rd32(dind_buf + (index / ptrs_per_block) * 4);
        if (!ind_block) return 0;
        uint8_t ind_buf[1024];
        if (read_block(ctx, ind_block, ind_buf) != 0) return 0;
        return (int)rd32(ind_buf + (index % ptrs_per_block) * 4);
    }

    return 0; /* triple indirect not supported */
}

int ext2_read_file(ext2_ctx_t *ctx, uint32_t ino, char *buf, int bufsize) {
    struct ext2_inode inode;
    if (ext2_read_inode(ctx, ino, &inode) != 0) return -1;

    int size = (int)inode.i_size;
    if (size > bufsize - 1) size = bufsize - 1;

    int copied = 0;
    int block_idx = 0;
    while (copied < size) {
        int blk_num = ext2_get_block_num(ctx, &inode, block_idx);
        if (blk_num == 0) break;
        uint8_t blk[1024];
        if (read_block(ctx, blk_num, blk) != 0) return -2;
        int chunk = size - copied;
        if (chunk > (int)ctx->block_size) chunk = (int)ctx->block_size;
        for (int i = 0; i < chunk; i++) buf[copied++] = blk[i];
        block_idx++;
    }
    buf[copied] = '\0';
    return copied;
}

/* Set block number for an inode at given index, allocating indirect blocks as needed */
static int ext2_set_block_num(ext2_ctx_t *ctx, struct ext2_inode *inode,
                              int index, uint32_t blk_num) {
    int ptrs_per_block = (int)(ctx->block_size / 4);

    /* Direct blocks: 0-11 */
    if (index < 12) {
        inode->i_block[index] = blk_num;
        return 0;
    }

    /* Single indirect: 12 .. 12+ptrs_per_block-1 */
    index -= 12;
    if (index < ptrs_per_block) {
        if (!inode->i_block[12]) {
            int ib = ext2_alloc_block(ctx);
            if (ib < 0) return -1;
            uint8_t zero[1024];
            for (int i = 0; i < (int)ctx->block_size; i++) zero[i] = 0;
            write_block(ctx, ib, zero);
            inode->i_block[12] = (uint32_t)ib;
        }
        uint8_t ind_buf[1024];
        if (read_block(ctx, inode->i_block[12], ind_buf) != 0) return -2;
        ind_buf[index * 4]     = blk_num & 0xFF;
        ind_buf[index * 4 + 1] = (blk_num >> 8) & 0xFF;
        ind_buf[index * 4 + 2] = (blk_num >> 16) & 0xFF;
        ind_buf[index * 4 + 3] = (blk_num >> 24) & 0xFF;
        write_block(ctx, inode->i_block[12], ind_buf);
        return 0;
    }

    return -3; /* double indirect write not yet supported */
}

/* Positioned write: write len bytes at offset, allocating blocks as needed */
int ext2_pwrite_file(ext2_ctx_t *ctx, uint32_t ino, int offset,
                     const uint8_t *data, int len) {
    struct ext2_inode inode;
    if (ext2_read_inode(ctx, ino, &inode) != 0) return -1;

    int block_size = (int)ctx->block_size;
    int written = 0;

    while (written < len) {
        int cur_off = offset + written;
        int block_idx = cur_off / block_size;
        int block_off = cur_off % block_size;

        /* Get or allocate block */
        int blk_num = ext2_get_block_num(ctx, &inode, block_idx);
        if (blk_num == 0) {
            blk_num = ext2_alloc_block(ctx);
            if (blk_num < 0) return -2;
            /* Zero the new block */
            uint8_t zero[1024];
            for (int i = 0; i < block_size; i++) zero[i] = 0;
            write_block(ctx, blk_num, zero);
            if (ext2_set_block_num(ctx, &inode, block_idx, (uint32_t)blk_num) != 0)
                return -3;
        }

        /* Read block, patch in our data, write back */
        uint8_t blk[1024];
        if (read_block(ctx, blk_num, blk) != 0) return -4;
        int chunk = block_size - block_off;
        if (chunk > len - written) chunk = len - written;
        for (int i = 0; i < chunk; i++)
            blk[block_off + i] = data[written + i];
        write_block(ctx, blk_num, blk);

        written += chunk;
    }

    /* Extend file size if needed */
    int end_pos = offset + len;
    if (end_pos > (int)inode.i_size)
        inode.i_size = (uint32_t)end_pos;

    /* Update block count */
    int blocks_used = ((int)inode.i_size + block_size - 1) / block_size;
    inode.i_blocks = (uint32_t)(blocks_used * (block_size / 512));

    write_inode(ctx, ino, &inode);
    return len;
}

/* Positioned read: read up to bufsize bytes starting at offset */
int ext2_pread_file(ext2_ctx_t *ctx, uint32_t ino, int offset, char *buf, int bufsize) {
    struct ext2_inode inode;
    if (ext2_read_inode(ctx, ino, &inode) != 0) return -1;

    int size = (int)inode.i_size;
    if (offset >= size) return 0;
    int avail = size - offset;
    if (avail > bufsize) avail = bufsize;

    int copied = 0;
    int block_idx = offset / (int)ctx->block_size;
    int block_off = offset % (int)ctx->block_size;

    while (copied < avail) {
        int blk_num = ext2_get_block_num(ctx, &inode, block_idx);
        if (blk_num == 0) break;
        uint8_t blk[1024];
        if (read_block(ctx, blk_num, blk) != 0) return -2;
        int start = block_off;
        int chunk = (int)ctx->block_size - start;
        if (chunk > avail - copied) chunk = avail - copied;
        for (int i = 0; i < chunk; i++) buf[copied++] = blk[start + i];
        block_idx++;
        block_off = 0;
    }
    return copied;
}

int ext2_lookup(ext2_ctx_t *ctx, uint32_t dir_ino, const char *name, uint32_t *out_ino) {
    struct ext2_inode inode;
    if (ext2_read_inode(ctx, dir_ino, &inode) != 0) return -1;

    int name_len = 0;
    while (name[name_len]) name_len++;

    for (int bi = 0; bi < 12 && inode.i_block[bi]; bi++) {
        uint8_t blk[1024];
        if (read_block(ctx, inode.i_block[bi], blk) != 0) return -2;

        int off = 0;
        while (off < (int)ctx->block_size) {
            uint32_t d_ino = rd32(blk + off);
            uint16_t rec_len = rd16(blk + off + 4);
            uint8_t d_name_len = blk[off + 6];
            if (rec_len == 0) break;

            if (d_ino && d_name_len == name_len) {
                int match = 1;
                for (int i = 0; i < name_len; i++) {
                    if (blk[off + 8 + i] != (uint8_t)name[i]) { match = 0; break; }
                }
                if (match) { *out_ino = d_ino; return 0; }
            }
            off += rec_len;
        }
    }
    return -3;
}

int ext2_resolve_path(ext2_ctx_t *ctx, const char *path, uint32_t *out_ino) {
    uint32_t ino = EXT2_ROOT_INO;
    if (path[0] == '/') path++;
    if (path[0] == '\0') { *out_ino = ino; return 0; }

    char comp[64];
    while (*path) {
        int len = 0;
        while (path[len] && path[len] != '/' && len < 63) {
            comp[len] = path[len]; len++;
        }
        comp[len] = '\0';
        path += len;
        if (*path == '/') path++;
        if (ext2_lookup(ctx, ino, comp, &ino) != 0) return -1;
    }
    *out_ino = ino;
    return 0;
}

/* ── VFS adapter ── */
static int ext2_vfs_list(void *ctx, uint32_t dir_ino, char *buf, int bufsize) {
    return ext2_list_dir((ext2_ctx_t *)ctx, dir_ino, buf, bufsize);
}

static int ext2_vfs_read(void *ctx, const char *path, char *buf, int bufsize) {
    ext2_ctx_t *e = (ext2_ctx_t *)ctx;
    uint32_t ino;
    if (ext2_resolve_path(e, path, &ino) != 0) return -1;
    return ext2_read_file(e, ino, buf, bufsize);
}

static int ext2_vfs_pwrite(void *ctx, const char *path, int offset,
                          const void *data, int len) {
    ext2_ctx_t *e = (ext2_ctx_t *)ctx;
    uint32_t ino;
    if (ext2_resolve_path(e, path, &ino) != 0) return -1;
    return ext2_pwrite_file(e, ino, offset, (const uint8_t *)data, len);
}

static int ext2_vfs_pread(void *ctx, const char *path, int offset, char *buf, int bufsize) {
    ext2_ctx_t *e = (ext2_ctx_t *)ctx;
    uint32_t ino;
    if (ext2_resolve_path(e, path, &ino) != 0) return -1;
    return ext2_pread_file(e, ino, offset, buf, bufsize);
}

static int ext2_vfs_stat(void *ctx, const char *path, uint32_t *mode, uint32_t *size) {
    ext2_ctx_t *e = (ext2_ctx_t *)ctx;
    uint32_t ino;
    if (path[0] == '/' && path[1] == '\0') ino = 2;
    else if (ext2_resolve_path(e, path, &ino) != 0) return -1;
    struct ext2_inode inode;
    if (ext2_read_inode(e, ino, &inode) != 0) return -1;
    *mode = inode.i_mode;
    *size = inode.i_size;
    return 0;
}

static int ext2_vfs_resolve(void *ctx, const char *path, uint32_t *ino) {
    return ext2_resolve_path((ext2_ctx_t *)ctx, path, ino);
}

static int ext2_vfs_mkdir(void *ctx, const char *path) {
    ext2_ctx_t *e = (ext2_ctx_t *)ctx;
    /* Split path into parent + name */
    const char *p = path;
    if (*p == '/') p++;
    /* Find last / */
    const char *last_slash = 0;
    for (const char *s = p; *s; s++) if (*s == '/') last_slash = s;

    uint32_t parent_ino = 2; /* root */
    char name[64];
    if (last_slash) {
        /* Resolve parent path */
        char parent[256];
        int pi = 0;
        while (p + pi < last_slash && pi < 255) { parent[pi] = p[pi]; pi++; }
        parent[pi] = '\0';
        if (ext2_resolve_path(e, parent, &parent_ino) != 0) return -1;
        const char *n = last_slash + 1;
        int ni = 0;
        while (n[ni] && ni < 63) { name[ni] = n[ni]; ni++; }
        name[ni] = '\0';
    } else {
        int ni = 0;
        while (p[ni] && ni < 63) { name[ni] = p[ni]; ni++; }
        name[ni] = '\0';
    }
    return ext2_mkdir(e, parent_ino, name);
}

static int ext2_vfs_create(void *ctx, const char *path, const void *data, int len) {
    ext2_ctx_t *e = (ext2_ctx_t *)ctx;
    const char *p = path;
    if (*p == '/') p++;
    const char *last_slash = 0;
    for (const char *s = p; *s; s++) if (*s == '/') last_slash = s;

    uint32_t parent_ino = 2;
    char name[64];
    if (last_slash) {
        char parent[256];
        int pi = 0;
        while (p + pi < last_slash && pi < 255) { parent[pi] = p[pi]; pi++; }
        parent[pi] = '\0';
        if (ext2_resolve_path(e, parent, &parent_ino) != 0) return -1;
        const char *n = last_slash + 1;
        int ni = 0;
        while (n[ni] && ni < 63) { name[ni] = n[ni]; ni++; }
        name[ni] = '\0';
    } else {
        int ni = 0;
        while (p[ni] && ni < 63) { name[ni] = p[ni]; ni++; }
        name[ni] = '\0';
    }
    /* unlink existing before create -- ensures write-then-read consistency
     * and prevents duplicate directory entries on overwrite */
    uint32_t existing_ino;
    if (ext2_lookup(e, parent_ino, name, &existing_ino) == 0) {
        ext2_unlink(e, parent_ino, name);
    }
    return ext2_create_file(e, parent_ino, name, data, len);
}

static int ext2_vfs_unlink(void *ctx, const char *path) {
    ext2_ctx_t *e = (ext2_ctx_t *)ctx;
    const char *p = path;
    if (*p == '/') p++;
    const char *last_slash = 0;
    for (const char *s = p; *s; s++) if (*s == '/') last_slash = s;

    uint32_t parent_ino = 2;
    char name[64];
    if (last_slash) {
        char parent[256];
        int pi = 0;
        while (p + pi < last_slash && pi < 255) { parent[pi] = p[pi]; pi++; }
        parent[pi] = '\0';
        if (ext2_resolve_path(e, parent, &parent_ino) != 0) return -1;
        const char *n = last_slash + 1;
        int ni = 0;
        while (n[ni] && ni < 63) { name[ni] = n[ni]; ni++; }
        name[ni] = '\0';
    } else {
        int ni = 0;
        while (p[ni] && ni < 63) { name[ni] = p[ni]; ni++; }
        name[ni] = '\0';
    }
    return ext2_unlink(e, parent_ino, name);
}

fs_ops_t ext2_fs_ops = {
    .fs_list = ext2_vfs_list,
    .fs_read = ext2_vfs_read,
    .fs_pread = ext2_vfs_pread,
    .fs_pwrite = ext2_vfs_pwrite,
    .fs_stat = ext2_vfs_stat,
    .fs_resolve = ext2_vfs_resolve,
    .fs_mkdir = ext2_vfs_mkdir,
    .fs_create = ext2_vfs_create,
    .fs_unlink = ext2_vfs_unlink,
};


/* ── Write support ── */

int ext2_init_write(ext2_ctx_t *ctx, blk_write_fn write) {
    ctx->write_sector = write;
    return 0;
}

static int write_block(ext2_ctx_t *ctx, uint32_t block, const void *buf) {
    if (!ctx->write_sector) return -1;
    int sectors = ctx->block_size / 512;
    const uint8_t *p = (const uint8_t *)buf;
    for (int i = 0; i < sectors; i++) {
        if (ctx->write_sector((uint64_t)block * sectors + i, p + i * 512) != 0)
            return -1;
    }
    cache_store(ctx->dev_id, block, buf);
    return 0;
}

int ext2_write_block(ext2_ctx_t *ctx, uint32_t block, const void *buf) {
    return write_block(ctx, block, buf);
}

/* Free a previously allocated block -- clear bit in block bitmap */
static int ext2_free_block(ext2_ctx_t *ctx, uint32_t block_num) {
    uint8_t bgdt_buf[1024];
    if (read_block(ctx, ctx->first_data_block + 1, bgdt_buf) != 0) return -1;

    /* Determine which group this block belongs to */
    uint32_t rel = block_num - ctx->first_data_block;
    uint32_t group = rel / ctx->blocks_per_group;
    uint32_t idx_in_group = rel % ctx->blocks_per_group;

    if (group >= 32) return -2;

    uint32_t bb_block = rd32(bgdt_buf + group * 32 + 0); /* bg_block_bitmap */
    uint8_t bmp[1024];
    if (read_block(ctx, bb_block, bmp) != 0) return -3;

    int byte_off = idx_in_group / 8;
    int bit_off  = idx_in_group % 8;

    if (!(bmp[byte_off] & (1 << bit_off)))
        return 0;  /* already free -- idempotent */

    bmp[byte_off] &= ~(1 << bit_off);
    write_block(ctx, bb_block, bmp);

    /* Update BGDT free block count */
    {
        uint16_t fc = rd16(bgdt_buf + group * 32 + 12);
        fc++;
        bgdt_buf[group * 32 + 12] = fc & 0xFF;
        bgdt_buf[group * 32 + 13] = (fc >> 8) & 0xFF;
        write_block(ctx, ctx->first_data_block + 1, bgdt_buf);
    }

    cache_invalidate(ctx->dev_id, block_num);
    return 0;
}

/* Free a previously allocated inode -- clear bit in inode bitmap */
static int ext2_free_inode(ext2_ctx_t *ctx, uint32_t ino) {
    if (ino < 11) return -1;  /* never free reserved inodes (1-10) */

    uint8_t bgdt_buf[1024];
    if (read_block(ctx, ctx->first_data_block + 1, bgdt_buf) != 0) return -2;

    /* All inodes allocated from group 0 in this implementation */
    uint32_t ib_block = rd32(bgdt_buf + 4); /* group 0 inode bitmap */
    uint8_t bmp[1024];
    if (read_block(ctx, ib_block, bmp) != 0) return -3;

    uint32_t idx = ino - 1;
    int byte_off = idx / 8;
    int bit_off  = idx % 8;

    if (!(bmp[byte_off] & (1 << bit_off)))
        return 0;  /* already free -- idempotent */

    bmp[byte_off] &= ~(1 << bit_off);
    write_block(ctx, ib_block, bmp);

    /* Update BGDT free inode count */
    {
        uint16_t fc = rd16(bgdt_buf + 14);
        fc++;
        bgdt_buf[14] = fc & 0xFF;
        bgdt_buf[15] = (fc >> 8) & 0xFF;
        write_block(ctx, ctx->first_data_block + 1, bgdt_buf);
    }

    return 0;
}

/* Allocate a free block — scan all block groups via BGDT */
int ext2_alloc_block(ext2_ctx_t *ctx) {
    uint8_t bgdt_buf[1024];
    /* BGDT is at block 2 (for 1K block size) */
    if (read_block(ctx, ctx->first_data_block + 1, bgdt_buf) != 0) return -1;

    /* Calculate number of groups */
    uint32_t total_blocks;
    uint8_t sb_buf[1024];
    if (read_block(ctx, ctx->first_data_block, sb_buf) != 0) return -1;
    total_blocks = rd32(sb_buf + 4);
    int num_groups = (total_blocks + ctx->blocks_per_group - 1) / ctx->blocks_per_group;

    for (int g = 0; g < num_groups && g < 32; g++) {
        uint32_t bb_block = rd32(bgdt_buf + g * 32 + 0); /* bg_block_bitmap */
        uint32_t group_start = g * ctx->blocks_per_group + ctx->first_data_block;

        uint8_t bmp[1024];
        if (read_block(ctx, bb_block, bmp) != 0) continue;

        for (int byte = 0; byte < (int)ctx->block_size; byte++) {
            if (bmp[byte] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (!(bmp[byte] & (1 << bit))) {
                    bmp[byte] |= (1 << bit);
                    write_block(ctx, bb_block, bmp);
                    /* Update BGDT alloc block count */
                    {
                        uint16_t fc = rd16(bgdt_buf + g * 32 + 12);
                        if (fc > 0) fc--;
                        bgdt_buf[g * 32 + 12] = fc & 0xFF;
                        bgdt_buf[g * 32 + 13] = (fc >> 8) & 0xFF;
                        write_block(ctx, ctx->first_data_block + 1, bgdt_buf);
                    }
                    return group_start + byte * 8 + bit;
                }
            }
        }
    }
    return -1;
}

/* Allocate a free inode — scan group 0 (all inodes in group 0 for now) */
int ext2_alloc_inode(ext2_ctx_t *ctx) {
    uint8_t bgdt_buf[1024];
    if (read_block(ctx, ctx->first_data_block + 1, bgdt_buf) != 0) return -1;
    uint32_t ib_block = rd32(bgdt_buf + 4); /* group 0 inode bitmap */

    uint8_t bmp[1024];
    if (read_block(ctx, ib_block, bmp) != 0) return -1;

    for (int byte = 0; byte < (int)ctx->block_size; byte++) {
        if (bmp[byte] == 0xFF) continue;
        for (int bit = 0; bit < 8; bit++) {
            if (!(bmp[byte] & (1 << bit))) {
                bmp[byte] |= (1 << bit);
                write_block(ctx, ib_block, bmp);
                /* Update BGDT alloc inode count */
                {
                    uint16_t fc = rd16(bgdt_buf + 14);
                    if (fc > 0) fc--;
                    bgdt_buf[14] = fc & 0xFF;
                    bgdt_buf[15] = (fc >> 8) & 0xFF;
                    write_block(ctx, ctx->first_data_block + 1, bgdt_buf);
                }
                return (byte * 8 + bit) + 1;
            }
        }
    }
    return -1;
}

/* Write an inode to disk */
static int write_inode(ext2_ctx_t *ctx, uint32_t ino, struct ext2_inode *inode) {
    uint32_t idx = ino - 1;
    uint32_t isz = ctx->inode_size;
    uint32_t inodes_per_block = ctx->block_size / isz;
    uint32_t block = ctx->inode_table_block + idx / inodes_per_block;
    uint32_t offset = (idx % inodes_per_block) * isz;

    uint8_t blk[1024];
    if (read_block(ctx, block, blk) != 0) return -1;

    uint8_t *dst = blk + offset;
    uint8_t *src = (uint8_t *)inode;
    for (int i = 0; i < (int)sizeof(struct ext2_inode); i++) dst[i] = src[i];

    return write_block(ctx, block, blk);
}

/* Add a directory entry to a directory */
static int add_dir_entry(ext2_ctx_t *ctx, uint32_t dir_ino, const char *name,
                         uint32_t child_ino, uint8_t ftype) {
    struct ext2_inode dir_inode;
    if (ext2_read_inode(ctx, dir_ino, &dir_inode) != 0) return -1;
    if (!(dir_inode.i_mode & 0x4000)) return -2; /* not a dir */

    int name_len = 0;
    while (name[name_len]) name_len++;
    int need = 8 + ((name_len + 3) & ~3);

    /* Walk directory blocks looking for space */
    for (int bi = 0; bi < 12 && dir_inode.i_block[bi]; bi++) {
        uint8_t blk[1024];
        if (read_block(ctx, dir_inode.i_block[bi], blk) != 0) return -3;

        int off = 0;
        while (off < (int)ctx->block_size) {
            uint16_t rec_len = rd16(blk + off + 4);
            if (rec_len == 0) break;

            uint8_t d_name_len = blk[off + 6];
            int actual = 8 + ((d_name_len + 3) & ~3);
            int slack = (int)rec_len - actual;

            if (slack >= need) {
                /* Split this entry */
                /* Shrink current entry */
                blk[off + 4] = (uint8_t)(actual & 0xFF);
                blk[off + 5] = (uint8_t)((actual >> 8) & 0xFF);

                /* Write new entry in slack space */
                int new_off = off + actual;
                int new_rec = (int)rec_len - actual;

                /* child inode */
                blk[new_off + 0] = (uint8_t)(child_ino & 0xFF);
                blk[new_off + 1] = (uint8_t)((child_ino >> 8) & 0xFF);
                blk[new_off + 2] = (uint8_t)((child_ino >> 16) & 0xFF);
                blk[new_off + 3] = (uint8_t)((child_ino >> 24) & 0xFF);
                /* rec_len */
                blk[new_off + 4] = (uint8_t)(new_rec & 0xFF);
                blk[new_off + 5] = (uint8_t)((new_rec >> 8) & 0xFF);
                /* name_len */
                blk[new_off + 6] = (uint8_t)name_len;
                /* file_type */
                blk[new_off + 7] = ftype;
                /* name */
                for (int i = 0; i < name_len; i++)
                    blk[new_off + 8 + i] = (uint8_t)name[i];

                write_block(ctx, dir_inode.i_block[bi], blk);
                return 0;
            }
            off += rec_len;
        }
    }
    return -4; /* no space */
}

int ext2_mkdir(ext2_ctx_t *ctx, uint32_t parent_ino, const char *name) {
    int new_ino = ext2_alloc_inode(ctx);
    if (new_ino < 0) return -1;
    int new_blk = ext2_alloc_block(ctx);
    if (new_blk < 0) return -2;

    /* Initialize directory inode */
    struct ext2_inode inode;
    uint8_t *p = (uint8_t *)&inode;
    for (int i = 0; i < (int)sizeof(inode); i++) p[i] = 0;
    inode.i_mode = 0x41ED; /* drwxr-xr-x */
    inode.i_size = ctx->block_size;
    inode.i_links_count = 2;
    inode.i_blocks = ctx->block_size / 512;
    inode.i_block[0] = new_blk;
    write_inode(ctx, new_ino, &inode);

    /* Initialize directory data with . and .. */
    uint8_t dir_data[1024];
    for (int i = 0; i < (int)ctx->block_size; i++) dir_data[i] = 0;

    int pos = 0;
    /* . entry */
    dir_data[pos+0] = new_ino & 0xFF; dir_data[pos+1] = (new_ino>>8) & 0xFF;
    dir_data[pos+2] = (new_ino>>16) & 0xFF; dir_data[pos+3] = (new_ino>>24) & 0xFF;
    dir_data[pos+4] = 12; dir_data[pos+5] = 0; /* rec_len=12 */
    dir_data[pos+6] = 1; dir_data[pos+7] = 2; /* name_len=1, type=dir */
    dir_data[pos+8] = '.';
    pos += 12;

    /* .. entry (takes rest of block) */
    int rest = ctx->block_size - pos;
    dir_data[pos+0] = parent_ino & 0xFF; dir_data[pos+1] = (parent_ino>>8) & 0xFF;
    dir_data[pos+2] = (parent_ino>>16) & 0xFF; dir_data[pos+3] = (parent_ino>>24) & 0xFF;
    dir_data[pos+4] = rest & 0xFF; dir_data[pos+5] = (rest>>8) & 0xFF;
    dir_data[pos+6] = 2; dir_data[pos+7] = 2;
    dir_data[pos+8] = '.'; dir_data[pos+9] = '.';

    write_block(ctx, new_blk, dir_data);

    /* Add entry in parent */
    add_dir_entry(ctx, parent_ino, name, new_ino, 2);

    return new_ino;
}

int ext2_create_file(ext2_ctx_t *ctx, uint32_t parent_ino, const char *name,
                     const void *data, int len) {
    int new_ino = ext2_alloc_inode(ctx);
    if (new_ino < 0) return -1;
    int new_blk = ext2_alloc_block(ctx);
    if (new_blk < 0) return -2;

    /* Write file data */
    uint8_t blk_data[1024];
    for (int i = 0; i < (int)ctx->block_size; i++) blk_data[i] = 0;
    int copy_len = len < (int)ctx->block_size ? len : (int)ctx->block_size;
    const uint8_t *src = (const uint8_t *)data;
    for (int i = 0; i < copy_len; i++) blk_data[i] = src[i];
    write_block(ctx, new_blk, blk_data);

    /* Create inode */
    struct ext2_inode inode;
    uint8_t *p = (uint8_t *)&inode;
    for (int i = 0; i < (int)sizeof(inode); i++) p[i] = 0;
    inode.i_mode = 0x81ED; /* -rwxr-xr-x */
    inode.i_size = len;
    inode.i_links_count = 1;
    inode.i_blocks = ctx->block_size / 512;
    inode.i_block[0] = new_blk;
    write_inode(ctx, new_ino, &inode);

    /* Add to parent directory */
    add_dir_entry(ctx, parent_ino, name, new_ino, 1);

    return new_ino;
}

int ext2_unlink(ext2_ctx_t *ctx, uint32_t parent_ino, const char *name) {
    struct ext2_inode dir_inode;
    if (ext2_read_inode(ctx, parent_ino, &dir_inode) != 0) return -1;

    int name_len = 0;
    while (name[name_len]) name_len++;

    for (int bi = 0; bi < 12 && dir_inode.i_block[bi]; bi++) {
        uint8_t blk[1024];
        if (read_block(ctx, dir_inode.i_block[bi], blk) != 0) return -2;

        int off = 0, prev_off = -1;
        while (off < (int)ctx->block_size) {
            uint32_t d_ino = rd32(blk + off);
            uint16_t rec_len = rd16(blk + off + 4);
            uint8_t d_name_len = blk[off + 6];
            if (rec_len == 0) break;

            if (d_ino && d_name_len == name_len) {
                int match = 1;
                for (int i = 0; i < name_len; i++) {
                    if (blk[off + 8 + i] != (uint8_t)name[i]) { match = 0; break; }
                }
                if (match) {
                    if (prev_off >= 0) {
                        /* Merge with previous entry */
                        uint16_t prev_rec = rd16(blk + prev_off + 4);
                        prev_rec += rec_len;
                        blk[prev_off + 4] = prev_rec & 0xFF;
                        blk[prev_off + 5] = (prev_rec >> 8) & 0xFF;
                    } else {
                        /* Zero the inode number */
                        blk[off] = 0; blk[off+1] = 0;
                        blk[off+2] = 0; blk[off+3] = 0;
                    }
                    write_block(ctx, dir_inode.i_block[bi], blk);

                    /* Free data blocks and inode for the unlinked file */
                    {
                        struct ext2_inode victim;
                        if (ext2_read_inode(ctx, d_ino, &victim) == 0 &&
                            !(victim.i_mode & EXT2_S_IFDIR)) {
                            int ptrs_per = (int)(ctx->block_size / 4);
                            /* Free direct blocks 0-11 */
                            for (int db = 0; db < 12; db++) {
                                if (victim.i_block[db])
                                    ext2_free_block(ctx, victim.i_block[db]);
                            }
                            /* Free single-indirect block and its pointers */
                            if (victim.i_block[12]) {
                                uint8_t ind[1024];
                                if (read_block(ctx, victim.i_block[12], ind) == 0) {
                                    for (int ip = 0; ip < ptrs_per; ip++) {
                                        uint32_t pb = rd32(ind + ip * 4);
                                        if (pb) ext2_free_block(ctx, pb);
                                    }
                                }
                                ext2_free_block(ctx, victim.i_block[12]);
                            }
                            /* Free double-indirect if present */
                            if (victim.i_block[13]) {
                                uint8_t dind[1024];
                                if (read_block(ctx, victim.i_block[13], dind) == 0) {
                                    for (int di = 0; di < ptrs_per; di++) {
                                        uint32_t ib2 = rd32(dind + di * 4);
                                        if (!ib2) continue;
                                        uint8_t ind2[1024];
                                        if (read_block(ctx, ib2, ind2) == 0) {
                                            for (int ip2 = 0; ip2 < ptrs_per; ip2++) {
                                                uint32_t pb2 = rd32(ind2 + ip2 * 4);
                                                if (pb2) ext2_free_block(ctx, pb2);
                                            }
                                        }
                                        ext2_free_block(ctx, ib2);
                                    }
                                }
                                ext2_free_block(ctx, victim.i_block[13]);
                            }
                            /* Zero the inode on disk */
                            uint8_t *vp = (uint8_t *)&victim;
                            for (int z = 0; z < (int)sizeof(struct ext2_inode); z++)
                                vp[z] = 0;
                            write_inode(ctx, d_ino, &victim);
                            ext2_free_inode(ctx, d_ino);
                        }
                    }
                    return 0;
                }
            }
            prev_off = off;
            off += rec_len;
        }
    }
    return -3; /* not found */
}
