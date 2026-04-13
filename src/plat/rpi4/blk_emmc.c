/*
 * blk_emmc.c -- BCM2835 SDHCI block driver stub (RPi4)
 *
 * Phase 1: returns -1 (no storage). Phase 2 will implement
 * SDHCI controller init and CMD17/CMD24 sector I/O.
 */
#include <stdio.h>
#include <stdint.h>
#include "plat/blk_hal.h"

int plat_blk_init(void) {
    printf("[blk] RPi4 SDHCI driver not yet implemented\n");
    return -1;
}

int plat_blk_init_log(void) {
    return -1;
}

int plat_blk_read(uint64_t sector, void *buf) {
    (void)sector; (void)buf;
    return -1;
}

int plat_blk_write(uint64_t sector, const void *buf) {
    (void)sector; (void)buf;
    return -1;
}

int plat_blk_read_log(uint64_t sector, void *buf) {
    (void)sector; (void)buf;
    return -1;
}

int plat_blk_write_log(uint64_t sector, const void *buf) {
    (void)sector; (void)buf;
    return -1;
}
