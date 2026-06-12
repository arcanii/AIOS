#ifndef AIOS_FAT32_H
#define AIOS_FAT32_H

/*
 * fat32.h -- v0.4.222 flash-over-network: in-place rewrite of KERNEL8.IMG
 * on the FAT32 boot partition (src/fat32.c). NOT a filesystem mount.
 */

#include <stdint.h>
#include "aios/ext2.h"

/* fat32_swap_kernel error codes (returned negative; 0 = success) */
#define FAT32_ERR_NOBOOT   (-2)   /* no FAT boot partition on this disk     */
#define FAT32_ERR_BPB      (-3)   /* unsupported/corrupt BPB                */
#define FAT32_ERR_SRC      (-4)   /* source file missing/unreadable/empty   */
#define FAT32_ERR_NODIRENT (-5)   /* KERNEL8.IMG not found in the root dir  */
#define FAT32_ERR_SPACE    (-6)   /* not enough free clusters               */
#define FAT32_ERR_IO       (-7)   /* sector I/O failed                      */
#define FAT32_ERR_VERIFY   (-8)   /* readback hash mismatch after commit    */
#define FAT32_ERR_CHAIN    (-9)   /* corrupt FAT chain (loop/out of range)  */
#define FAT32_ERR_TOOBIG   (-10)  /* source exceeds chain-table capacity    */

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

/* Rewrite KERNEL8.IMG on the FAT boot partition from a file on src_fs.
 * MUST run on the fs_thread (FS_FATSWAP) -- see src/fat32.c header. */
int fat32_swap_kernel(ext2_ctx_t *src_fs, const char *src_path,
                      uint32_t abort_phase, fat32_swap_result_t *out);

#endif /* AIOS_FAT32_H */
