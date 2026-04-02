#ifndef AIOS_EXT2_H
#define AIOS_EXT2_H

#include <stdint.h>

/* ext2 superblock (at offset 1024 on disk) */
struct __attribute__((packed)) ext2_super {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint32_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
};

/* ext2 group descriptor */
struct __attribute__((packed)) ext2_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
};

/* ext2 inode */
struct __attribute__((packed)) ext2_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
};

/* ext2 directory entry */
struct __attribute__((packed)) ext2_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
};

#define EXT2_MAGIC 0xEF53
#define EXT2_ROOT_INO 2
#define EXT2_FT_REG  1
#define EXT2_FT_DIR  2
#define EXT2_S_IFDIR 0x4000
#define EXT2_S_IFREG 0x8000

/* FS IPC protocol */
#define FS_LS   10
#define FS_CAT  11
#define FS_STAT 12

/* Block read function type (provided by caller) */
typedef int (*blk_read_fn)(uint64_t sector, void *buf);

/* ext2 context */
typedef struct {
    blk_read_fn read_sector;
    uint32_t block_size;
    uint32_t inodes_per_group;
    uint32_t inode_table_block;
    uint32_t blocks_per_group;
    uint32_t first_data_block;
} ext2_ctx_t;

int ext2_init(ext2_ctx_t *ctx, blk_read_fn read);
int ext2_read_inode(ext2_ctx_t *ctx, uint32_t ino, struct ext2_inode *out);
int ext2_read_block(ext2_ctx_t *ctx, uint32_t block, void *buf);
int ext2_lookup(ext2_ctx_t *ctx, uint32_t dir_ino, const char *name, uint32_t *out_ino);
int ext2_list_dir(ext2_ctx_t *ctx, uint32_t dir_ino, char *buf, int bufsize);
int ext2_read_file(ext2_ctx_t *ctx, uint32_t ino, char *buf, int bufsize);
int ext2_resolve_path(ext2_ctx_t *ctx, const char *path, uint32_t *out_ino);

#endif
