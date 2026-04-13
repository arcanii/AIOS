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

/* Sector I/O for log device. */
int plat_blk_read_log(uint64_t sector, void *buf);
int plat_blk_write_log(uint64_t sector, const void *buf);

#endif /* AIOS_BLK_HAL_H */
