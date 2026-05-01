/*
 * blk_cache.c -- v0.4.112 block-layer cache
 *
 * See include/aios/blk_cache.h for the design. Cache lines are 4 KB
 * (8 sectors), backed by frames allocated from vka and mapped into
 * the root vspace so we can memcpy directly. Lookup is via a simple
 * hash table; LRU eviction tracks a doubly-linked list head/tail.
 *
 * Locking: AIOS root task is single-threaded across cache callers
 * (ext2 calls happen on the fs_thread, log calls on the same path,
 * warmup runs sequentially before login). No locks needed; the cache
 * data structures are a private global.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sel4/sel4.h>
#include <sel4utils/vspace.h>
#include <vka/object.h>
#include "aios/root_shared.h"
#include "aios/blk_cache.h"
#include "aios/vka_audit.h"
#define LOG_MODULE "cache"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "aios/aios_log.h"

#define HASH_BUCKETS 512

typedef struct cache_line {
    uint32_t drive;
    uint64_t line_sector;     /* base sector, multiple of LINE_SECTORS */
    uint8_t *data;            /* mapped 4 KB region */
    vka_object_t frame;
    uint32_t lru_tick;
    uint8_t  valid;
    uint8_t  dirty;           /* unused in v0.4.112 (write-through) */
    struct cache_line *hash_next;
    struct cache_line *prev, *next;  /* LRU: head=most recent, tail=oldest */
} cache_line_t;

static cache_line_t *bucket_head[HASH_BUCKETS];
static cache_line_t *lru_head = NULL;   /* MRU */
static cache_line_t *lru_tail = NULL;   /* LRU -- evict from here */
static int          line_count = 0;
static int          line_max   = BLK_CACHE_DEFAULT_MAX_PAGES;
static uint32_t     monotonic_tick = 0;

static blk_backend_read_fn  backend_read[2]  = { NULL, NULL };
static blk_backend_write_fn backend_write[2] = { NULL, NULL };

static blk_cache_stats_t stats;

static inline uint32_t hash_key(uint32_t drive, uint64_t line_sector) {
    /* Mix drive and line_sector; line_sector is already multiple of 8
     * so the low bits are sparse -- shift down before mixing. */
    uint64_t k = (uint64_t)drive * 0x9e3779b97f4a7c15ULL
               ^ (line_sector >> 3) * 0xbf58476d1ce4e5b9ULL;
    return (uint32_t)(k % HASH_BUCKETS);
}

static cache_line_t *lookup(uint32_t drive, uint64_t line_sector) {
    uint32_t h = hash_key(drive, line_sector);
    for (cache_line_t *p = bucket_head[h]; p; p = p->hash_next) {
        if (p->drive == drive && p->line_sector == line_sector) return p;
    }
    return NULL;
}

static void hash_insert(cache_line_t *l) {
    uint32_t h = hash_key(l->drive, l->line_sector);
    l->hash_next = bucket_head[h];
    bucket_head[h] = l;
}

static void hash_remove(cache_line_t *l) {
    uint32_t h = hash_key(l->drive, l->line_sector);
    cache_line_t **pp = &bucket_head[h];
    while (*pp) {
        if (*pp == l) { *pp = l->hash_next; l->hash_next = NULL; return; }
        pp = &(*pp)->hash_next;
    }
}

static void lru_unlink(cache_line_t *l) {
    if (l->prev) l->prev->next = l->next; else lru_head = l->next;
    if (l->next) l->next->prev = l->prev; else lru_tail = l->prev;
    l->prev = l->next = NULL;
}

static void lru_push_front(cache_line_t *l) {
    l->prev = NULL;
    l->next = lru_head;
    if (lru_head) lru_head->prev = l;
    lru_head = l;
    if (!lru_tail) lru_tail = l;
}

static void lru_touch(cache_line_t *l) {
    l->lru_tick = ++monotonic_tick;
    if (lru_head == l) return;
    lru_unlink(l);
    lru_push_front(l);
}

/* Allocate a fresh line backed by a vka frame mapped in root vspace.
 * Returns NULL on failure (vka exhausted). */
static cache_line_t *line_alloc(void) {
    cache_line_t *l = malloc(sizeof(*l));
    if (!l) return NULL;
    memset(l, 0, sizeof(*l));
    if (vka_alloc_frame(&vka, seL4_PageBits, &l->frame)) {
        free(l);
        return NULL;
    }
    /* Map the frame into root vspace so we can memcpy. cookie=ut so
     * if root vspace ever tears down it would free the frame too. */
    void *va = vspace_map_pages(&vspace, &l->frame.cptr, NULL,
                                seL4_AllRights, 1, seL4_PageBits, 1);
    if (!va) {
        vka_free_object(&vka, &l->frame);
        free(l);
        return NULL;
    }
    l->data = (uint8_t *)va;
    vka_audit_frame(VKA_SUB_BLKCACHE, 1);
    line_count++;
    return l;
}

