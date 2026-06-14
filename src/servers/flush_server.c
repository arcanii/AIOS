/*
 * flush_server.c -- v0.4.188 periodic write-back flusher.
 *
 * A dedicated root-task thread that, every FLUSH_PERIOD_SEC, asks the
 * fs_thread to flush the write-back block cache (FS_SYNC). This bounds the
 * data-loss window of drive 0's write-back policy: the threshold flush only
 * fires once 16 dirty lines accrue, so a burst of writes smaller than that
 * (or the tail of a larger one) could otherwise sit unflushed until the next
 * eviction or a shutdown that a power cut never reaches.
 *
 * Why a dedicated thread that sends FS_SYNC, rather than calling
 * blk_cache_flush() directly: the block cache and the backend DMA ring are
 * UNLOCKED and assume a single owner thread (blk_cache.c:9-12). All block IO
 * is serialized through the fs_thread. Routing the flush through FS_SYNC makes
 * blk_cache_flush() run on the fs_thread, preserving that invariant -- a
 * second thread touching the cache/ring concurrently would corrupt IO. This
 * mirrors serverstats.c, which uses a dedicated thread for the same
 * non-MCS-reply-cap-safety reason.
 *
 * The flush is a no-op when nothing is dirty (flush_all_dirty skips clean
 * lines), so a quiet system pays only one cheap IPC per period.
 */
#include "aios/root_shared.h"
#include <sel4/sel4.h>
#include <sel4utils/thread.h>
#include "aios/ext2.h"
#include <stdio.h>
#include <string.h>
#define LOG_MODULE "flush"
#define LOG_LEVEL  LOG_LEVEL_INFO
#include "aios/aios_log.h"
#include "aios/cpuacct.h"

/* Write-back durability window. 30s bounds the worst-case loss of sub-threshold
 * dirty data to the last half-minute, while keeping eMMC wear and the IPC
 * negligible. The log drive is write-through, so only drive 0 is at stake. */
#define FLUSH_PERIOD_SEC  30

static uint32_t flush_count;       /* periodic flushes issued */
static uint64_t last_flush_us;     /* cntpct time of the last issued flush */

static uint64_t flush_now_us(void) {
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    return cnt * 1000000ULL / freq;
}

static void flush_sleep(uint32_t sec) {
    uint64_t target = flush_now_us() + (uint64_t)sec * 1000000ULL;
    while (flush_now_us() < target) {
        seL4_Yield();
    }
}

static void flush_thread_fn(void *a, void *b, void *c) {
    (void)a; (void)b; (void)c;
    while (1) {
        flush_sleep(FLUSH_PERIOD_SEC);
        if (!fs_ep_cap) continue;
        /* FS_SYNC runs blk_cache_flush() on the fs_thread (serialized IO). */
        seL4_Call(fs_ep_cap, seL4_MessageInfo_new(FS_SYNC, 0, 0, 0));
        flush_count++;
        last_flush_us = flush_now_us();
    }
}

void flush_server_init(void) {
    sel4utils_thread_t thread;
    int err = sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
        simple_get_cnode(&simple), seL4_NilData, &thread);
    if (err) {
        AIOS_LOG_WARN_V("flush: thread configure failed err=", (unsigned long)err);
        return;
    }
    /* Same priority (200) and core (0) as the other root threads -- it spends
     * almost all its time blocked in seL4_Call to the fs_thread, so it does not
     * steal CPU; running at server priority avoids starvation (see serverstats). */
    seL4_TCB_SetPriority(thread.tcb.cptr, simple_get_tcb(&simple), 200);
    #if CONFIG_MAX_NUM_NODES > 1
    seL4_TCB_SetAffinity(thread.tcb.cptr, 0);
    #endif
    err = sel4utils_start_thread(&thread, flush_thread_fn, NULL, NULL, 1);
    if (err) {
        AIOS_LOG_WARN_V("flush: thread start failed err=", (unsigned long)err);
        return;
    }
    aios_acct_register("flush", thread.tcb.cptr);   /* /proc/cpuacct */
    AIOS_LOG_INFO("write-back flusher thread started");
}

int flush_server_format(char *buf, int bufsize) {
    uint64_t now = flush_now_us();
    uint64_t age_s = last_flush_us ? (now - last_flush_us) / 1000000ULL : 0;
    return snprintf(buf, bufsize,
        "period_s=%d flushes=%u last_age_s=%llu\n",
        FLUSH_PERIOD_SEC, flush_count, (unsigned long long)age_s);
}
