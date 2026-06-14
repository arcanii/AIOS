/*
 * blk_hal.h -- Block device Hardware Abstraction Layer
 *
 * Platform-agnostic interface for sector I/O.
 * Matches ext2 blk_read_fn / blk_write_fn signatures.
 */
#ifndef AIOS_BLK_HAL_H
#define AIOS_BLK_HAL_H

#include <stdint.h>

/* Initialize primary block device (system disk).
 * Returns 0 on success. After success, plat_blk_read/write are valid. */
int plat_blk_init(void);

/* Initialize secondary block device (log drive).
 * Returns 0 on success, -1 if no second device available. */
int plat_blk_init_log(void);

/* Sector I/O for primary device.
 * Signatures match ext2 blk_read_fn / blk_write_fn. */
int plat_blk_read(uint64_t sector, void *buf);
int plat_blk_write(uint64_t sector, const void *buf);

/* v0.4.172: write `count` contiguous sectors in one operation (multi-block).
 * Lets the block cache flush a whole 4 KB line as one transfer (RPi4 CMD25),
 * the core of the write-back file-write speedup. Returns 0 on success. */
int plat_blk_write_multi(uint64_t sector, const void *buf, int count);

/* Read `count` contiguous sectors in one operation (multi-block). Lets the
 * block cache fill a whole 4 KB line as one transfer (RPi4 CMD18). Mirrors
 * plat_blk_write_multi. Returns 0 on success. */
int plat_blk_read_multi(uint64_t sector, void *buf, int count);

/* Discard (erase) `count` contiguous sectors the filesystem just freed
 * (RPi4 CMD38 ERASE). Best-effort -- a failure is never data loss. No-op on
 * backends without discard support. Returns 0 on success. */
int plat_blk_discard(uint64_t sector, int count);

/* Sector I/O for log device. */
int plat_blk_read_log(uint64_t sector, void *buf);
int plat_blk_write_log(uint64_t sector, const void *buf);

/* v0.4.222 fatswap: ABSOLUTE-LBA I/O on the primary device -- whole-card
 * sector space, no ext2 partition offset. Used to rewrite KERNEL8.IMG on
 * the FAT32 boot partition (src/fat32.c). These bypass the block cache by
 * design; that is coherent because the cache only ever addresses
 * ext2-partition-relative sectors, which map to a disjoint physical range.
 * MUST be called from the fs_thread only (single-owner DMA/PIO state). */
int plat_blk_read_abs(uint64_t sector, void *buf);
int plat_blk_write_abs(uint64_t sector, const void *buf);
int plat_blk_write_multi_abs(uint64_t sector, const void *buf, int count);
int plat_blk_read_multi_abs(uint64_t sector, void *buf, int count);

/* FAT32/FAT16 boot partition (MBR type 0x0b/0x0c) discovered at
 * plat_blk_init. start==0 means no boot partition on this disk. */
uint64_t plat_blk_boot_part_start(void);
uint64_t plat_blk_boot_part_sectors(void);

#endif /* AIOS_BLK_HAL_H */