static void line_free(cache_line_t *l) {
    if (l->data) {
        vspace_unmap_pages(&vspace, l->data, 1, seL4_PageBits, NULL);
    }
    vka_free_object(&vka, &l->frame);
    vka_audit_frame_release(1);
    line_count--;
    free(l);
}

/* Evict the LRU tail. Returns 1 if a line was freed, 0 if the cache
 * is empty. */
static int evict_one(void) {
    cache_line_t *victim = lru_tail;
    if (!victim) return 0;
    /* Write-through means no dirty data to flush. Just unlink. */
    lru_unlink(victim);
    hash_remove(victim);
    line_free(victim);
    stats.evicted++;
    return 1;
}

void blk_cache_init(int max_pages) {
    memset(&stats, 0, sizeof(stats));
    memset(bucket_head, 0, sizeof(bucket_head));
    lru_head = lru_tail = NULL;
    line_count = 0;
    monotonic_tick = 0;
    line_max = max_pages > 0 ? max_pages : BLK_CACHE_DEFAULT_MAX_PAGES;
    stats.pages_max = (uint32_t)line_max;
    AIOS_LOG_INFO_V("init max_pages=", (unsigned long)line_max);
}

void blk_cache_register_backend(int drive,
                                blk_backend_read_fn read_fn,
                                blk_backend_write_fn write_fn) {
    if (drive < 0 || drive > 1) return;
    backend_read[drive]  = read_fn;
    backend_write[drive] = write_fn;
}

/* Ensure a line exists for (drive, line_sector). Reads from backend
 * on miss. Returns the line, or NULL on hard failure. */
static cache_line_t *get_line(uint32_t drive, uint64_t line_sector) {
    cache_line_t *l = lookup(drive, line_sector);
    if (l) {
        stats.hits++;
        lru_touch(l);
        return l;
    }
    stats.misses++;

    /* Make room if at the cap */
    while (line_count >= line_max) {
        if (!evict_one()) break;
    }

    /* Allocate fresh */
    l = line_alloc();
    if (!l) {
        /* Try one more eviction in case allocation failed under pressure */
        if (evict_one()) l = line_alloc();
        if (!l) return NULL;
    }
    l->drive = drive;
    l->line_sector = line_sector;

    /* Read all 8 sectors of the line from backend */
    blk_backend_read_fn rd = backend_read[drive];
    if (!rd) { line_free(l); return NULL; }
    for (int i = 0; i < BLK_CACHE_LINE_SECTORS; i++) {
        if (rd(line_sector + i, l->data + i * 512) != 0) {
            line_free(l);
            return NULL;
        }
    }
    l->valid = 1;

    hash_insert(l);
    lru_push_front(l);
    lru_touch(l);
    return l;
}

/* Read one sector via the cache. Computes the containing line, fills
 * if needed, copies the sector slice out. */
static int cache_read_sector(uint32_t drive, uint64_t sector, void *buf) {
    uint64_t line_sector = sector & ~(uint64_t)(BLK_CACHE_LINE_SECTORS - 1);
    int offset = (int)(sector - line_sector);
    cache_line_t *l = get_line(drive, line_sector);
    if (!l) return -1;
    memcpy(buf, l->data + offset * 512, 512);
    return 0;
}

/* Write-through: update the line if cached, then push to backend. */
static int cache_write_sector(uint32_t drive, uint64_t sector, const void *buf) {
    uint64_t line_sector = sector & ~(uint64_t)(BLK_CACHE_LINE_SECTORS - 1);
    int offset = (int)(sector - line_sector);
    cache_line_t *l = lookup(drive, line_sector);
    if (l) {
        memcpy(l->data + offset * 512, buf, 512);
        lru_touch(l);
    }
    blk_backend_write_fn wr = backend_write[drive];
    if (!wr) return -1;
    stats.writes++;
    return wr(sector, buf);
}

int blk_cache_read0 (uint64_t s, void *b)        { return cache_read_sector(0, s, b); }
int blk_cache_write0(uint64_t s, const void *b)  { return cache_write_sector(0, s, b); }
int blk_cache_read1 (uint64_t s, void *b)        { return cache_read_sector(1, s, b); }
int blk_cache_write1(uint64_t s, const void *b)  { return cache_write_sector(1, s, b); }

int blk_cache_evict(int n_pages) {
    int freed = 0;
    while (freed < n_pages && evict_one()) freed++;
    return freed;
}

void blk_cache_flush(void) {
    /* Write-through: nothing to flush. Reserved for future write-back. */
}

void blk_cache_stats(blk_cache_stats_t *out) {
    *out = stats;
    out->pages = (uint32_t)line_count;
    out->pages_max = (uint32_t)line_max;
}
