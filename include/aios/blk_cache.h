#ifndef AIOS_BLK_CACHE_H
#define AIOS_BLK_CACHE_H
/*
 * blk_cache.h -- v0.4.112 block-layer cache
 *
 * Sits between ext2 (which calls blk_read_fn / blk_write_fn) and the
 * platform HAL (plat_blk_read / plat_blk_write). 4 KB cache lines,
 * each holding 8 contiguous 512 B sectors. Hash table for O(1) lookup,
 * doubly-linked LRU for eviction. Lines are backed by 4 KB frames
 * allocated from vka under VKA_SUB_BLKCACHE so /proc/vka shows the
 * cache footprint and /proc/cachestats reports hit rate.
 *
 * Phase 1 (v0.4.112) is write-through: every write goes straight to
 * the underlying HAL and updates the cache line. Phase 2 may move to
 * write-back with a dirty list and explicit flush, but only after we
 * confirm the cache is stable under concurrent fs traffic.
 *
 * Drive id matches the dev_id passed to ext2_init: 0 = system disk,
 * 1 = log drive. The cache transparently routes (drive, sector) to
 * the right plat_blk_read/write_log function via init-time wiring
 * (blk_cache_register_backend).
 */
#include <stdint.h>

/* 4 KB cache line = 8 sectors. Page-aligned reads dominate after
 * ext2 inode lookup; smaller reads (single-sector superblock probe)
 * still use a full line. */
#define BLK_CACHE_LINE_SECTORS  8
#define BLK_CACHE_LINE_BYTES    4096

/* Configurable cap; default 2048 pages = 8 MB. Eviction respects
 * this limit AND drops lines when vka headroom is tight. */
#define BLK_CACHE_DEFAULT_MAX_PAGES  2048

typedef int (*blk_backend_read_fn)(uint64_t sector, void *buf);
typedef int (*blk_backend_write_fn)(uint64_t sector, const void *buf);

/* Initialize cache with up to max_pages 4 KB lines. Must be called
 * after vka is up but before ext2_init. Pass 0 to use the default. */
void blk_cache_init(int max_pages);

/* Register a backend for a drive id. drive=0 system disk, drive=1
 * log drive. Pass NULL for write_fn if the device is read-only. */
void blk_cache_register_backend(int drive,
                                blk_backend_read_fn read_fn,
                                blk_backend_write_fn write_fn);

/* The read/write entry points used as the blk_read_fn / blk_write_fn
 * passed to ext2_init / ext2_init_write. drive_id is implicit -- use
 * blk_cache_read0 for drive 0 and blk_cache_read1 for drive 1. */
int blk_cache_read0(uint64_t sector, void *buf);
int blk_cache_write0(uint64_t sector, const void *buf);
int blk_cache_read1(uint64_t sector, void *buf);
int blk_cache_write1(uint64_t sector, const void *buf);

/* Drop up to n_pages cold lines back to vka. Returns the number
 * actually freed (may be less if the cache is empty or all lines
 * are pinned -- pinning is reserved for future use). Called from
 * vka_audit_check_headroom under memory pressure. */
int blk_cache_evict(int n_pages);

/* Force write-back of any dirty lines (no-op in v0.4.112 since cache
 * is write-through, but the symbol is reserved for shutdown). */
void blk_cache_flush(void);

/* Snapshot stats for /proc/cachestats. */
typedef struct {
    uint32_t hits;
    uint32_t misses;
    uint32_t pages;          /* current cache size in 4 KB pages */
    uint32_t pages_max;      /* configured cap */
    uint32_t evicted;        /* cumulative line evictions */
    uint32_t writes;         /* cumulative writes through cache */
} blk_cache_stats_t;

void blk_cache_stats(blk_cache_stats_t *out);

#endif
