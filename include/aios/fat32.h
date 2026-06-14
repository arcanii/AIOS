#ifndef AIOS_FAT32_H
#define AIOS_FAT32_H

/*
 * fat32.h -- v0.4.222 flash-over-network: crash-safe rewrite of an existing
 * root-directory file (kernel8.img, config.txt, ...) on the FAT32 boot
 * partition (src/fat32.c), plus a read path. NOT a filesystem mount: never
 * creates/moves dirents, never reformats.
 */

#include <stdint.h>
#include "aios/ext2.h"

/* fat32 error codes (returned negative; 0 = success) */
#define FAT32_ERR_NOBOOT   (-2)   /* no FAT boot partition on this disk     */
#define FAT32_ERR_BPB      (-3)   /* unsupported/corrupt BPB                */
#define FAT32_ERR_SRC      (-4)   /* source file missing/unreadable/empty   */
#define FAT32_ERR_NODIRENT (-5)   /* target file not found in the root dir  */
#define FAT32_ERR_SPACE    (-6)   /* not enough free clusters               */
#define FAT32_ERR_IO       (-7)   /* sector I/O failed                      */
#define FAT32_ERR_VERIFY   (-8)   /* readback hash mismatch after commit    */
#define FAT32_ERR_CHAIN    (-9)   /* corrupt FAT chain (loop/out of range)  */
#define FAT32_ERR_TOOBIG   (-10)  /* source exceeds chain-table / dst cap    */
#define FAT32_ERR_NAME     (-11)  /* target name not 8.3-representable       */

/* Debug abort phases: stop between crash-safety ordering steps so tests can
 * prove a power cut at that point leaves a bootable kernel. */
#define FAT32_ABORT_NONE          0
#define FAT32_ABORT_AFTER_DATA    1  /* data written; FAT + dirent untouched */
#define FAT32_ABORT_AFTER_FAT     2  /* new chain linked; dirent still old   */
#define FAT32_ABORT_AFTER_DIRENT  3  /* dirent committed; old chain leaked   */

typedef struct {
    uint32_t bytes;          /* bytes written                          */
    uint32_t clusters;       /* clusters in the new chain              */
    uint32_t first_cluster;  /* head of the new chain                  */
    uint8_t  sha_src[32];    /* sha256 of the source bytes as written  */
    uint8_t  sha_disk[32];   /* sha256 read back via the new dirent    */
} fat32_swap_result_t;

/* Rewrite an existing root-dir file (target_name, 8.3 e.g. "kernel8.img" or
 * "config.txt") on the FAT boot partition from a file on src_fs. Crash-safe
 * ordering; variable size (allocates/frees clusters). MUST run on the fs_thread
 * (FS_FATSWAP) -- see src/fat32.c header. */
int fat32_write_file(ext2_ctx_t *src_fs, const char *src_path,
                     const char *target_name, uint32_t abort_phase,
                     fat32_swap_result_t *out);

/* Read an existing root-dir file off the FAT into dst (up to max bytes); fills
 * *out_size. Read-only. MUST run on the fs_thread. */
int fat32_read_file(const char *target_name, uint8_t *dst, uint32_t max,
                    uint32_t *out_size);

#endif /* AIOS_FAT32_H */
