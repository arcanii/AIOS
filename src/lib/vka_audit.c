/*
 * vka_audit.c -- VKA allocation counter implementation
 * v0.4.65: tracks per-subsystem resource consumption
 * v0.4.103: vka_audit_frame now also updates live count for /proc/vka
 *
 * Note: this only tracks frames we EXPLICITLY allocate from pipe/fork/exec
 * subsystems. ELF segments, BSS (morecore eager mapping), and stacks are
 * allocated implicitly by sel4utils_elf_load and not reflected here.
 * For total page accounting, use the seL4 untyped tracking.
 */
#include <stdio.h>
#include "aios/vka_audit.h"
#define LOG_MODULE "vka"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "aios/aios_log.h"

vka_audit_entry_t vka_audit[VKA_SUB_COUNT];

const char *vka_sub_names[VKA_SUB_COUNT] = {
    "boot", "fork", "exec", "thread", "pipe", "net", "gpu", "other",
    "blkcache"
};

/* v0.4.103: pool capacity for pressure warnings */
#define VKA_POOL_PAGES         8000
#define VKA_POOL_WARN_BELOW    1500
#define VKA_POOL_CRIT_BELOW    500

static int last_warned_at = 0;

void vka_audit_frame(vka_subsystem_t sub, int pages) {
    if (sub >= VKA_SUB_COUNT) sub = VKA_SUB_OTHER;
    vka_audit[sub].frames += (uint32_t)pages;
    vka_audit[sub].total_pages += (uint32_t)pages;
    /* v0.4.103: bump live count + check memory pressure */
    for (int i = 0; i < pages; i++) vka_audit_frame_alloc();
    /* Periodic warning when pool is low (every 100 allocations) */
    if (vka_live_frames - last_warned_at >= 100) {
        last_warned_at = vka_live_frames;
        int free_est = VKA_POOL_PAGES - vka_live_frames;
        if (free_est < VKA_POOL_CRIT_BELOW) {
            AIOS_LOG_ERROR_V("CRITICAL: pool nearly exhausted free_pages=",
                             (unsigned long)(free_est < 0 ? 0 : free_est));
        } else if (free_est < VKA_POOL_WARN_BELOW) {
            AIOS_LOG_WARN_V("Pool pressure: free_pages=",
                            (unsigned long)free_est);
        }
    }
}

void vka_audit_endpoint(vka_subsystem_t sub) {
    if (sub >= VKA_SUB_COUNT) sub = VKA_SUB_OTHER;
    vka_audit[sub].endpoints++;
}

void vka_audit_tcb(vka_subsystem_t sub) {
    if (sub >= VKA_SUB_COUNT) sub = VKA_SUB_OTHER;
    vka_audit[sub].tcbs++;
}

void vka_audit_cslot(vka_subsystem_t sub) {
    if (sub >= VKA_SUB_COUNT) sub = VKA_SUB_OTHER;
    vka_audit[sub].cslots++;
}

void vka_audit_untyped(vka_subsystem_t sub, int size_bits) {
    if (sub >= VKA_SUB_COUNT) sub = VKA_SUB_OTHER;
    vka_audit[sub].untypeds++;
    int pages = (1 << (size_bits - 12));
    vka_audit[sub].total_pages += (uint32_t)pages;
    /* v0.4.103: untyped also consumes pool pages */
    for (int i = 0; i < pages; i++) vka_audit_frame_alloc();
}

/* v0.4.103: called by reap/destroy paths to release frame counts.
 * Called per-page; subsystem counts are not decremented (informational
 * only) but vka_live_frames decreases for /proc/vka observability. */
void vka_audit_frame_release(int pages) {
    for (int i = 0; i < pages; i++) vka_audit_frame_free();
    if (vka_live_frames < 0) vka_live_frames = 0;
}

/* v0.4.103: Check if we have headroom before spawning a new process.
 * Returns 0 if OK, -1 if pool too low. Caller should refuse to fork/exec
 * with a clear error rather than letting the allocation silently fail.
 *
 * v0.4.113: Under pressure, evict from the block cache before failing.
 * Process pages beat cache pages -- the cache is reclaimable by design. */
int vka_audit_check_headroom(int needed_pages) {
    int free_est = VKA_POOL_PAGES - vka_live_frames;
    if (free_est < needed_pages) {
        /* Reclaim from block cache. Overshoot by 256 pages so we don't
         * thrash on every spawn. Weak symbol so this still links if
         * blk_cache.c is omitted from a future minimal build. */
        extern int blk_cache_evict(int n_pages) __attribute__((weak));
        if (blk_cache_evict) {
            int target = needed_pages - free_est + 256;
            int got = blk_cache_evict(target);
            if (got > 0) {
                AIOS_LOG_INFO_V("Reclaimed cache pages=", (unsigned long)got);
                free_est = VKA_POOL_PAGES - vka_live_frames;
            }
        }
    }
    if (free_est < needed_pages) {
        /* v0.4.263: rate-limit the rejection log. A spawn-storm at the ceiling
         * (e.g. a wide pipeline or a fork-bomb) would otherwise flood the console
         * with this line on every refused spawn. Log the first + every 64th; the
         * caller still returns a clean error to the shell ("Cannot fork") each
         * time. This is the graceful-degradation path -- the alternative was
         * sel4utils_elf_load failing mid-load + dumping a multi-line lib cascade. */
        static int reject_n = 0;
        if ((reject_n++ & 63) == 0) {
            AIOS_LOG_ERROR_V("Insufficient memory, spawn refused: free_pages=",
                             (unsigned long)(free_est < 0 ? 0 : free_est));
        }
        return -1;
    }
    return 0;
}

/* v0.4.109: Release per-process audit pages. Called before destroy.
 * Takes a pointer so it can also zero the count. */
void vka_audit_release_proc_pages(int *audit_pages_allocated) {
    if (!audit_pages_allocated) return;
    int n = *audit_pages_allocated;
    if (n > 0) vka_audit_frame_release(n);
    *audit_pages_allocated = 0;
}

void vka_audit_dump(void) {
    printf("[VKA-AUDIT] Per-subsystem allocation summary:\n");
    printf("  %-8s %6s %5s %4s %6s %6s %8s\n",
           "subsys", "frames", "eps", "tcbs", "cslots", "untypd", "tot_pg");
    uint32_t grand = 0;
    for (int i = 0; i < VKA_SUB_COUNT; i++) {
        vka_audit_entry_t *e = &vka_audit[i];
        if (e->frames || e->endpoints || e->tcbs ||
            e->cslots || e->untypeds || e->total_pages) {
            printf("  %-8s %6u %5u %4u %6u %6u %8u\n",
                   vka_sub_names[i], e->frames, e->endpoints,
                   e->tcbs, e->cslots, e->untypeds, e->total_pages);
            grand += e->total_pages;
        }
    }
    printf("  %-8s %6s %5s %4s %6s %6s %8u\n",
           "TOTAL", "", "", "", "", "", grand);
    printf("  pool = 8000 pages, remaining ~ %u pages\n", 8000 - grand);
    printf("  vka_live_frames = %d (peak %d)\n", vka_live_frames, vka_peak_frames);
}

int vka_live_frames = 0;
int vka_peak_frames = 0;

void vka_audit_frame_alloc(void) {
    vka_live_frames++;
    if (vka_live_frames > vka_peak_frames)
        vka_peak_frames = vka_live_frames;
}

void vka_audit_frame_free(void) {
    vka_live_frames--;
}
