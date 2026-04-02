#include "ext2.h"
#include <stdio.h>

/* Read uint16/uint32 from raw buffer (little-endian) */
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

static int read_block(ext2_ctx_t *ctx, uint32_t block, void *buf) {
    int sectors = ctx->block_size / 512;
    uint8_t *p = (uint8_t *)buf;
    for (int i = 0; i < sectors; i++) {
        if (ctx->read_sector((uint64_t)block * sectors + i, p + i * 512) != 0)
            return -1;
    }
    return 0;
}

int ext2_init(ext2_ctx_t *ctx, blk_read_fn read) {
    ctx->read_sector = read;

    /* Superblock at offset 1024 = sectors 2-3 */
    uint8_t sb[1024];
    if (read(2, sb) != 0) return -1;
    if (read(3, sb + 512) != 0) return -1;

    uint16_t magic = rd16(sb + 0x38);
    printf("[ext2] magic=0x%04x blocks=%u inodes=%u\n",
           magic, rd32(sb + 4), rd32(sb + 0));
    if (magic != EXT2_MAGIC) return -2;

    ctx->block_size = 1024 << rd32(sb + 24);
    ctx->inodes_per_group = rd32(sb + 40);
    ctx->blocks_per_group = rd32(sb + 32);
    ctx->first_data_block = rd32(sb + 20);

    /* Group descriptor (block after superblock) */
    uint8_t gd[1024];
    if (read_block(ctx, ctx->first_data_block + 1, gd) != 0) return -3;
    ctx->inode_table_block = rd32(gd + 8);

    printf("[ext2] block_size=%u inode_table=%u\n",
           ctx->block_size, ctx->inode_table_block);
    return 0;
}

int ext2_read_inode(ext2_ctx_t *ctx, uint32_t ino, struct ext2_inode *out) {
    uint32_t idx = ino - 1;
    uint32_t inode_size = 128;
    uint32_t inodes_per_block = ctx->block_size / inode_size;
    uint32_t block = ctx->inode_table_block + idx / inodes_per_block;
    uint32_t offset = (idx % inodes_per_block) * inode_size;

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

int ext2_read_file(ext2_ctx_t *ctx, uint32_t ino, char *buf, int bufsize) {
    struct ext2_inode inode;
    if (ext2_read_inode(ctx, ino, &inode) != 0) return -1;

    int size = (int)inode.i_size;
    if (size > bufsize - 1) size = bufsize - 1;

    int copied = 0;
    for (int bi = 0; bi < 12 && inode.i_block[bi] && copied < size; bi++) {
        uint8_t blk[1024];
        if (read_block(ctx, inode.i_block[bi], blk) != 0) return -2;
        int chunk = size - copied;
        if (chunk > (int)ctx->block_size) chunk = (int)ctx->block_size;
        for (int i = 0; i < chunk; i++) buf[copied++] = blk[i];
    }
    buf[copied] = '\0';
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
