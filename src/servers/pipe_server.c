#define LOG_MODULE "pipe"
#define LOG_LEVEL  LOG_LEVEL_INFO
#include "aios/aios_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sel4/sel4.h>
#include <sel4utils/process.h>
#include <sel4utils/process_config.h>
#include <sel4utils/vspace.h>
#include <sel4utils/api.h>
#include <vka/capops.h>
#include <vka/object.h>
#include <elf/elf.h>
#include <sel4utils/elf.h>
#include <simple/simple.h>
#include "aios/root_shared.h"
#include "aios/mono_wait.h"   /* diag v0.4.201 */

/* diag v0.4.201: per-message latency probe state */
static seL4_Word diag_prev_label = (seL4_Word)~0ul;
static seL4_Word diag_prev_badge;
static uint64_t  diag_prev_t0;
static uint64_t pipe_diag_ms(uint64_t t0) {
    uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (!f) f = 54000000;
    return (mono_ticks() - t0) * 1000ull / f;
}
#include "aios/vka_audit.h"
/* Signal fetch: client retrieves and clears pending bits */
#ifndef PIPE_SIG_FETCH
#define PIPE_SIG_FETCH 76
#endif

#include "aios/vfs.h"
#include "aios/procfs.h"
#include "aios/cow.h"

/* v0.4.142: declared in sel4utils/vspace_internal.h, which we cannot include
 * here (that header carries many static helper definitions). Returns the
 * frame cap mapped at vaddr, or 0 if the page is unmapped. */
seL4_CPtr sel4utils_get_cap(vspace_t *vspace, void *vaddr);

/* ---- Blocked reader table ---- */
static pipe_read_blocked_t pipe_read_blocked[MAX_PIPE_READ_BLOCKED];
static vka_object_t read_reply_objs[MAX_PIPE_READ_BLOCKED];
/* v0.4.139: per-pipe flag set once a child has been exec'd onto the pipe.
 * Gates the orphaned-pipe reclaim so an in-flight (just-created, not yet
 * child-associated) pipe is never reclaimed. Separate array (not a pipe_t
 * field) to avoid shifting the root-task BSS layout. */
static unsigned char pipe_had_child[MAX_PIPES];

/* ---- Exec-wait barrier for SMP fork serialization ---- */
static struct {
    int active;
    int child_idx;
    seL4_CPtr reply_cap;
} exec_wait;
static vka_object_t exec_wait_reply_obj;
static int exec_done[MAX_ACTIVE_PROCS];

/* v0.4.140 bug-1 fix: per-proc BSS reservation stored BY VALUE here. The
 * old code set bss_reservation to the malloc-d sel4utils_res_t pointer that
 * vspace_reserve_range_at returns; libsel4utils later frees and recycles
 * that struct, so the stored pointer dangled and described an unrelated
 * range (bounds=0 BSS-fault). A stable per-proc struct filled via
 * sel4utils_reserve_range_at_no_alloc has malloced=0, so libsel4utils never
 * frees it and the pointer cannot dangle. Shared with exec_server.c. */
sel4utils_res_t aios_bss_res[MAX_ACTIVE_PROCS];

/* ===================== v0.4.258: direct SPSC SHM-ring pipes =====================
 *
 * A ring-mode pipe's data lives in ONE shared frame (include/aios/shm_ring.h) that
 * the writer and reader map directly: bytes flow in userspace on the producer/
 * consumer cores, and the pipe_server is touched only at empty/full transitions.
 * This takes the kernel (+ its big lock) out of the per-chunk data path so a
 * `cmd | cmd` pipeline can span cores instead of serialising every chunk through
 * the core-0 server.
 *
 * The ring is the pipe's ONE buffer in ring mode: whether an end moves bytes
 * itself (direct, the fast path) or falls back to legacy IPC (PIPE_WRITE/READ* --
 * used only if its PIPE_MAP_RING failed), both operate on the SAME ring via the
 * helpers below, so a failed map never splits data across two buffers. Mode is
 * latched per-pipe at PIPE_CREATE from g_shm_ring; a later toggle affects only
 * new pipes. Default OFF -> armed=0 for every pipe -> every pipe_is_ring() check
 * is false -> the legacy path is byte-identical to before this change. */
#include "aios/shm_ring.h"

/* /proc/shmring[.0|.1] -- arm/disarm the SHM-ring for pipes created afterwards.
 * Default OFF: the cross-core coherency path (cacheable inner-shareable + the
 * release/acquire/fence handshake) needs real-A72 soak; QEMU cannot model it. */
volatile int g_shm_ring = 0;
/* Observability counters (shown in /proc/shmring). A direct writer/reader never
 * touches the server, so push/pull stay 0 in the pure fast path; nonzero push/pull
 * = an end fell back to server-mediated IPC (line-buffered writers go through the
 * MR PIPE_WRITE path, so push is normal). park = times a full-ring writer blocked;
 * wake = PIPE_RING_WAKE calls from direct writers. */
static unsigned long g_ring_map_ok, g_ring_map_fail, g_ring_push, g_ring_pull,
                     g_ring_wake, g_ring_park;

typedef struct {
    int          armed;        /* ring mode chosen for this pipe at PIPE_CREATE */
    int          valid;        /* frame allocated + root-mapped (header live) */
    vka_object_t frame;
    char        *buf;          /* root mapping; shm_ring_hdr_t header at offset 0 */
    seL4_CPtr    copies[4];    /* child cap copies (writer + reader, + a couple fork dups) */
    int          copy_count;
} pipe_ring_t;
static pipe_ring_t pipe_ring[MAX_PIPES];

/* v0.4.258: a writer that fills a ring blocks in the server (SaveCaller) instead
 * of spin-dropping data -- symmetric to the reader blocking on an empty ring. SPSC
 * => at most one parked writer per pipe. Its bytes sit in xfer_buf_w (the client is
 * blocked in the Call, so it cannot overwrite them). The reader's drain (which frees
 * space) pushes them and replies the accepted count; teardown replies -1 (EPIPE).
 * Per-pipe reply-cap slot is lazy-allocated on first park. */
typedef struct {
    int active; seL4_CPtr reply_cap; int wlen;
    int is_mr;            /* 1: bytes are in `stash` (MR PIPE_WRITE); 0: in xfer_buf_w */
    char stash[900];      /* MR-write payload (<=900) held while parked */
} ring_wblock_t;
static ring_wblock_t ring_wblock[MAX_PIPES];
static vka_object_t  ring_wblock_obj[MAX_PIPES];
static int           ring_wblock_obj_valid[MAX_PIPES];

int shmring_cmd(const char *args, char *buf, int bufsize) {
    if (args[0] == '.' && (args[1] == '0' || args[1] == '1'))
        g_shm_ring = (args[1] == '1');
    return snprintf(buf, bufsize,
        "shmring: enabled=%d data=%uB  map_ok=%lu map_fail=%lu push=%lu pull=%lu wake=%lu park=%lu\n",
        g_shm_ring, (unsigned)SHM_RING_DATA, g_ring_map_ok, g_ring_map_fail,
        g_ring_push, g_ring_pull, g_ring_wake, g_ring_park);
}

static inline int pipe_is_ring(int pi) {
    return pi >= 0 && pi < MAX_PIPES && pipe_ring[pi].armed && pipe_ring[pi].valid;
}
static inline shm_ring_hdr_t *ring_hdr(int pi) {
    return pipe_is_ring(pi) ? (shm_ring_hdr_t *)pipe_ring[pi].buf : (shm_ring_hdr_t *)0;
}

/* Allocate + root-map the ring frame and initialise its header. Mirrors the
 * PIPE_CREATE shm_frame alloc. Returns 0 on success (valid=1), -1 on failure. */
static int ring_alloc(int pi) {
    pipe_ring[pi].valid = 0;
    pipe_ring[pi].buf = NULL;
    pipe_ring[pi].copy_count = 0;
    vka_audit_frame(VKA_SUB_PIPE, 1);
    if (vka_alloc_frame(&vka, seL4_PageBits, &pipe_ring[pi].frame) != 0) {
        vka_audit_frame_release(1);
        return -1;
    }
    /* Cacheable (inner-shareable on the A72): the writer + reader map it the same
     * way, so the cores stay coherent (the v0.4.164 all-NUL bug was a memory-type
     * mismatch -- map both ends identically). */
    void *m = vspace_map_pages(&vspace, &pipe_ring[pi].frame.cptr, NULL,
        seL4_AllRights, 1, seL4_PageBits, 1 /* cacheable */);
    if (!m) {
        vka_free_object(&vka, &pipe_ring[pi].frame);
        vka_audit_frame_release(1);
        return -1;
    }
    pipe_ring[pi].buf = (char *)m;
    shm_ring_hdr_t *h = (shm_ring_hdr_t *)m;
    memset(h, 0, SHM_RING_DATAOFF);
    h->magic = SHM_RING_MAGIC;
    h->size  = SHM_RING_DATA;
    shm_ring_fence();   /* header initialised before any child maps + reads it */
    pipe_ring[pi].valid = 1;
    return 0;
}

/* Free the ring frame + child cap copies. Called from pipe_maybe_free. */
static void ring_free(int pi) {
    if (ring_wblock[pi].active) {   /* v0.4.258: drop any parked writer before teardown */
        seL4_CNode_Delete(seL4_CapInitThreadCNode, ring_wblock[pi].reply_cap, seL4_WordBits);
        ring_wblock[pi].active = 0;
    }
    if (!pipe_ring[pi].valid) { pipe_ring[pi].armed = 0; return; }
    for (int ci = 0; ci < pipe_ring[pi].copy_count; ci++) {
        seL4_CNode_Delete(seL4_CapInitThreadCNode, pipe_ring[pi].copies[ci], seL4_WordBits);
        vka_cspace_free(&vka, pipe_ring[pi].copies[ci]);
    }
    pipe_ring[pi].copy_count = 0;
    cspacepath_t rp;
    vka_cspace_make_path(&vka, pipe_ring[pi].frame.cptr, &rp);
    seL4_CNode_Revoke(rp.root, rp.capPtr, rp.capDepth);
    vspace_unmap_pages(&vspace, pipe_ring[pi].buf, 1, seL4_PageBits, NULL);
    vka_free_object(&vka, &pipe_ring[pi].frame);
    vka_audit_frame_release(1);
    pipe_ring[pi].buf = NULL;
    pipe_ring[pi].valid = 0;
    pipe_ring[pi].armed = 0;
}

/* Server-side producer: copy up to n bytes into the ring, advance tail. Used
 * only when the WRITER end fell back to legacy IPC (server is then the sole
 * producer for this pipe). Returns the bytes accepted (< n if the ring fills). */
static int ring_server_push(int pi, const char *src, int n) {
    shm_ring_hdr_t *h = ring_hdr(pi);
    if (!h || n < 0) return -1;
    uint32_t tail = h->tail;          /* sole producer -> plain load of our own index */
    uint32_t head = h->head;          /* peer index; a stale (older/smaller) head only
                                       * under-estimates space, which is safe */
    uint32_t space = shm_ring_space(tail, head);
    if ((uint32_t)n > space) n = (int)space;
    char *data = shm_ring_data(h);
    for (int i = 0; i < n; i++)
        data[(tail + (uint32_t)i) & SHM_RING_MASK] = src[i];
    shm_ring_publish();               /* release: data stores complete before tail advertises them */
    h->tail = tail + (uint32_t)n;
    if (n > 0) g_ring_push += (unsigned long)n;
    return n;
}

/* Server-side consumer: copy up to max bytes out of the ring into dst, advance
 * head. Used to serve a reader that fell back to / blocked via legacy IPC.
 * Returns the bytes copied. */
static int ring_server_pull(int pi, char *dst, int max) {
    shm_ring_hdr_t *h = ring_hdr(pi);
    if (!h || max < 0) return -1;
    uint32_t head = h->head;          /* sole consumer -> plain load of our own index */
    uint32_t tail = h->tail;
    shm_ring_observe();               /* acquire: the data we read is >= as new as this tail */
    uint32_t avail = shm_ring_used(tail, head);
    if ((uint32_t)max > avail) max = (int)avail;
    char *data = shm_ring_data(h);
    for (int i = 0; i < max; i++)
        dst[i] = data[(head + (uint32_t)i) & SHM_RING_MASK];
    shm_ring_observe();               /* load->store: reads complete before head frees the cells */
    h->head = head + (uint32_t)max;
    if (max > 0) g_ring_pull += (unsigned long)max;
    return max;
}

/* Mark the writer end gone (reader sees EOF after draining) / reader end gone
 * (writer sees EPIPE). Set with a fence so a peer spinning on the flag observes it. */
static void ring_set_writer_closed(int pi) {
    shm_ring_hdr_t *h = ring_hdr(pi);
    if (h) { h->writer_closed = 1; shm_ring_fence(); }
}
static void ring_set_reader_closed(int pi) {
    shm_ring_hdr_t *h = ring_hdr(pi);
    if (h) { h->reader_closed = 1; shm_ring_fence(); }
}
/* Clear the parked-reader flag when a blocked reader is served / woken / EINTR'd.
 * Fenced so a direct writer spinning on reader_waiting (the Dekker handshake in
 * shm_ring_send) promptly observes the clear and stops re-issuing PIPE_RING_WAKE. */
static void ring_clear_reader_waiting(int pi) {
    shm_ring_hdr_t *h = ring_hdr(pi);
    if (h) { h->reader_waiting = 0; shm_ring_fence(); }
}

/* v0.4.87: Wake a blocked reader whose PID matches, return EINTR (-4).
 * Called from PIPE_SIGNAL when a signal is delivered to a blocked process. */
static void wake_blocked_reader_signal(int target_pid) {
    for (int bi = 0; bi < MAX_PIPE_READ_BLOCKED; bi++) {
        if (!pipe_read_blocked[bi].active) continue;
        if (pipe_read_blocked[bi].caller_pid != target_pid) continue;
        seL4_SetMR(0, (seL4_Word)(-4));  /* -4 = EINTR sentinel */
        seL4_Send(pipe_read_blocked[bi].reply_cap,
                  seL4_MessageInfo_new(0, 0, 0, 1));
        seL4_CNode_Delete(seL4_CapInitThreadCNode,
                          pipe_read_blocked[bi].reply_cap, seL4_WordBits);
        pipe_read_blocked[bi].active = 0;
        ring_clear_reader_waiting(pipe_read_blocked[bi].pipe_id);  /* v0.4.258 */
        return;  /* one reader per PID */
    }
}

/* Wake all blocked readers on a pipe with EOF (0 bytes) */
static void wake_blocked_readers_eof(int pipe_id) {
    for (int bi = 0; bi < MAX_PIPE_READ_BLOCKED; bi++) {
        if (!pipe_read_blocked[bi].active) continue;
        if (pipe_read_blocked[bi].pipe_id != pipe_id) continue;
        seL4_SetMR(0, 0);
        seL4_Send(pipe_read_blocked[bi].reply_cap,
                  seL4_MessageInfo_new(0, 0, 0, 1));
        seL4_CNode_Delete(seL4_CapInitThreadCNode,
                          pipe_read_blocked[bi].reply_cap, seL4_WordBits);
        pipe_read_blocked[bi].active = 0;
        ring_clear_reader_waiting(pipe_id);  /* v0.4.258 */
    }
}

/* Unblock parent waiting for child exec completion */
static void check_exec_wait(int child_idx) {
    if (!exec_wait.active) return;
    if (exec_wait.child_idx != child_idx) return;
    seL4_SetMR(0, 0);
    seL4_Send(exec_wait.reply_cap, seL4_MessageInfo_new(0, 0, 0, 1));
    seL4_CNode_Delete(seL4_CapInitThreadCNode,
                      exec_wait.reply_cap, seL4_WordBits);
    exec_wait.active = 0;
}

/* Serve one blocked reader with available pipe data */
static void wake_one_blocked_reader(int pipe_id) {
    for (int bi = 0; bi < MAX_PIPE_READ_BLOCKED; bi++) {
        if (!pipe_read_blocked[bi].active) continue;
        if (pipe_read_blocked[bi].pipe_id != pipe_id) continue;
        pipe_t *p = &pipes[pipe_id];
        int max_len = pipe_read_blocked[bi].max_len;
        /* v0.4.258: ring mode -- serve the parked reader straight from the ring
         * (advancing head with the consumer barriers) instead of from shm_buf. */
        if (pipe_is_ring(pipe_id)) {
            int is_shm = pipe_read_blocked[bi].is_shm && p->xfer_valid;
            char tmp[900];
            char *dst;
            if (is_shm) {
                if (max_len > PAGE_SIZE) max_len = PAGE_SIZE;
                if (max_len > (int)SHM_RING_DATA) max_len = (int)SHM_RING_DATA;
                dst = p->xfer_buf;
            } else {
                if (max_len > 900) max_len = 900;
                dst = tmp;
            }
            int rlen = ring_server_pull(pipe_id, dst, max_len);
            if (rlen <= 0) {
                /* Nothing to serve. If the writer is gone, the reader must get EOF
                 * NOW -- no future publish will wake it (the close path already ran
                 * wake_blocked_readers_eof, but a WAKE can race ahead of it). If the
                 * writer is still live, leave the reader parked: reader_waiting stays
                 * set, so the next tail publish re-issues PIPE_RING_WAKE and we retry. */
                shm_ring_hdr_t *h = ring_hdr(pipe_id);
                if (h && h->writer_closed) {
                    seL4_SetMR(0, 0);
                    seL4_Send(pipe_read_blocked[bi].reply_cap,
                              seL4_MessageInfo_new(0, 0, 0, 1));
                    ring_clear_reader_waiting(pipe_id);
                    seL4_CNode_Delete(seL4_CapInitThreadCNode,
                                      pipe_read_blocked[bi].reply_cap, seL4_WordBits);
                    pipe_read_blocked[bi].active = 0;
                }
                break;
            }
            if (is_shm) {
                seL4_SetMR(0, (seL4_Word)rlen);
                seL4_Send(pipe_read_blocked[bi].reply_cap,
                          seL4_MessageInfo_new(0, 0, 0, 1));
            } else {
                seL4_SetMR(0, (seL4_Word)rlen);
                int mr = 1; seL4_Word w = 0;
                for (int i = 0; i < rlen; i++) {
                    w |= ((seL4_Word)(uint8_t)tmp[i]) << ((i % 8) * 8);
                    if (i % 8 == 7 || i == rlen - 1) { seL4_SetMR(mr++, w); w = 0; }
                }
                seL4_Send(pipe_read_blocked[bi].reply_cap,
                          seL4_MessageInfo_new(0, 0, 0, mr));
            }
            ring_clear_reader_waiting(pipe_id);
            seL4_CNode_Delete(seL4_CapInitThreadCNode,
                              pipe_read_blocked[bi].reply_cap, seL4_WordBits);
            pipe_read_blocked[bi].active = 0;
            break;
        }
        if (p->count == 0) break;
        int rlen = p->count < max_len ? p->count : max_len;
        /* v0.4.66: SHM wake -- copy to xfer page, reply with length only */
        if (pipe_read_blocked[bi].is_shm && p->xfer_valid) {
            if (rlen > PAGE_SIZE) rlen = PAGE_SIZE;
            for (int i = 0; i < rlen; i++)
                p->xfer_buf[i] = p->shm_buf[(p->head + i) % PIPE_BUF_SIZE];
            p->head = (p->head + rlen) % PIPE_BUF_SIZE;
            p->count -= rlen;
            seL4_SetMR(0, (seL4_Word)rlen);
            seL4_Send(pipe_read_blocked[bi].reply_cap,
                      seL4_MessageInfo_new(0, 0, 0, 1));
        } else {
            /* MR-based wake (original path) */
            if (rlen > 900) rlen = 900;
            seL4_SetMR(0, (seL4_Word)rlen);
            int mr = 1;
            seL4_Word w = 0;
            for (int i = 0; i < rlen; i++) {
                char c = p->shm_buf[(p->head + i) % PIPE_BUF_SIZE];
                w |= ((seL4_Word)(uint8_t)c) << ((i % 8) * 8);
                if (i % 8 == 7 || i == rlen - 1) {
                    seL4_SetMR(mr++, w); w = 0;
                }
            }
            p->head = (p->head + rlen) % PIPE_BUF_SIZE;
            p->count -= rlen;
            seL4_Send(pipe_read_blocked[bi].reply_cap,
                      seL4_MessageInfo_new(0, 0, 0, mr));
        }
        seL4_CNode_Delete(seL4_CapInitThreadCNode,
                          pipe_read_blocked[bi].reply_cap, seL4_WordBits);
        pipe_read_blocked[bi].active = 0;
        break;
    }
}

/* v0.4.258: park the current caller as a blocked reader on pipe pi (SaveCaller).
 * Returns the slot index (>=0; NO reply sent -- the saved cap is Send'd on wake)
 * or -1 if the blocked-reader table is full (caller should reply 0). Shared by the
 * ring-mode PIPE_READ / PIPE_READ_SHM branches. */
static int park_reader_save(int pi, int max_len, int is_shm, int badge) {
    int bi = -1;
    for (int b = 0; b < MAX_PIPE_READ_BLOCKED; b++)
        if (!pipe_read_blocked[b].active) { bi = b; break; }
    if (bi < 0) return -1;
    seL4_CNode_Delete(seL4_CapInitThreadCNode, read_reply_objs[bi].cptr, seL4_WordBits);
    seL4_CNode_SaveCaller(seL4_CapInitThreadCNode, read_reply_objs[bi].cptr, seL4_WordBits);
    pipe_read_blocked[bi].active = 1;
    pipe_read_blocked[bi].pipe_id = pi;
    pipe_read_blocked[bi].max_len = max_len;
    pipe_read_blocked[bi].is_shm = is_shm;
    pipe_read_blocked[bi].reply_cap = read_reply_objs[bi].cptr;
    int ci = badge - 1;
    pipe_read_blocked[bi].caller_pid =
        (ci >= 0 && ci < MAX_ACTIVE_PROCS && active_procs[ci].active)
            ? active_procs[ci].pid : -1;
    return bi;
}

/* v0.4.258: resume a writer parked on a full ring. Push its pending xfer_buf_w
 * (now that the reader has freed space), reply the accepted count, and wake the
 * reader if bytes landed. If `epipe` (the read end is gone) or the push still
 * can't place any byte with the reader gone, reply -1 (EPIPE). Called whenever the
 * reader advances head, and on teardown. */
static void wake_blocked_writer(int pi, int epipe) {
    if (pi < 0 || pi >= MAX_PIPES || !ring_wblock[pi].active) return;
    int written = 0;
    if (!epipe && pipe_is_ring(pi)) {
        const char *src = ring_wblock[pi].is_mr
            ? ring_wblock[pi].stash : pipes[pi].xfer_buf_w;
        written = ring_server_push(pi, src, ring_wblock[pi].wlen);
    }
    if (written == 0 && !epipe) return;   /* still full -> stay parked */
    seL4_SetMR(0, (seL4_Word)(written > 0 ? written : -1));
    seL4_Send(ring_wblock[pi].reply_cap, seL4_MessageInfo_new(0, 0, 0, 1));
    seL4_CNode_Delete(seL4_CapInitThreadCNode, ring_wblock[pi].reply_cap, seL4_WordBits);
    ring_wblock[pi].active = 0;
    if (written > 0) wake_one_blocked_reader(pi);
}

/* Park the current caller as the writer of ring pipe pi. For the MR path, `mr_src`
 * holds the wlen payload bytes (copied into the park stash); for the SHM path pass
 * NULL (the bytes stay in xfer_buf_w). Returns 0 on park (NO reply), -1 if no cap
 * slot (caller replies 0). */
static int park_writer_save(int pi, int wlen, const char *mr_src) {
    if (!ring_wblock_obj_valid[pi]) {
        cspacepath_t path;
        if (vka_cspace_alloc_path(&vka, &path) != 0) return -1;
        ring_wblock_obj[pi].cptr = path.capPtr;
        ring_wblock_obj_valid[pi] = 1;
    }
    seL4_CNode_Delete(seL4_CapInitThreadCNode, ring_wblock_obj[pi].cptr, seL4_WordBits);
    seL4_CNode_SaveCaller(seL4_CapInitThreadCNode, ring_wblock_obj[pi].cptr, seL4_WordBits);
    g_ring_park++;
    ring_wblock[pi].active = 1;
    ring_wblock[pi].reply_cap = ring_wblock_obj[pi].cptr;
    ring_wblock[pi].wlen = wlen;
    if (mr_src) {
        ring_wblock[pi].is_mr = 1;
        if (wlen > 900) wlen = 900;
        for (int i = 0; i < wlen; i++) ring_wblock[pi].stash[i] = mr_src[i];
        ring_wblock[pi].wlen = wlen;
    } else {
        ring_wblock[pi].is_mr = 0;
    }
    return 0;
}

/* Auto-free pipe when all refs dropped */
static void pipe_maybe_free(int pi) {
    if (pi < 0 || pi >= MAX_PIPES) return;
    pipe_t *p = &pipes[pi];
    if (p->read_refs <= 0 && p->write_refs <= 0) {
        if (p->xfer_valid) {
            /* v0.4.67: delete cap copies so untyped can be reclaimed */
            for (int ci = 0; ci < p->xfer_copy_count; ci++) {
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                    p->xfer_copies[ci], seL4_WordBits);
                vka_cspace_free(&vka, p->xfer_copies[ci]);
            }
            p->xfer_copy_count = 0;
            /* Revoke any remaining derived caps, then free */
            cspacepath_t xp;
            vka_cspace_make_path(&vka, p->xfer_frame.cptr, &xp);
            seL4_CNode_Revoke(xp.root, xp.capPtr, xp.capDepth);
            vspace_unmap_pages(&vspace, p->xfer_buf, 1, seL4_PageBits, NULL);
            vka_free_object(&vka, &p->xfer_frame);
            vka_audit_frame_release(1);  /* v0.4.138: keep live count accurate */
            p->xfer_buf = NULL;
            p->xfer_valid = 0;
        }
        if (p->xfer_valid_w) {     /* v0.4.257: free the write-direction xfer page (mirror) */
            for (int ci = 0; ci < p->xfer_copy_count_w; ci++) {
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                    p->xfer_copies_w[ci], seL4_WordBits);
                vka_cspace_free(&vka, p->xfer_copies_w[ci]);
            }
            p->xfer_copy_count_w = 0;
            cspacepath_t xpw;
            vka_cspace_make_path(&vka, p->xfer_frame_w.cptr, &xpw);
            seL4_CNode_Revoke(xpw.root, xpw.capPtr, xpw.capDepth);
            vspace_unmap_pages(&vspace, p->xfer_buf_w, 1, seL4_PageBits, NULL);
            vka_free_object(&vka, &p->xfer_frame_w);
            vka_audit_frame_release(1);
            p->xfer_buf_w = NULL;
            p->xfer_valid_w = 0;
        }
        if (p->shm_valid) {
            vspace_unmap_pages(&vspace, p->shm_buf, 1, seL4_PageBits, NULL);
            vka_free_object(&vka, &p->shm_frame);
            vka_audit_frame_release(1);  /* v0.4.138: keep live count accurate */
            p->shm_buf = p->buf;
            p->shm_valid = 0;
        }
        ring_free(pi);   /* v0.4.258: free the SHM-ring frame + child cap copies */
        p->active = 0;
    }
}

/* v0.4.166: wall-clock epoch offset (seconds). wall_time = uptime + offset.
 * 0 until set via PIPE_SET_TIME (the SNTP client). aios_wall_now() gives the
 * current wall-clock seconds for in-root-task callers (e.g. fs mtimes). */
long aios_wall_offset_sec = 0;
long aios_wall_now(void) {
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    return (long)(cnt / freq) + aios_wall_offset_sec;
}

/* v0.4.143: is there a live process registered as a WRITER of pipe pi (its
 * stdout is this pipe, recorded at PIPE_EXEC) that has not closed its write
 * end? The write end must report EOF to readers only when the actual writer
 * is gone -- NOT when the creating shell or the sibling reader closes its
 * inherited write-end copy. Those non-writer closes race ahead of a slow
 * (fs-reading) writer and would otherwise latch write_closed before the
 * writer ever writes, giving the reader a premature EOF. except_ci excludes
 * a process that is itself exiting/closing (pass -1 for none). */
static int pipe_live_writer_exists(int pi, int except_ci) {
    if (pi < 0 || pi >= MAX_PIPES) return 0;
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
        if (i == except_ci) continue;
        if (active_procs[i].active
            && active_procs[i].stdout_pipe_id == pi
            && !active_procs[i].pipe_write_closed)
            return 1;
    }
    return 0;
}

/* v0.4.139: reclaim orphaned (leaked) pipes when CREATE finds no free
 * slot. A pipe leaks when a child fork-fault leaves its refs unbalanced
 * so pipe_maybe_free never collects it; after MAX_PIPES leaks pipe()
 * fails ("Pipe call failed"). Only reclaim a pipe that was associated
 * with an exec'd child (pipe_had_child), has no active process
 * referencing it, and has no blocked reader -- so a freshly created pipe
 * still being set up is never touched. Returns the number freed. */
static int reclaim_orphaned_pipes(void) {
    int reclaimed = 0;
    for (int pi = 0; pi < MAX_PIPES; pi++) {
        if (!pipes[pi].active || !pipe_had_child[pi]) continue;
        int referenced = 0;
        for (int ci = 0; ci < MAX_ACTIVE_PROCS; ci++) {
            if (active_procs[ci].active &&
                (active_procs[ci].stdout_pipe_id == pi ||
                 active_procs[ci].stdin_pipe_id == pi)) {
                referenced = 1;
                break;
            }
        }
        if (referenced) continue;
        int blocked = 0;
        for (int bi = 0; bi < MAX_PIPE_READ_BLOCKED; bi++) {
            if (pipe_read_blocked[bi].active &&
                pipe_read_blocked[bi].pipe_id == pi) {
                blocked = 1;
                break;
            }
        }
        if (blocked) continue;
        /* Orphaned: children exec'd and reaped, nobody references it. */
        pipes[pi].read_refs = 0;
        pipes[pi].write_refs = 0;
        pipe_maybe_free(pi);
        if (!pipes[pi].active) reclaimed++;
    }
    return reclaimed;
}

/* v0.4.229 (netd Stage 0): cleanup-proxy SPSC ring (DESIGN_NETD s5).
 *
 * handle_child_fault (running on THIS pipe_server thread) used to stash one
 * exiting pid in a single int, and the main loop issued the blocking
 * seL4_Call(net_ep_cap, NET_CLEANUP_PID). Two problems: (a) one int drops a
 * second exit that arrives before the loop drains it; (b) the blocking Call sat
 * on the pipe_server's own thread, so a slow or wedged net server would freeze
 * ALL process/pipe management. Fix: a 64-entry single-producer/single-consumer
 * ring (the net_rx_ring idiom) drained by a sacrificial proxy thread. The
 * producer (this thread) only enqueues + Signals; the proxy makes the Call, so
 * a hung net server parks only the expendable proxy. Both ends are pinned to
 * core 0 (boot_services ROOT_CORE), so the dmb is belt-and-braces, kept for the
 * cross-core future and idiom match. */
#define NET_CLEANUP_RING_SZ MAX_ACTIVE_PROCS   /* 64; up to 63 pids in flight */
static volatile int      net_cleanup_ring[NET_CLEANUP_RING_SZ];
static volatile uint32_t net_cleanup_head;     /* producer: pipe_server thread */
static volatile uint32_t net_cleanup_tail;     /* consumer: proxy thread */
static seL4_CPtr         net_cleanup_ntfn;      /* proxy wakeup; set by boot
                                                 * before any child can fault */
static uint32_t          net_cleanup_lost;      /* ring-full drops (observability) */

void pipe_set_net_cleanup_ntfn(seL4_CPtr ntfn) { net_cleanup_ntfn = ntfn; }

static void net_cleanup_enqueue(int pid) {
    uint32_t head = net_cleanup_head;
    uint32_t next = (head + 1u) % NET_CLEANUP_RING_SZ;
    if (next == net_cleanup_tail) {
        /* Ring full -- drop and count. Far harder to hit than the old single
         * int, which dropped on the very next concurrent exit. */
        net_cleanup_lost++;
        return;
    }
    net_cleanup_ring[head] = pid;
    __asm__ volatile("dmb sy" ::: "memory");   /* publish entry before head */
    net_cleanup_head = next;
    if (net_cleanup_ntfn) seL4_Signal(net_cleanup_ntfn);
}

/* Sacrificial proxy thread: drain the ring, issue the blocking NET_CLEANUP_PID
 * Call. Drain-then-wait, so a Signal that races the drain just makes the next
 * Wait return immediately (no lost wakeup). Re-checks net_ep_cap per dequeue:
 * once net moves to netd, its fault listener zeroes net_ep_cap on a crash, and
 * the proxy then drops cleanups instead of Calling a dead endpoint. */
void net_cleanup_proxy_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr ntfn = (seL4_CPtr)(uintptr_t)arg0;
    (void)arg1; (void)ipc_buf;
    while (1) {
        while (net_cleanup_head != net_cleanup_tail) {
            __asm__ volatile("dmb sy" ::: "memory");   /* acquire: pair w/ enqueue */
            int pid = net_cleanup_ring[net_cleanup_tail];
            net_cleanup_tail = (net_cleanup_tail + 1u) % NET_CLEANUP_RING_SZ;
            if (net_ep_cap) {
                seL4_SetMR(0, (seL4_Word)pid);
                seL4_Call(net_ep_cap,
                          seL4_MessageInfo_new(NET_CLEANUP_PID, 0, 0, 1));
            }
        }
        seL4_Wait(ntfn, NULL);
    }
}

/* Handle child fault arriving on pipe_ep */

static void handle_child_fault(int child_idx) {
    active_proc_t *ch = &active_procs[child_idx];

    if (!ch->active || !ch->fault_on_pipe_ep) return;

    /* v0.4.84: suppress TCB destruction fault during exec */
    if (ch->ignore_next_fault) {
        ch->ignore_next_fault = 0;
        return;
    }

    /* Clear foreground tracking if this was the fg process.
     * v0.4.86: promote parent to fg so Ctrl-C can kill it
     * (e.g., sshd after its child shell is killed). */
    if (ch->pid == fg_pid) {
        int promoted = 0;
        if (ch->ppid > 0) {
            for (int pi = 0; pi < MAX_ACTIVE_PROCS; pi++) {
                if (active_procs[pi].active &&
                    active_procs[pi].pid == ch->ppid) {
                    fg_pid = ch->ppid;
                    promoted = 1;
                    break;
                }
            }
        }
        if (!promoted) fg_pid = -1;
        fg_fault_ep = 0;
        fg_sigint_sent = 0;  /* v0.4.85: reset two-stage Ctrl-C */
    }

    /* v0.4.86: defer net cleanup (cannot seL4_Call here -- would destroy the
     * reply cap needed for PIPE_SIGNAL reply). v0.4.229: enqueue to the
     * cleanup-proxy ring + Signal it (see NET_CLEANUP_RING above) instead of
     * stashing one pid for the main loop. */
    if (net_ep_cap && ch->pid > 0)
        net_cleanup_enqueue(ch->pid);

    /* Auto-close write end if child was a registered pipe writer. */
    if (ch->stdout_pipe_id >= 0 && ch->stdout_pipe_id < MAX_PIPES
        && pipes[ch->stdout_pipe_id].active) {
        int wpi = ch->stdout_pipe_id;
        /* accounting only -- skip if it already closed via PIPE_CLOSE_WRITE */
        if (!ch->pipe_write_closed) pipes[wpi].write_refs--;
        /* v0.4.143: the registered writer is EXITING -- the sole authoritative
         * EOF. Latch once no other live writer remains. Because this is the
         * only place write_closed is set, no close (the creating shell, the
         * sibling reader, or the writer's own redundant original-write-fd
         * close) can cause a premature EOF before the writer has produced its
         * data. write_refs is not consulted -- it is unreliable (forked
         * write-end inheritance and dup2 are not counted). */
        if (!pipes[wpi].write_closed
            && !pipe_live_writer_exists(wpi, child_idx)) {
            pipes[wpi].write_closed = 1;
            ring_set_writer_closed(wpi);   /* v0.4.258: EOF for a spinning ring reader */
            wake_blocked_readers_eof(wpi);
        }
        /* v0.4.258: a parked ring writer that was KILLED -- drop its stale park so a
         * later read does not push its dead buffer / Send to a dangling reply cap. */
        if (ring_wblock[wpi].active) {
            seL4_CNode_Delete(seL4_CapInitThreadCNode, ring_wblock[wpi].reply_cap, seL4_WordBits);
            ring_wblock[wpi].active = 0;
        }
        pipe_maybe_free(wpi);
    }
    /* Auto-close read end if child was reading from a pipe */
    if (ch->stdin_pipe_id >= 0 && ch->stdin_pipe_id < MAX_PIPES
        && pipes[ch->stdin_pipe_id].active
        && pipes[ch->stdin_pipe_id].read_refs > 0
        && !pipes[ch->stdin_pipe_id].read_closed) {
        pipes[ch->stdin_pipe_id].read_refs--;
        /* v0.4.85: only mark closed when last reader gone */
        if (pipes[ch->stdin_pipe_id].read_refs <= 0) {
            pipes[ch->stdin_pipe_id].read_closed = 1;
            ring_set_reader_closed(ch->stdin_pipe_id);  /* v0.4.258: EPIPE for a spinning ring writer */
            wake_blocked_writer(ch->stdin_pipe_id, 1);  /* v0.4.258: EPIPE a parked ring writer */
        }
        pipe_maybe_free(ch->stdin_pipe_id);
    }
    reap_forked_child(child_idx);
    check_exec_wait(child_idx);
}

/* Mint badged pipe_ep as fault endpoint for a child process */
static int mint_pipe_fault_ep(int child_idx, vka_object_t *out) {
    cspacepath_t src, dest;
    vka_cspace_make_path(&vka, pipe_ep_cap, &src);
    if (vka_cspace_alloc_path(&vka, &dest)) return -1;
    int err = seL4_CNode_Mint(dest.root, dest.capPtr, dest.capDepth,
        src.root, src.capPtr, src.capDepth,
        seL4_AllRights, (seL4_Word)(child_idx + 1));
    if (err) {
        vka_cspace_free(&vka, dest.capPtr);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->cptr = dest.capPtr;
    return 0;
}

/* v0.4.145: file-backed mmap (server-side fill / msync write-back). A child
 * page frame is mapped into the child vspace only, so to read or write its
 * bytes from the root we CNode_Copy the frame cap and map the copy into the
 * root vspace (a frame cap maps into one vspace at a time). This mirrors the
 * fork.c bounce-copy pattern. file_bounce_map returns ok=0 on any failure;
 * callers then skip that page (a missing fill page just stays zero). */
typedef struct { void *rptr; cspacepath_t dup; int ok; } file_bounce_t;

static file_bounce_t file_bounce_map(int ci, uintptr_t cva) {
    file_bounce_t b;
    b.rptr = NULL; b.ok = 0;
    seL4_CPtr ccap = vspace_get_cap(&active_procs[ci].proc.vspace, (void *)cva);
    if (ccap == seL4_CapNull) return b;
    cspacepath_t src;
    vka_cspace_make_path(&vka, ccap, &src);
    if (vka_cspace_alloc_path(&vka, &b.dup) != 0) return b;
    if (seL4_CNode_Copy(b.dup.root, b.dup.capPtr, b.dup.capDepth,
                        src.root, src.capPtr, src.capDepth, seL4_AllRights) != 0) {
        vka_cspace_free(&vka, b.dup.capPtr);
        return b;
    }
    b.rptr = vspace_map_pages(&vspace, &b.dup.capPtr, NULL,
                              seL4_AllRights, 1, seL4_PageBits, 1);
    if (!b.rptr) {
        seL4_CNode_Delete(b.dup.root, b.dup.capPtr, b.dup.capDepth);
        vka_cspace_free(&vka, b.dup.capPtr);
        return b;
    }
    b.ok = 1;
    return b;
}

static void file_bounce_unmap(file_bounce_t *b) {
    if (!b->ok) return;
    vspace_unmap_pages(&vspace, b->rptr, 1, seL4_PageBits, NULL);
    seL4_CNode_Delete(b->dup.root, b->dup.capPtr, b->dup.capDepth);
    vka_cspace_free(&vka, b->dup.capPtr);
    b->ok = 0;
}

/* v0.4.146: demand-paged file-backed mmap descriptors (architecture B). Kept
 * file-local here (NOT in active_proc_t) to avoid shifting the root task BSS
 * layout. handle_file_mmap_fault is called from both fault handlers (this file
 * and exec_server.c) -- a fault address inside a registered range allocates a
 * frame into the reservation and fills it from the file with vfs_pread (direct
 * MMIO block read, no nested IPC, so the fault reply cap is safe). */
#define MAX_FILE_VMAS 64   /* v0.4.181: 16->64 -- one demand-text VMA per proc + mmap slack */
typedef struct {
    int       active;
    int       ci;
    uintptr_t va_start;
    uintptr_t va_end;     /* exclusive */
    long      offset;
    void     *reservation;
    char      path[128];
} file_vma_t;
static file_vma_t file_vmas[MAX_FILE_VMAS];
/* v0.4.181: per-proc reservation storage for the demand-paged read-only text
 * segment (mirrors aios_bss_res). One RO code segment per proc is the norm. */
static sel4utils_res_t aios_text_res[MAX_ACTIVE_PROCS];

static file_vma_t *file_vma_find(int ci, uintptr_t va) {
    for (int i = 0; i < MAX_FILE_VMAS; i++) {
        file_vma_t *v = &file_vmas[i];
        if (v->active && v->ci == ci && va >= v->va_start && va < v->va_end)
            return v;
    }
    return NULL;
}

/* Clear all descriptors for a process slot on teardown/reuse (mirrors
 * cow_clear_proc). Does NOT free the reservation -- the vspace tear-down owns
 * it by then; PIPE_MUNMAP_FILE is the path that frees a live reservation. */
void clear_file_vmas(int ci) {
    for (int i = 0; i < MAX_FILE_VMAS; i++)
        if (file_vmas[i].active && file_vmas[i].ci == ci)
            file_vmas[i].active = 0;
}

/* Returns 1 if the fault was a file-mmap page (filled + ready to resume),
 * 0 if the address is not in any file-mmap range, -1 on a hard error. */
int handle_file_mmap_fault(int ci, seL4_Word fault_addr) {
    if (ci < 0 || ci >= MAX_ACTIVE_PROCS || !active_procs[ci].active) return 0;
    file_vma_t *v = file_vma_find(ci, (uintptr_t)fault_addr);
    if (!v) return 0;
    uintptr_t page_va = (uintptr_t)fault_addr & ~(uintptr_t)0xFFF;
    if (vspace_get_cap(&active_procs[ci].proc.vspace, (void *)page_va) != seL4_CapNull)
        return 0;  /* already mapped -- let the normal path handle it */
    extern int vka_audit_check_headroom(int needed_pages);
    if (vka_audit_check_headroom(1) < 0) return -1;
    reservation_t rsv = { .res = v->reservation };
    int merr = vspace_new_pages_at_vaddr(&active_procs[ci].proc.vspace,
                                         (void *)page_va, 1, seL4_PageBits, rsv);
    if (merr) return -1;
    vka_audit_frame(VKA_SUB_OTHER, 1);
    active_procs[ci].audit_pages_allocated++;
    long foff = v->offset + (long)(page_va - v->va_start);
    file_bounce_t fb = file_bounce_map(ci, page_va);
    if (fb.ok) {
        vfs_pread(v->path, (int)foff, (char *)fb.rptr, PAGE_SIZE);
        /* v0.4.182: A72 I-cache coherency for demand-paged CODE. The page was
         * just loaded via DATA writes (the fb.dup root mapping), so clean it to
         * PoU here and invalidate the child's I-cache below -- otherwise the
         * real A72 fetches stale instructions and the process crashes. Mirrors
         * sel4utils_elf_load, which unifies BOTH the loader and loadee frames.
         * Harmless for data mmap. QEMU's a53 model does not enforce this, which
         * is why v0.4.181 demand-text booted on QEMU but killed every
         * disk-exec'd proc (getty/netconsole/sshd) on real hardware. */
        seL4_ARM_Page_Unify_Instruction(fb.dup.capPtr, 0, PAGE_SIZE);
        file_bounce_unmap(&fb);
    }
    seL4_CPtr ccap = vspace_get_cap(&active_procs[ci].proc.vspace, (void *)page_va);
    if (ccap != seL4_CapNull)
        seL4_ARM_Page_Unify_Instruction(ccap, 0, PAGE_SIZE);
    return 1;
}

/* v0.4.183: SHARED read-only .text cache -- Phase 2 of the per-proc footprint
 * work (Phase 1 = v0.4.181/182 demand-text). Same-binary processes (a pipeline
 * storm of seq/wc/cat/dash) each kept their OWN copy of identical read-only
 * code. This root-side cache loads each binary's read-only (R+X) ELF segment
 * ONCE -- frames filled from the file + I-cache-unified once -- then maps those
 * SHARED frames read-only into every later process of the same binary: one
 * physical copy for N procs instead of N. Read-only code is the ideal thing to
 * share (immutable, no COW). This REPLACES demand-text for the RO segment (a
 * proc shares OR demand-pages, never both); demand-BSS + writable .data are
 * untouched. EXEC PATH ONLY -- do_fork eager-reloads, and the boot servers load
 * from the CPIO (spawn_util.c), so neither is affected.
 *
 * Memory model: a bump allocator (text_frames[]) backs every entry; master
 * frames live for the boot (reboot clears the cache). When the entry table or
 * the frame pool fills, exec falls back to demand-text -- purely an optimisation
 * path, NEVER a correctness gate. The masters are kept (the design's accepted
 * "just keep it" option), so refcount is observational only and a stale count
 * can never cause a use-after-free.
 *
 * Staleness: a binary updated by a live push-deploy would leave a stale entry.
 * The normal deploy flow (push + reboot) sidesteps this; the per-boot cache is
 * rebuilt every boot. A defensive vaddr/npages layout check below rejects a
 * gross mismatch (then that proc demand-pages instead). */
#define TEXT_CACHE_ENTRIES 24
#define TEXT_FRAME_POOL    1024   /* total cached text pages across all binaries (4 MB cap) */
typedef struct {
    int       used;
    char      path[128];
    uintptr_t vaddr;        /* page-aligned start of the RO text segment */
    long      offset;       /* file offset corresponding to vaddr */
    int       frame_base;   /* index into text_frames[] */
    int       npages;
    int       refcount;     /* observational: live procs mapping these frames */
} text_entry_t;
static text_entry_t text_cache[TEXT_CACHE_ENTRIES];
static vka_object_t text_frames[TEXT_FRAME_POOL];
static int          text_frames_used = 0;              /* bump allocator high-water */
static int          text_share_slot[MAX_ACTIVE_PROCS]; /* 0 = none, else cache idx+1 */

static int text_path_eq(const char *a, const char *b) {
    for (int p = 0; p < 128; p++) {
        if (a[p] != b[p]) return 0;
        if (!a[p]) return 1;
    }
    return 1;
}

static text_entry_t *text_cache_find(const char *path) {
    for (int i = 0; i < TEXT_CACHE_ENTRIES; i++)
        if (text_cache[i].used && text_path_eq(text_cache[i].path, path))
            return &text_cache[i];
    return NULL;
}

/* Build a new cache entry from the executable file: allocate npages frames, fill
 * each from the file (vfs_pread, exactly like handle_file_mmap_fault), and unify
 * the I-cache ONCE per master frame. The v0.4.182 lesson: code loaded via DATA
 * writes MUST be Unify_Instruction'd (it cleans D-cache to PoU), and QEMU (a53)
 * will NOT catch a missing one -- only the real A72 will. Because every sharer
 * maps these frames read-only at the SAME link vaddr, the fill-time unify is
 * sufficient: a later proc fetches from PoU (its I-cache for that vaddr is cold,
 * or hits the identical physical line) with NO per-proc unify. Returns the entry,
 * or NULL if the cache is full / alloc fails (caller falls back to demand-text). */
static text_entry_t *text_cache_fill(const char *path, uintptr_t vaddr,
                                     long offset, int npages) {
    if (npages <= 0 || npages > TEXT_FRAME_POOL) return NULL;
    int slot = -1;
    for (int i = 0; i < TEXT_CACHE_ENTRIES; i++)
        if (!text_cache[i].used) { slot = i; break; }
    if (slot < 0) return NULL;                              /* entry table full */
    if (text_frames_used + npages > TEXT_FRAME_POOL) return NULL;  /* frame pool full */
    if (vka_audit_check_headroom(npages) < 0) return NULL;
    int base = text_frames_used;
    int got = 0;
    for (; got < npages; got++) {
        vka_object_t *fr = &text_frames[base + got];
        if (vka_alloc_frame(&vka, seL4_PageBits, fr)) break;
        vka_audit_frame(VKA_SUB_OTHER, 1);
        /* Map the master frame into the root vspace, fill it from the file,
         * unify the I-cache, then unmap KEEPING the cap (NULL vka = do not free
         * the object). The cap owns the physical frame for the boot. */
        void *r = vspace_map_pages(&vspace, &fr->cptr, NULL,
                                   seL4_AllRights, 1, seL4_PageBits, 1);
        if (!r) { vka_free_object(&vka, fr); break; }
        long foff = offset + (long)got * PAGE_SIZE;
        vfs_pread(path, (int)foff, (char *)r, PAGE_SIZE);
        seL4_ARM_Page_Unify_Instruction(fr->cptr, 0, PAGE_SIZE);
        vspace_unmap_pages(&vspace, r, 1, seL4_PageBits, NULL);
    }
    if (got != npages) {                                   /* roll back partial fill */
        for (int j = 0; j < got; j++) vka_free_object(&vka, &text_frames[base + j]);
        return NULL;
    }
    text_frames_used = base + npages;
    text_entry_t *e = &text_cache[slot];
    e->used = 1;
    int p = 0;
    for (; path[p] && p < 127; p++) e->path[p] = path[p];
    e->path[p] = 0;
    e->vaddr = vaddr;
    e->offset = offset;
    e->frame_base = base;
    e->npages = npages;
    e->refcount = 0;
    AIOS_LOG_DEBUG_V("text cached pages=", (unsigned long)npages);
    return e;
}

/* Release ci's shared-text mapping + reference on teardown / slot reuse. MUST be
 * called while the child vspace is still ALIVE (before sel4utils_destroy_process),
 * exactly like cow_release_proc -- and at the same call sites.
 *
 * Why the explicit unmap is REQUIRED (not optional): the shared pages were mapped
 * with cookie=NULL (so the master frame is not co-owned by this vspace). At
 * tear-down, sel4utils free_page() no-ops every cookie==0 entry -- it does NOT
 * delete the cap or free the cslot. So WITHOUT this unmap each shared page leaks
 * a root cslot per proc, exhausting the cspace under storms ("Cannot fork").
 * vspace_unmap_pages(&vka) deletes the cap + frees the cslot, and because the
 * cookie is NULL it does NOT free the master frame (sel4utils_unmap_pages only
 * vka_utspace_free's when the cookie is set). This mirrors cow_release_proc,
 * which reclaims its cookie=NULL shared dups the same way.
 *
 * Idempotent (zeroes the share slot, so it cannot double-decrement / double-
 * unmap) and a safe no-op on a non-sharing slot or an already-dead vspace. */
void clear_shared_text(int ci) {
    if (ci < 0 || ci >= MAX_ACTIVE_PROCS) return;
    int s = text_share_slot[ci];
    if (!s) return;
    text_share_slot[ci] = 0;
    text_entry_t *e = &text_cache[s - 1];
    if (e->refcount > 0) e->refcount--;
    if (!active_procs[ci].active) return;   /* vspace already gone -> nothing to unmap */
    vspace_t *vs = &active_procs[ci].proc.vspace;
    for (int pg = 0; pg < e->npages; pg++) {
        void *va = (void *)(e->vaddr + (uintptr_t)pg * PAGE_SIZE);
        if (vspace_get_cap(vs, va) == seL4_CapNull) continue;  /* RESERVED/EMPTY */
        vspace_unmap_pages(vs, va, 1, seL4_PageBits, &vka);
    }
}

/* v0.4.183: map a binary's shared read-only .text into a freshly elf_load'd
 * child, REPLACING its private eager copy (and replacing setup_demand_text for
 * the RO segment). Returns the number of pages handled (>0) on success, or 0 if
 * sharing was skipped/failed WITH the child's eager pages still intact -- in
 * which case the caller falls back to setup_demand_text. EXEC PATH ONLY. */
int setup_shared_text(int ci, elf_t *elf, const char *path) {
    if (ci < 0 || ci >= MAX_ACTIVE_PROCS || !active_procs[ci].active || !path) return 0;
    sel4utils_process_t *proc = &active_procs[ci].proc;
    int nph = elf_getNumProgramHeaders(elf);
    for (int i = 0; i < nph; i++) {
        if (elf_getProgramHeaderType(elf, i) != 1 /* PT_LOAD */) continue;
        uint32_t fl = (uint32_t)elf_getProgramHeaderFlags(elf, i);
        if ((fl & 2) || !(fl & 1)) continue;        /* want R+X, NOT writable (PF_W=2,PF_X=1) */
        uintptr_t v  = (uintptr_t)elf_getProgramHeaderVaddr(elf, i);
        size_t    fz = (size_t)elf_getProgramHeaderFileSize(elf, i);
        long      fo = (long)elf_getProgramHeaderOffset(elf, i);
        if (fz < (size_t)(8 * PAGE_SIZE)) return 0; /* tiny text: not worth sharing */
        uintptr_t s = v & ~(uintptr_t)0xFFF;
        uintptr_t e = (v + fz + 0xFFF) & ~(uintptr_t)0xFFF;
        long off0 = fo - (long)(v - s);             /* file offset at page-aligned s */
        if (off0 < 0 || e <= s) return 0;
        int pages = (int)((e - s) / PAGE_SIZE);

        /* Look up or build the per-binary cache entry (no child mutation yet). */
        text_entry_t *ent = text_cache_find(path);
        if (ent && (ent->vaddr != s || ent->npages != pages)) return 0; /* stale layout */
        if (!ent) ent = text_cache_fill(path, s, off0, pages);
        if (!ent) return 0;   /* cache/pool full or alloc failed -> demand-text */

        /* Swap the child's private eager RO pages for the shared frames. Free
         * the eager frames (&vka) -- that reclaim is the whole point. */
        vspace_unmap_pages(&proc->vspace, (void *)s, pages, seL4_PageBits, &vka);
        sel4utils_res_t *tres = &aios_text_res[ci];
        if (sel4utils_reserve_range_at_no_alloc(&proc->vspace, tres,
                (void *)s, (size_t)pages * PAGE_SIZE, seL4_AllRights, 1)) {
            /* no_alloc reserve is deterministic bookkeeping and essentially
             * never fails; if it does, the eager pages are gone -- return 0 so
             * the caller's demand-text re-reserves the (now unmapped) range. */
            AIOS_LOG_WARN("shared-text: reserve failed");
            return 0;
        }
        reservation_t rsv = { .res = tres };
        /* read-only cap (executable via Default_VMAttributes, which excludes
         * ExecuteNever); a writable mapping of shared code would let one proc
         * corrupt all. cookie=NULL so vspace_tear_down deletes the child cap but
         * does NOT free the shared master frame (the cow_setup pattern). */
        seL4_CapRights_t ro = seL4_CapRights_new(0, 0, 1, 0);
        int mapped = 0, failed = 0;
        for (int pg = 0; pg < pages; pg++) {
            void *va = (void *)(s + (uintptr_t)pg * PAGE_SIZE);
            seL4_CPtr master = text_frames[ent->frame_base + pg].cptr;
            cspacepath_t src, dup;
            vka_cspace_make_path(&vka, master, &src);
            if (vka_cspace_alloc_path(&vka, &dup)) { failed++; continue; }
            vka_audit_cslot(VKA_SUB_OTHER);
            if (seL4_CNode_Copy(dup.root, dup.capPtr, dup.capDepth,
                                src.root, src.capPtr, src.capDepth, ro)) {
                vka_cspace_free(&vka, dup.capPtr); failed++; continue;
            }
            if (vspace_map_pages_at_vaddr(&proc->vspace, &dup.capPtr, NULL,
                    va, 1, seL4_PageBits, rsv)) {
                seL4_CNode_Delete(dup.root, dup.capPtr, dup.capDepth);
                vka_cspace_free(&vka, dup.capPtr); failed++; continue;
            }
            mapped++;
        }
        if (failed) {
            /* Demand-load any holes: register a file_vma over the segment so the
             * unmapped pages fault into handle_file_mmap_fault (the shared pages
             * are present and never fault). Belt-and-suspenders -- near
             * impossible once the entry is filled (the loop above is pure cap
             * ops). The whole range is already reserved into tres. */
            int vslot = -1;
            for (int k = 0; k < MAX_FILE_VMAS; k++)
                if (!file_vmas[k].active) { vslot = k; break; }
            if (vslot >= 0) {
                file_vmas[vslot].active = 1;
                file_vmas[vslot].ci = ci;
                file_vmas[vslot].va_start = s;
                file_vmas[vslot].va_end = e;
                file_vmas[vslot].offset = off0;
                file_vmas[vslot].reservation = tres;
                int q = 0;
                for (; path[q] && q < 127; q++) file_vmas[vslot].path[q] = path[q];
                file_vmas[vslot].path[q] = 0;
            }
            AIOS_LOG_WARN_V("shared-text: holes demand-paged=", (unsigned long)failed);
        }
        ent->refcount++;
        text_share_slot[ci] = (int)(ent - text_cache) + 1;
        AIOS_LOG_DEBUG_V("text shared pages=", (unsigned long)mapped);
        return pages;   /* whole RO segment now handled (shared + any holes) */
    }
    return 0;
}

/* v0.4.181: demand-page the read-only executable segment(s) of a freshly
 * elf_load'd process from the executable file, mirroring demand-BSS. Unmaps the
 * eagerly-loaded code pages and registers a file_vma so the first instruction
 * fetch into each page faults into handle_file_mmap_fault, which reads the page
 * from the executable and maps it (executable via Default_VMAttributes, which
 * excludes ExecuteNever). Cuts the resident footprint from the whole binary to
 * the pages actually executed. EXEC PATH ONLY -- do_fork eager-reloads, so a
 * forked child's transient code stays mapped (no fork-lazy hazard); and the
 * boot servers load from the CPIO (spawn_util.c), not here, so they are
 * unaffected. Returns the number of pages made lazy (0 if none / skipped). */
int setup_demand_text(int ci, elf_t *elf, const char *path) {
    if (ci < 0 || ci >= MAX_ACTIVE_PROCS || !active_procs[ci].active || !path) return 0;
    sel4utils_process_t *proc = &active_procs[ci].proc;
    int nph = elf_getNumProgramHeaders(elf);
    for (int i = 0; i < nph; i++) {
        if (elf_getProgramHeaderType(elf, i) != 1 /* PT_LOAD */) continue;
        uint32_t fl = (uint32_t)elf_getProgramHeaderFlags(elf, i);
        if ((fl & 2) || !(fl & 1)) continue;        /* want R+X, NOT writable (PF_W=2,PF_X=1) */
        uintptr_t v  = (uintptr_t)elf_getProgramHeaderVaddr(elf, i);
        size_t    fz = (size_t)elf_getProgramHeaderFileSize(elf, i);
        long      fo = (long)elf_getProgramHeaderOffset(elf, i);
        if (fz < (size_t)(8 * PAGE_SIZE)) continue; /* not worth the faults for tiny text */
        uintptr_t s = v & ~(uintptr_t)0xFFF;
        uintptr_t e = (v + fz + 0xFFF) & ~(uintptr_t)0xFFF;
        long off0 = fo - (long)(v - s);             /* file offset at page-aligned s */
        if (off0 < 0 || e <= s) continue;
        int slot = -1;
        for (int k = 0; k < MAX_FILE_VMAS; k++)
            if (!file_vmas[k].active) { slot = k; break; }
        if (slot < 0) { AIOS_LOG_WARN("demand-text: no free vma slot"); return 0; }
        size_t pages = (e - s) / PAGE_SIZE;
        vspace_unmap_pages(&proc->vspace, (void *)s, pages, seL4_PageBits, &vka);
        sel4utils_res_t *tres = &aios_text_res[ci];
        if (sel4utils_reserve_range_at_no_alloc(&proc->vspace, tres,
                (void *)s, pages * PAGE_SIZE, seL4_AllRights, 1)) {
            AIOS_LOG_WARN("demand-text: reserve failed");
            return 0;
        }
        file_vmas[slot].active = 1;
        file_vmas[slot].ci = ci;
        file_vmas[slot].va_start = s;
        file_vmas[slot].va_end = e;
        file_vmas[slot].offset = off0;
        file_vmas[slot].reservation = tres;
        int p = 0;
        for (; path[p] && p < 127; p++) file_vmas[slot].path[p] = path[p];
        file_vmas[slot].path[p] = 0;
        AIOS_LOG_DEBUG_V("text lazy pages=", (unsigned long)pages);
        return (int)pages;   /* one RO code segment is the norm */
    }
    return 0;
}

void pipe_server_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    (void)arg1; (void)ipc_buf;

    /* Init pipes */
    for (int i = 0; i < MAX_PIPES; i++) pipes[i].active = 0;
    for (int i = 0; i < MAX_PIPES; i++) {     /* v0.4.258 SHM-ring state */
        pipe_ring[i].armed = 0;
        pipe_ring[i].valid = 0;
        pipe_ring[i].copy_count = 0;
        ring_wblock[i].active = 0;
        ring_wblock_obj_valid[i] = 0;
    }
    wait_init();

    /* Init blocked reader table */
    for (int i = 0; i < MAX_PIPE_READ_BLOCKED; i++) {
        pipe_read_blocked[i].active = 0;
        cspacepath_t path;
        vka_cspace_alloc_path(&vka, &path);
        read_reply_objs[i].cptr = path.capPtr;
    }

    /* Init exec-wait barrier */
    exec_wait.active = 0;
    {
        cspacepath_t path;
        vka_cspace_alloc_path(&vka, &path);
        exec_wait_reply_obj.cptr = path.capPtr;
    }
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++) exec_done[i] = 0;

    while (1) {
        /* v0.4.229: net-socket cleanup on child exit now runs on the
         * net_cleanup_proxy thread (enqueued by handle_child_fault), not here --
         * a wedged net server must never freeze this loop. */

        seL4_Word badge;
        /* diag v0.4.201: report the previous message if its handler ran long */
        if (diag_prev_label != (seL4_Word)~0ul) {
            uint64_t dms = pipe_diag_ms(diag_prev_t0);
            if (dms > 250)
                printf("[pipe] SLOW msg label=%lu badge=%lu took %lums\n",
                       (unsigned long)diag_prev_label,
                       (unsigned long)diag_prev_badge, (unsigned long)dms);
            diag_prev_label = (seL4_Word)~0ul;
        }
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);
        diag_prev_t0 = mono_ticks();
        diag_prev_label = label;
        diag_prev_badge = badge;

        /* ---- Fault detection: labels below PIPE_CREATE are seL4 faults ---- */
        if (label < PIPE_CREATE && badge > 0 && badge <= MAX_ACTIVE_PROCS) {
            int ci = (int)badge - 1;
            if (ci >= 0 && ci < MAX_ACTIVE_PROCS
                && active_procs[ci].active
                && active_procs[ci].fault_on_pipe_ep) {
                /* v0.4.106: BSS fault? allocate + map + reply (resumes child) */
                if (label == seL4_Fault_VMFault
                    && active_procs[ci].bss_reservation != NULL) {
                    seL4_Word fault_addr = seL4_GetMR(seL4_VMFault_Addr);
                    uintptr_t page_va = fault_addr & ~(uintptr_t)0xFFF;
                    /* v0.4.142: a pre-exec fork can leave a few COW-shared
                     * pages inside the lazy-BSS range. Those already have a
                     * frame cap and are COW-managed, so the BSS map below
                     * would fail its reservation check (logging noise) before
                     * the COW handler promotes them. Skip the BSS path for an
                     * in-range page that is already mapped or COW-managed;
                     * genuine demand-BSS pages are unmapped (get_cap==0) and
                     * not COW, so they still take it. */
                    if (fault_addr >= active_procs[ci].bss_lazy_start
                        && fault_addr <  active_procs[ci].bss_lazy_end
                        && !cow_in_range(ci, page_va)
                        && sel4utils_get_cap(&active_procs[ci].proc.vspace,
                                             (void *)page_va) == 0) {
                        if (vka_audit_check_headroom(1) >= 0) {
                            reservation_t res = { .res = active_procs[ci].bss_reservation };
                            int merr = vspace_new_pages_at_vaddr(
                                &active_procs[ci].proc.vspace,
                                (void *)page_va, 1, seL4_PageBits, res);
                            if (!merr) {
                                vka_audit_frame(VKA_SUB_OTHER, 1);
                                active_procs[ci].audit_pages_allocated++;
                                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                                continue;
                            }
                            AIOS_LOG_ERROR_V("BSS fault: map failed err=",
                                             (unsigned long)merr);
                        } else {
                            AIOS_LOG_ERROR("BSS fault: out of memory");
                        }
                        /* Fall through to handle_child_fault on failure */
                    }
                }
                /* v0.4.146: file-backed mmap demand-page fault */
                if (label == seL4_Fault_VMFault) {
                    seL4_Word ffa = seL4_GetMR(seL4_VMFault_Addr);
                    if (handle_file_mmap_fault(ci, ffa) > 0) {
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                        continue;
                    }
                }
                /* v0.4.110: COW write fault? promote the page in place. */
                if (label == seL4_Fault_VMFault) {
                    seL4_Word fault_addr = seL4_GetMR(seL4_VMFault_Addr);
                    int rc = cow_handle_write_fault(ci, fault_addr);
                    if (rc > 0) {
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                        continue;
                    }
                    /* rc == 0: not in any COW range; fall through.
                     * rc < 0: hard error (logged); fall through to exit. */
                }
                handle_child_fault(ci);
                continue;  /* No reply for faults */
            }
        }

        switch (label) {
        case PIPE_CREATE: {
            int pi = -1;
            for (int i = 0; i < MAX_PIPES; i++) {
                if (!pipes[i].active) { pi = i; break; }
            }
            if (pi < 0) {
                /* v0.4.139: slots exhausted -- reclaim leaked pipes (a
                 * child fork-fault can leave stuck refs that
                 * pipe_maybe_free never collects) and retry once. */
                if (reclaim_orphaned_pipes() > 0) {
                    for (int i = 0; i < MAX_PIPES; i++) {
                        if (!pipes[i].active) { pi = i; break; }
                    }
                }
            }
            if (pi < 0) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            pipes[pi].active = 1;
            pipe_had_child[pi] = 0;  /* v0.4.139: not yet child-associated */
            pipes[pi].head = 0;
            pipes[pi].count = 0;
            pipes[pi].read_closed = 0;
            pipes[pi].write_closed = 0;
            pipes[pi].read_refs = 1;
            pipes[pi].write_refs = 1;
            /* v0.4.65: allocate shared frame for pipe buffer */
            pipes[pi].shm_valid = 0;
            pipes[pi].shm_buf = pipes[pi].buf;
            vka_audit_frame(VKA_SUB_PIPE, 1);
            if (vka_alloc_frame(&vka, seL4_PageBits, &pipes[pi].shm_frame) == 0) {
                void *mapped = vspace_map_pages(&vspace,
                    &pipes[pi].shm_frame.cptr, NULL,
                    seL4_AllRights, 1, seL4_PageBits, 1);
                if (mapped) {
                    pipes[pi].shm_buf = (char *)mapped;
                    pipes[pi].shm_valid = 1;
                } else {
                    vka_free_object(&vka, &pipes[pi].shm_frame);
                }
            }
            /* v0.4.66: xfer page allocated lazily on first PIPE_MAP_SHM */
            pipes[pi].xfer_valid = 0;
            pipes[pi].xfer_buf = NULL;
            pipes[pi].xfer_copy_count = 0;
            pipes[pi].xfer_valid_w = 0;
            pipes[pi].xfer_buf_w = NULL;
            pipes[pi].xfer_copy_count_w = 0;
            /* v0.4.258: latch SHM-ring mode for this pipe from the current flag.
             * Allocate the ring frame NOW (before any data op) so the ring is the
             * pipe's one buffer from the start -- no data ever lands in shm_buf. A
             * later /proc/shmring toggle affects only pipes created afterwards. */
            pipe_ring[pi].armed = 0;
            pipe_ring[pi].valid = 0;
            pipe_ring[pi].copy_count = 0;
            if (g_shm_ring && ring_alloc(pi) == 0)
                pipe_ring[pi].armed = 1;
            seL4_SetMR(0, (seL4_Word)pi);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_WRITE: {
            int pi = (int)seL4_GetMR(0);
            int wlen = (int)seL4_GetMR(1);
            if (pi < 0 || pi >= MAX_PIPES || !pipes[pi].active) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            pipe_t *p = &pipes[pi];
            /* v0.4.258: ring-mode fallback (the writer didn't map the ring) --
             * unpack the MR bytes into a temp and push them through the ring. */
            if (pipe_is_ring(pi)) {
                char tmp[920];
                if (wlen > 900) wlen = 900;
                if (wlen < 0) wlen = 0;
                int mr = 2;
                for (int i = 0; i < wlen; i++) {
                    if (i % 8 == 0 && i > 0) mr++;
                    tmp[i] = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
                }
                int written = ring_server_push(pi, tmp, wlen);
                if (written > 0) {
                    seL4_SetMR(0, (seL4_Word)written);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    wake_one_blocked_reader(pi);
                    break;
                }
                /* full: EPIPE if the reader is gone (avoid an unwakeable park), else
                 * stash + block. read_closed is authoritative once the reader has
                 * exec'd (PIPE_EXEC clears any premature latch). */
                if (pipes[pi].read_closed) {
                    seL4_SetMR(0, (seL4_Word)-1);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                if (park_writer_save(pi, wlen, tmp) != 0) {
                    seL4_SetMR(0, 0);   /* no cap slot -> client retries (rare) */
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                }
                break;   /* parked: no reply now */
            }
            int mr = 2;
            int written = 0;
            for (int i = 0; i < wlen && p->count < PIPE_BUF_SIZE; i++) {
                if (i % 8 == 0 && i > 0) mr++;
                char c = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
                p->shm_buf[(p->head + p->count) % PIPE_BUF_SIZE] = c;
                p->count++;
                written++;
            }
            seL4_SetMR(0, (seL4_Word)written);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            /* Wake any blocked reader now that data is available */
            if (written > 0) wake_one_blocked_reader(pi);
            break;
        }
        case PIPE_READ: {
            int pi = (int)seL4_GetMR(0);
            int max_len = (int)seL4_GetMR(1);
            if (pi >= 0 && pi < MAX_PIPES && pipes[pi].active)
            if (pi < 0 || pi >= MAX_PIPES || !pipes[pi].active) {
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            pipe_t *p = &pipes[pi];

            /* v0.4.258: ring mode -- serve from the ring; block via the same
             * SaveCaller machinery (the direct reader falls through to here when it
             * finds the ring empty, and a fallback reader uses it for every read). */
            if (pipe_is_ring(pi)) {
                int nb = (int)seL4_GetMR(2);
                char tmp[900];
                int want = max_len; if (want > 900) want = 900; if (want < 0) want = 0;
                if (want == 0) {   /* read(fd,buf,0) returns 0 -- never park on a 0-len read */
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                int got = ring_server_pull(pi, tmp, want);
                if (got <= 0) {
                    shm_ring_hdr_t *h = ring_hdr(pi);
                    if (h->writer_closed) {                 /* EOF after drain */
                        seL4_SetMR(0, 0);
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                        break;
                    }
                    if (nb & 1) {                           /* O_NONBLOCK */
                        seL4_SetMR(0, (seL4_Word)(-1));
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                        break;
                    }
                    /* Lost-wakeup guard (Dekker): publish reader_waiting, full
                     * fence, THEN re-pull. A writer that advanced tail and read
                     * reader_waiting==0 must, by the matching fence on its side,
                     * have made that tail visible to this re-pull -> we serve and
                     * never park stale. */
                    h->reader_waiting = 1;
                    shm_ring_fence();
                    got = ring_server_pull(pi, tmp, want);
                    if (got <= 0) {
                        if (h->writer_closed) {
                            h->reader_waiting = 0;
                            seL4_SetMR(0, 0);
                            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                            break;
                        }
                        if (park_reader_save(pi, max_len, 0, (int)badge) < 0) {
                            h->reader_waiting = 0;
                            seL4_SetMR(0, 0);
                            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                            break;
                        }
                        break;   /* parked; reader_waiting stays set for the writer */
                    }
                    h->reader_waiting = 0;   /* re-pull found data -- serve below */
                }
                seL4_SetMR(0, (seL4_Word)got);
                int mr = 1; seL4_Word w = 0;
                for (int i = 0; i < got; i++) {
                    w |= ((seL4_Word)(uint8_t)tmp[i]) << ((i % 8) * 8);
                    if (i % 8 == 7 || i == got - 1) { seL4_SetMR(mr++, w); w = 0; }
                }
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, mr));
                wake_blocked_writer(pi, 0);   /* freed space -> let a parked writer refill */
                break;
            }

            /* Pipe empty -- check if writer is done */
            if (p->count == 0) {
                if (p->write_closed) {
                    /* EOF: writer closed, no more data */
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                /* v0.4.79: O_NONBLOCK -- return -1 instead of blocking */
                {
                    int nb_flags = (int)seL4_GetMR(2);
                    if (nb_flags & 1) {
                        seL4_SetMR(0, (seL4_Word)(-1));
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                        break;
                    }
                }
                /* Writer alive -- block reader via SaveCaller */
                int bi = -1;
                for (int b = 0; b < MAX_PIPE_READ_BLOCKED; b++) {
                    if (!pipe_read_blocked[b].active) { bi = b; break; }
                }
                if (bi < 0) {
                    /* No slots -- return 0 (caller retries) */
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                    read_reply_objs[bi].cptr, seL4_WordBits);
                seL4_CNode_SaveCaller(seL4_CapInitThreadCNode,
                    read_reply_objs[bi].cptr, seL4_WordBits);
                pipe_read_blocked[bi].active = 1;
                pipe_read_blocked[bi].pipe_id = pi;
                pipe_read_blocked[bi].max_len = max_len;
                pipe_read_blocked[bi].is_shm = 0;
                pipe_read_blocked[bi].reply_cap = read_reply_objs[bi].cptr;
                /* Do NOT reply -- reader blocks until data or EOF */
                break;
            }

            /* Data available -- serve immediately */
            int rlen = p->count < max_len ? p->count : max_len;
            if (rlen > 900) rlen = 900;
            seL4_SetMR(0, (seL4_Word)rlen);
            int mr = 1;
            seL4_Word w = 0;
            for (int i = 0; i < rlen; i++) {
                char c = p->shm_buf[(p->head + i) % PIPE_BUF_SIZE];
                w |= ((seL4_Word)(uint8_t)c) << ((i % 8) * 8);
                if (i % 8 == 7 || i == rlen - 1) {
                    seL4_SetMR(mr++, w); w = 0;
                }
            }
            p->head = (p->head + rlen) % PIPE_BUF_SIZE;
            p->count -= rlen;
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, mr));
            break;
        }
        case PIPE_CLOSE: {
            int pi = (int)seL4_GetMR(0);
            if (pi >= 0 && pi < MAX_PIPES && pipes[pi].active) {
                pipes[pi].read_refs = 0;
                pipes[pi].write_refs = 0;
                /* v0.4.258: mark both ends closed so any spinning ring peer bails
                 * (EOF for a reader / EPIPE for a writer) BEFORE the frame is freed. */
                ring_set_writer_closed(pi);
                ring_set_reader_closed(pi);
                wake_blocked_writer(pi, 1);   /* v0.4.258: EPIPE a parked writer */
                pipes[pi].active = 0;
                /* Wake any blocked readers with EOF */
                wake_blocked_readers_eof(pi);
                /* shm frame freed via pipe_maybe_free refcount path */
                pipe_maybe_free(pi);
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_CLOSE_WRITE: {
            int pi = (int)seL4_GetMR(0);
            /* v0.4.87: mark that this process already closed its write ref
             * so handle_child_fault skips the redundant decrement */
            {
                int ci = (int)badge - 1;
                if (ci >= 0 && ci < MAX_ACTIVE_PROCS && active_procs[ci].active)
                    active_procs[ci].pipe_write_closed = 1;
            }
            if (pi >= 0 && pi < MAX_PIPES && pipes[pi].active) {
                pipes[pi].write_refs--;
                /* v0.4.143: do not latch EOF while a live REGISTERED writer
                 * remains -- this stops a post-registration non-writer close
                 * (the creating shell or the sibling reader) from giving the
                 * reader a premature EOF while the writer is still producing.
                 * (Builtins write unregistered, so the write_refs<=0 fallback
                 * still latches them -- see PIPE_SET_PIPES note. A residual
                 * pre-registration window remains; see the v0.4.143 caveat.) */
                /* v0.4.143: a close NEVER latches EOF -- not the creating
                 * shell relinquishing the write end it set up, not the writer's
                 * own redundant original-write-fd close (it still holds fd 1),
                 * not the sibling reader closing its inherited copy. ALL of
                 * those race the writer's data/registration and caused the old
                 * premature-EOF flake (cat|wc=0, ls|wc=1). The registered
                 * writer's EXIT (handle_child_fault) is the sole authoritative
                 * EOF -- and every writer, builtins included, is registered
                 * server-side (PIPE_EXEC or dup2/PIPE_SET_PIPES) and stays so
                 * until exit. */
                pipe_maybe_free(pi);
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_CLOSE_READ: {
            int pi = (int)seL4_GetMR(0);
            if (pi >= 0 && pi < MAX_PIPES && pipes[pi].active) {
                pipes[pi].read_refs--;
                /* v0.4.85: only mark closed when last reader gone */
                if (pipes[pi].read_refs <= 0) {
                    pipes[pi].read_closed = 1;
                    ring_set_reader_closed(pi);   /* v0.4.258 */
                    wake_blocked_writer(pi, 1);   /* v0.4.258: EPIPE a parked writer */
                }
                pipe_maybe_free(pi);
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_KILL: {
            /* PIPE_KILL: reap path for proper zombie creation.
             * find active_proc index, then handle_child_fault
             * which does pipe cleanup + reap_forked_child. */
            int pid = (int)seL4_GetMR(0);
            int ki = -1;
            for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
                if (active_procs[i].active && active_procs[i].pid == pid) {
                    ki = i; break;
                }
            }
            if (ki < 0) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            active_procs[ki].exit_status = 9; /* SIGKILL */
            if (active_procs[ki].fault_on_pipe_ep) {
                handle_child_fault(ki);
            } else {
                /* Non-forked process: use direct destroy + reap */
                reap_forked_child(ki);
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_GETPID: {
            int caller_idx = (int)badge - 1;
            int pid = -1;
            if (caller_idx >= 0 && caller_idx < MAX_ACTIVE_PROCS
                && active_procs[caller_idx].active) {
                pid = active_procs[caller_idx].pid;
            }
            seL4_SetMR(0, (seL4_Word)pid);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_EXIT: {
            /* Child sends exit code before dying via NULL deref.
             * After reply, child faults. The fault arrives on pipe_ep
             * as a normal IPC message (label < PIPE_CREATE) on a
             * future iteration. No spinning needed. */
            int caller_idx = (int)badge - 1;
            int exit_code = (int)seL4_GetMR(0);
            if (caller_idx >= 0 && caller_idx < MAX_ACTIVE_PROCS
                && active_procs[caller_idx].active) {
                active_procs[caller_idx].exit_status = exit_code;
                active_procs[caller_idx].exit_cb_ran = 1;  /* v0.4.87: pipes already closed */
                                /* v0.4.87: do NOT clear fg_pid here. handle_child_fault
                 * runs after the VM fault and promotes fg_pid to parent.
                 * Clearing here prevents promotion, leaving fg_pid=-1
                 * and making processes unkillable via Ctrl-C. */
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }
        case PIPE_WAIT: {
            int caller_idx = (int)badge - 1;
            int target_pid = (int)seL4_GetMR(0);
            /* kill(0, sig): resolve to fg_pid */
            if (target_pid == 0 && fg_pid > 0)
                target_pid = fg_pid;

            if (caller_idx < 0 || caller_idx >= MAX_ACTIVE_PROCS
                || !active_procs[caller_idx].active) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int caller_pid = active_procs[caller_idx].pid;

            /* Check if child already exited */
            int already_exited = 1;
            if (target_pid > 0) {
                for (int ci = 0; ci < MAX_ACTIVE_PROCS; ci++) {
                    if (active_procs[ci].active
                        && active_procs[ci].pid == target_pid
                        && active_procs[ci].ppid == caller_pid) {
                        already_exited = 0;
                        break;
                    }
                }
            } else {
                for (int ci = 0; ci < MAX_ACTIVE_PROCS; ci++) {
                    if (active_procs[ci].active
                        && active_procs[ci].ppid == caller_pid) {
                        already_exited = 0;
                        break;
                    }
                }
            }

            if (already_exited) {
                int zstatus = 0;
                int zpid = target_pid;
                for (int z = 0; z < MAX_ZOMBIES; z++) {
                    if (!zombies[z].active) continue;
                    if ((target_pid > 0 && zombies[z].pid == target_pid) ||
                        (target_pid == -1 && zombies[z].ppid == caller_pid)) {
                        zpid = zombies[z].pid;
                        zstatus = zombies[z].exit_status;
                        zombies[z].active = 0;
                        break;
                    }
                }
                seL4_SetMR(0, (seL4_Word)zpid);
                seL4_SetMR(1, (seL4_Word)zstatus);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
                break;
            }

            /* Child still alive -- defer reply via SaveCaller */
            int wi = -1;
            for (int w = 0; w < MAX_WAIT_PENDING; w++) {
                if (!wait_pending[w].active) { wi = w; break; }
            }
            if (wi < 0) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            seL4_CNode_Delete(seL4_CapInitThreadCNode,
                              wait_reply_objects[wi].cptr, seL4_WordBits);
            seL4_CNode_SaveCaller(seL4_CapInitThreadCNode,
                                   wait_reply_objects[wi].cptr, seL4_WordBits);
            wait_pending[wi].active = 1;
            wait_pending[wi].waiting_pid = caller_pid;
            wait_pending[wi].child_pid = target_pid;
            wait_pending[wi].reply_cap = wait_reply_objects[wi].cptr;
            break;
        }
        case PIPE_FORK: {
            int parent_idx = (int)badge - 1;
            if (parent_idx < 0 || parent_idx >= MAX_ACTIVE_PROCS
                || !active_procs[parent_idx].active) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int child_pid = do_fork(parent_idx);
            /* Clear exec_done for the new child so PIPE_EXEC_WAIT works */
            if (child_pid > 0) {
                for (int fi = 0; fi < MAX_ACTIVE_PROCS; fi++) {
                    if (active_procs[fi].active
                        && active_procs[fi].pid == child_pid) {
                        exec_done[fi] = 0;
                        break;
                    }
                }
            }
            seL4_SetMR(0, (seL4_Word)child_pid);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_EXEC: {
            int ci = (int)badge - 1;
            if (ci < 0 || ci >= MAX_ACTIVE_PROCS || !active_procs[ci].active) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int nmrs = seL4_MessageInfo_get_length(msg);
            seL4_Word pipe_meta_word = seL4_GetMR(0);
            int exec_stdout_pipe = (int)((pipe_meta_word >> 16) & 0xFFFF) - 1;
            int exec_stdin_pipe = (int)(pipe_meta_word & 0xFFFF) - 1;

            char exec_buf[900];
            int ebi = 0;
            for (int m = 1; m < nmrs && ebi < 899; m++) {
                seL4_Word w = seL4_GetMR(m);
                for (int b = 0; b < 8 && ebi < 899; b++) {
                    exec_buf[ebi++] = (char)((w >> (b * 8)) & 0xFF);
                }
            }
            exec_buf[ebi] = 0;

            char *elf_path = exec_buf;

            #define MAX_USER_ARGV 32
            char *exec_argv[MAX_USER_ARGV];
            int exec_argc = 0;
            char child_cwd[128];
            child_cwd[0] = '/'; child_cwd[1] = 0;
            char child_redir_out[96];
            char child_redir_err[96];
            child_redir_out[0] = 0;
            child_redir_err[0] = 0;
            {
                int pos = 0;
                /* Skip path */
                while (pos < ebi && exec_buf[pos] != 0) pos++;
                pos++;
                /* Parse argv entries */
                while (pos < ebi && exec_buf[pos] != 0 && exec_argc < MAX_USER_ARGV) {
                    exec_argv[exec_argc++] = &exec_buf[pos];
                    while (pos < ebi && exec_buf[pos] != 0) pos++;
                    pos++;
                }
                /* Skip past double-null */
                if (pos < ebi) pos++;
                /* Extract CWD if present */
                if (pos < ebi && exec_buf[pos] != 0) {
                    int ci = 0;
                    while (pos < ebi && exec_buf[pos] != 0 && ci < 127) {
                        child_cwd[ci++] = exec_buf[pos++];
                    }
                    child_cwd[ci] = 0;
                }
                /* v0.4.115: skip past CWD's null, then extract optional
                 * stdout/stderr file-redirect paths (empty string = none) */
                if (pos < ebi) pos++;
                child_redir_out[0] = 0;
                child_redir_err[0] = 0;
                if (pos < ebi) {
                    int ri = 0;
                    while (pos < ebi && exec_buf[pos] != 0 && ri < 95) {
                        child_redir_out[ri++] = exec_buf[pos++];
                    }
                    child_redir_out[ri] = 0;
                    if (pos < ebi) pos++;
                }
                if (pos < ebi) {
                    int ri = 0;
                    while (pos < ebi && exec_buf[pos] != 0 && ri < 95) {
                        child_redir_err[ri++] = exec_buf[pos++];
                    }
                    child_redir_err[ri] = 0;
                }
            }

            active_proc_t *ap = &active_procs[ci];
            int old_pid = ap->pid;
            int old_ppid = ap->ppid;
            uint32_t old_uid = ap->uid;
            uint32_t old_gid = ap->gid;
            vka_object_t old_fault_ep = ap->fault_ep;
            int old_fope = ap->fault_on_pipe_ep;

            int esz = vfs_read(elf_path, elf_buf, sizeof(elf_buf));
            if (esz <= 0) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            elf_t elf;
            if (elf_newFile(elf_buf, esz, &elf) != 0) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            /* v0.4.263: refuse the exec when the memory pool is low BEFORE tearing
             * down the old image, so exec() returns -1 and the original process
             * survives (POSIX exec-failure semantics) instead of
             * sel4utils_elf_load failing mid-load + dumping the seL4-lib cascade.
             * Mirrors exec_server's pre-spawn headroom check. */
            if (vka_audit_check_headroom(2000) < 0) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            ap->ignore_next_fault = 1;
            /* v0.4.109: release old process's audit pages before destroy */
            vka_audit_release_proc_pages(&ap->audit_pages_allocated);
            /* v0.4.111: explicitly unmap COW R/O dup pages so the
             * vspace tear-down sees RESERVED entries (not POPULATED) */
            cow_release_proc(ci);
            cow_clear_proc(ci);
            clear_file_vmas(ci);  /* v0.4.146: drop stale file-mmap descriptors */
            clear_shared_text(ci);  /* v0.4.183: release old shared-text ref before destroy */
            sel4utils_destroy_process(&ap->proc, &vka);

            /* Free old fault ep -- minted cap vs allocated endpoint */
            if (old_fope) {
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                    old_fault_ep.cptr, seL4_WordBits);
                vka_cspace_free(&vka, old_fault_ep.cptr);
            } else {
                vka_free_object(&vka, &old_fault_ep);
            }

            /* Mint fresh pipe_ep as fault endpoint */
            vka_object_t new_fault_ep;
            if (mint_pipe_fault_ep(ci, &new_fault_ep)) {
                ap->active = 0;
                proc_remove(old_pid);
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            sel4utils_process_config_t cfg = process_config_new(&simple);
            cfg = process_config_create_cnode(cfg, 12);
            cfg = process_config_create_vspace(cfg, NULL, 0);
            cfg = process_config_priority(cfg, 200);
            cfg = process_config_auth(cfg, simple_get_tcb(&simple));
            cfg = process_config_fault_endpoint(cfg, new_fault_ep);

            sel4utils_process_t *proc = &ap->proc;
            int err = sel4utils_configure_process_custom(proc, &vka, &vspace, cfg);
            if (err) {
                /* v0.4.263: free the minted fault ep (a root cslot) + reply -1.
                 * This path previously leaked new_fault_ep AND never replied (the
                 * caller hung) -- the leak compounded under a spawn-storm. */
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                                  new_fault_ep.cptr, seL4_WordBits);
                vka_cspace_free(&vka, new_fault_ep.cptr);
                ap->active = 0;
                proc_remove(old_pid);
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            ap->num_segs = 0;
            for (int si = 0; si < elf_getNumProgramHeaders(&elf)
                 && ap->num_segs < MAX_ELF_SEGS; si++) {
                if (elf_getProgramHeaderType(&elf, si) == 1) {
                    elf_seg_info_t *seg = &ap->segs[ap->num_segs++];
                    seg->vaddr = (uintptr_t)elf_getProgramHeaderVaddr(&elf, si);
                    seg->memsz = (size_t)elf_getProgramHeaderMemorySize(&elf, si);
                    seg->filesz = (size_t)elf_getProgramHeaderFileSize(&elf, si);
                    seg->flags = (uint32_t)elf_getProgramHeaderFlags(&elf, si);
                }
            }

            proc->entry_point = sel4utils_elf_load(
                &proc->vspace, &vspace, &vka, &vka, &elf);
            if (!proc->entry_point) {
                sel4utils_destroy_process(proc, &vka);
                /* v0.4.263: free the minted fault ep (a root cslot) + reply -1.
                 * This path previously leaked new_fault_ep AND never replied. */
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                                  new_fault_ep.cptr, seL4_WordBits);
                vka_cspace_free(&vka, new_fault_ep.cptr);
                ap->active = 0;
                proc_remove(old_pid);
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            proc->sysinfo = sel4utils_elf_get_vsyscall(&elf);

            /* v0.4.106: Demand-paged BSS for forked-then-execed children.
             * Unmap the BSS portion of LOAD segments and create a
             * reservation so the fault handler (above, in this server's
             * dispatch loop) can map pages on demand. */
            ap->bss_lazy_start = 0;
            ap->bss_lazy_end = 0;
            ap->bss_reservation = NULL;
            {
                int nph2 = elf_getNumProgramHeaders(&elf);
                uintptr_t bs = 0, be = 0;
                for (int i = 0; i < nph2; i++) {
                    if (elf_getProgramHeaderType(&elf, i) != 1) continue;
                    uintptr_t v   = (uintptr_t)elf_getProgramHeaderVaddr(&elf, i);
                    size_t fz     = (size_t)elf_getProgramHeaderFileSize(&elf, i);
                    size_t mz     = (size_t)elf_getProgramHeaderMemorySize(&elf, i);
                    if (mz <= fz) continue;
                    uintptr_t s = (v + fz + 0xFFF) & ~((uintptr_t)0xFFF);
                    uintptr_t e = (v + mz + 0xFFF) & ~((uintptr_t)0xFFF);
                    if (e <= s) continue;
                    if ((e - s) > (be - bs)) { bs = s; be = e; }
                }
                if (bs && be > bs) {
                    size_t pages = (be - bs) / 4096;
                    vspace_unmap_pages(&proc->vspace, (void *)bs,
                                       pages, seL4_PageBits, &vka);
                    int bidx = (int)(ap - active_procs);
                    sel4utils_res_t *bres = &aios_bss_res[bidx];
                    int rerr = sel4utils_reserve_range_at_no_alloc(
                        &proc->vspace, bres, (void *)bs, pages * 4096,
                        seL4_AllRights, 1);
                    if (!rerr) {
                        ap->bss_lazy_start = bs;
                        ap->bss_lazy_end   = be;
                        ap->bss_reservation = bres;
                        AIOS_LOG_DEBUG_V("BSS lazy pages=", (unsigned long)pages);
                    }
                }
            }

            /* v0.4.183: share the read-only text segment across same-binary
             * procs; fall back to per-proc demand-text (v0.4.181) only when
             * sharing is skipped (cache full / tiny text), eager pages intact. */
            if (setup_shared_text(ci, &elf, elf_path) <= 0)
                setup_demand_text(ci, &elf, elf_path);

            seL4_CPtr cs = sel4utils_copy_cap_to_process(proc, &vka, serial_ep.cptr);
            ap->child_ser_slot = cs;

            cspacepath_t fs_s, fs_d;
            vka_cspace_make_path(&vka, fs_ep_cap, &fs_s);
            vka_cspace_alloc_path(&vka, &fs_d);
            {
                int merr = seL4_CNode_Mint(fs_d.root, fs_d.capPtr, fs_d.capDepth,
                    fs_s.root, fs_s.capPtr, fs_s.capDepth,
                    seL4_AllRights, (seL4_Word)(ci + 1));
                if (merr) AIOS_LOG_WARN_V("fs_ep Mint failed: ", (unsigned long)merr);
            }
            seL4_CPtr cf = sel4utils_copy_cap_to_process(proc, &vka, fs_d.capPtr);
            ap->child_fs_slot = cf;

            cspacepath_t te_s, te_d;
            vka_cspace_make_path(&vka, thread_ep_cap, &te_s);
            vka_cspace_alloc_path(&vka, &te_d);
            {
                int merr = seL4_CNode_Mint(te_d.root, te_d.capPtr, te_d.capDepth,
                    te_s.root, te_s.capPtr, te_s.capDepth,
                    seL4_AllRights, (seL4_Word)(ci + 1));
                if (merr) AIOS_LOG_WARN_V("thread_ep Mint failed: ", (unsigned long)merr);
            }
            seL4_CPtr ct = sel4utils_copy_cap_to_process(proc, &vka, te_d.capPtr);
            ap->child_thread_slot = ct;

            seL4_CPtr ca = sel4utils_copy_cap_to_process(proc, &vka, auth_ep_cap);
            ap->child_auth_slot = ca;

            cspacepath_t pi_s, pi_d;
            vka_cspace_make_path(&vka, pipe_ep_cap, &pi_s);
            vka_cspace_alloc_path(&vka, &pi_d);
            {
                int merr = seL4_CNode_Mint(pi_d.root, pi_d.capPtr, pi_d.capDepth,
                    pi_s.root, pi_s.capPtr, pi_s.capDepth,
                    seL4_AllRights, (seL4_Word)(ci + 1));
                if (merr) AIOS_LOG_WARN_V("pipe_ep Mint failed: ", (unsigned long)merr);
            }
            seL4_CPtr cp2 = sel4utils_copy_cap_to_process(proc, &vka, pi_d.capPtr);
            ap->child_pipe_slot = cp2;

            /* M3: copy net_ep to child (0 if no network) */
            seL4_CPtr cn = 0;
            if (net_ep_cap)
                cn = sel4utils_copy_cap_to_process(proc, &vka, net_ep_cap);

            /* Copy disp_ep to child (0 if no display) */
            seL4_CPtr cd = 0;
            if (disp_ep_cap)
                cd = sel4utils_copy_cap_to_process(proc, &vka, disp_ep_cap);

            char s_ser[16], s_fs[16], s_thr[16], s_auth[16], s_pip[16];
            char s_net[16], s_disp[16], s_crypto[16];
            char cwd[64];
            snprintf(s_ser, 16, "%lu", (unsigned long)cs);
            snprintf(s_fs, 16, "%lu", (unsigned long)cf);
            snprintf(s_thr, 16, "%lu", (unsigned long)ct);
            snprintf(s_auth, 16, "%lu", (unsigned long)ca);
            snprintf(s_pip, 16, "%lu", (unsigned long)cp2);
            snprintf(s_net, 16, "%lu", (unsigned long)cn);
            snprintf(s_disp, 16, "%lu", (unsigned long)cd);
            snprintf(s_crypto, 16, "%lu", (unsigned long)(crypto_ep_cap ? sel4utils_copy_cap_to_process(&active_procs[ci].proc, &vka, crypto_ep_cap) : 0));

            if (exec_stdout_pipe >= 0 || exec_stdin_pipe >= 0) {
                int sp_id = exec_stdout_pipe < 0 ? 99 : exec_stdout_pipe;
                int rp_id = exec_stdin_pipe < 0 ? 99 : exec_stdin_pipe;
                snprintf(cwd, 64, "%u:%u:%d:%d:%s",
                         old_uid, old_gid, sp_id, rp_id, child_cwd);
            } else {
                snprintf(cwd, 64, "%u:%u:%s", old_uid, old_gid, child_cwd);
            }

            char *spawn_argv[12 + MAX_USER_ARGV];
            int spawn_argc = 0;
            spawn_argv[spawn_argc++] = s_ser;
            spawn_argv[spawn_argc++] = s_fs;
            spawn_argv[spawn_argc++] = s_thr;
            spawn_argv[spawn_argc++] = s_auth;
            spawn_argv[spawn_argc++] = s_pip;
            spawn_argv[spawn_argc++] = s_net;
            spawn_argv[spawn_argc++] = s_disp;
            spawn_argv[spawn_argc++] = s_crypto;
            spawn_argv[spawn_argc++] = cwd;
            /* v0.4.115: argv[9] = stdout file-redirect path (or empty),
             * argv[10] = stderr file-redirect path (or empty). User
             * argv (incl. progname) starts at argv[11]. */
            spawn_argv[spawn_argc++] = child_redir_out;
            spawn_argv[spawn_argc++] = child_redir_err;
            if (exec_argc > 0) {
                for (int ai = 0; ai < exec_argc
                     && spawn_argc < 11 + MAX_USER_ARGV; ai++)
                    spawn_argv[spawn_argc++] = exec_argv[ai];
            } else {
                spawn_argv[spawn_argc++] = elf_path;
            }
            err = sel4utils_spawn_process_v(proc, &vka, &vspace,
                                            spawn_argc, spawn_argv, 1);
            if (err) {
                clear_shared_text(ci);  /* v0.4.183: release shared-text before destroy */
                sel4utils_destroy_process(proc, &vka);
                ap->active = 0;
                proc_remove(old_pid);
                break;
            }

            aios_assign_core(proc->thread.tcb.cptr);   /* v0.4.257 Stage S: distribute to cores 1..N-1 */

            ap->active = 1;
            ap->pid = old_pid;
            ap->ppid = old_ppid;
            ap->uid = old_uid;
            ap->gid = old_gid;
            ap->fault_ep = new_fault_ep;
            ap->fault_on_pipe_ep = 1;
            ap->ignore_next_fault = 0;  /* v0.4.85: clear flag for new process */
            ap->stdout_pipe_id = exec_stdout_pipe;
            ap->stdin_pipe_id = exec_stdin_pipe;
            /* v0.4.84: increment pipe refs so child exit does not
             * close the pipe while parent still uses it */
            if (exec_stdout_pipe >= 0 && exec_stdout_pipe < MAX_PIPES
                && pipes[exec_stdout_pipe].active) {
                /* v0.4.141: this exec-ing child is the live writer for this
                 * pipe. Forked writers hold no write_ref until exec, so the
                 * parent (pipe creator) and the sibling reader may close their
                 * inherited write-end copies first -- driving write_refs to 0
                 * and latching write_closed -- before a slow writer (one that
                 * does an fs read before its first write, e.g. cat/ls) reaches
                 * exec. That premature write_closed gave the reader an early
                 * EOF (cat /etc/passwd | wc -l returned 0, while seq | wc and
                 * echo | wc -- writers that write immediately -- worked).
                 * Reclaim the end: ensure a live ref and clear the premature
                 * close. */
                if (pipes[exec_stdout_pipe].write_refs < 1)
                    pipes[exec_stdout_pipe].write_refs = 1;
                else
                    pipes[exec_stdout_pipe].write_refs++;
                pipes[exec_stdout_pipe].write_closed = 0;
                /* v0.4.258: a fresh writer is claiming this ring -- clear any
                 * ring EOF/EPIPE latched by the shell closing its inherited
                 * pipe-end before this writer reached exec (mirror write_closed). */
                if (pipe_is_ring(exec_stdout_pipe)) {
                    shm_ring_hdr_t *rh = ring_hdr(exec_stdout_pipe);
                    /* Clear ONLY writer_closed (this end's flag, mirroring write_closed
                     * above). Do NOT touch reader_closed: if the reader already exited
                     * (e.g. `yes | head -1`), its EPIPE must survive so this writer stops
                     * instead of spinning on a ring nobody drains. */
                    if (rh) rh->writer_closed = 0;
                }
                pipe_had_child[exec_stdout_pipe] = 1;  /* v0.4.139 */
            }
            if (exec_stdin_pipe >= 0 && exec_stdin_pipe < MAX_PIPES
                && pipes[exec_stdin_pipe].active) {
                /* v0.4.141: symmetric reclaim for the read end -- a forked
                 * reader that execs after its inherited read-end copy was
                 * closed elsewhere would otherwise inherit a latched
                 * read_closed. */
                if (pipes[exec_stdin_pipe].read_refs < 1)
                    pipes[exec_stdin_pipe].read_refs = 1;
                else
                    pipes[exec_stdin_pipe].read_refs++;
                pipes[exec_stdin_pipe].read_closed = 0;
                if (pipe_is_ring(exec_stdin_pipe)) {
                    shm_ring_hdr_t *rh = ring_hdr(exec_stdin_pipe);
                    /* Clear ONLY reader_closed (this end's flag, mirroring read_closed).
                     * Do NOT touch writer_closed: if the writer already finished and
                     * closed, its EOF must survive so this reader sees end-of-stream
                     * instead of blocking forever on an empty, writer-less ring. */
                    if (rh) rh->reader_closed = 0;
                }
                pipe_had_child[exec_stdin_pipe] = 1;  /* v0.4.139 */
            }
            ap->exit_status = 0;
            ap->sig_pending = 0;  /* v0.4.87: clear stale signals from previous slot occupant */
            ap->exit_cb_ran = 0;
            ap->pipe_write_closed = 0;

            ap->num_threads = 0;
            for (int ti = 0; ti < MAX_THREADS_PER_PROC; ti++)
                ap->threads[ti].active = 0;

            for (int pi = 0; pi < PROC_MAX; pi++) {
                if (proc_table[pi].active && proc_table[pi].pid == old_pid) {
                    int ni = 0;
                    const char *n = elf_path;
                    while (*n && ni < 63) proc_table[pi].name[ni++] = *n++;
                    proc_table[pi].name[ni] = 0;
                    break;
                }
            }
            exec_done[ci] = 1;
            check_exec_wait(ci);

            /* Track as foreground process for Ctrl-C */
            fg_pid = old_pid;
            fg_fault_ep = new_fault_ep.cptr;

            /* No reply -- old process destroyed, new one running */
            break;
        }
        case PIPE_EXEC_WAIT: {
            int target_pid = (int)seL4_GetMR(0);
            int target_idx = -1;
            for (int ti = 0; ti < MAX_ACTIVE_PROCS; ti++) {
                if (active_procs[ti].active
                    && active_procs[ti].pid == target_pid) {
                    target_idx = ti;
                    break;
                }
            }
            if (target_idx < 0 || exec_done[target_idx]) {
                /* Child already exec-ed or not found -- unblock now */
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            /* Defer reply until child PIPE_EXEC completes */
            seL4_CNode_Delete(seL4_CapInitThreadCNode,
                exec_wait_reply_obj.cptr, seL4_WordBits);
            seL4_CNode_SaveCaller(seL4_CapInitThreadCNode,
                exec_wait_reply_obj.cptr, seL4_WordBits);
            exec_wait.active = 1;
            exec_wait.child_idx = target_idx;
            exec_wait.reply_cap = exec_wait_reply_obj.cptr;
            break;
        }
        case PIPE_MAP_SHM: {
            /* v0.4.66: lazy xfer. v0.4.257 coalescing: MR1 picks the direction --
             * 0 = READ xfer (server->reader), 1 = WRITE xfer (writer->server). The two
             * directions use SEPARATE per-pipe pages so a writer and reader (different
             * processes) never race on a single shared staging page. */
            int pi = (int)seL4_GetMR(0);
            int dir = (int)seL4_GetMR(1) ? 1 : 0;
            int ci = (int)badge - 1;
            if (pi < 0 || pi >= MAX_PIPES || !pipes[pi].active
                || ci < 0 || ci >= MAX_ACTIVE_PROCS
                || !active_procs[ci].active) {
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            vka_object_t *xframe = dir ? &pipes[pi].xfer_frame_w : &pipes[pi].xfer_frame;
            char    **xbuf   = dir ? &pipes[pi].xfer_buf_w        : &pipes[pi].xfer_buf;
            int      *xvalid = dir ? &pipes[pi].xfer_valid_w      : &pipes[pi].xfer_valid;
            seL4_CPtr *xcop  = dir ? pipes[pi].xfer_copies_w      : pipes[pi].xfer_copies;
            int      *xcnt   = dir ? &pipes[pi].xfer_copy_count_w : &pipes[pi].xfer_copy_count;
            /* Lazy alloc: create xfer frame on first request */
            if (!*xvalid) {
                vka_audit_frame(VKA_SUB_PIPE, 1);
                if (vka_alloc_frame(&vka, seL4_PageBits, xframe) != 0) {
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                /* v0.4.165: map the shared xfer page CACHEABLE, matching the client
                 * mapping below. Both ends MUST use the same memory type or the Cortex-A72
                 * loses coherency (the v0.4.164 all-NUL bug; QEMU does not model it).
                 * Cacheable-on-both is coherent on the A72 and fast for large transfers. */
                void *xm = vspace_map_pages(&vspace, &xframe->cptr, NULL,
                    seL4_AllRights, 1, seL4_PageBits, 1);
                if (!xm) {
                    vka_free_object(&vka, xframe);
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                *xbuf = (char *)xm;
                *xvalid = 1;
            }
            /* Copy frame cap to fresh slot */
            cspacepath_t xsrc, xdest;
            vka_cspace_make_path(&vka, xframe->cptr, &xsrc);
            if (vka_cspace_alloc_path(&vka, &xdest) != 0) {
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            if (seL4_CNode_Copy(xdest.root, xdest.capPtr, xdest.capDepth,
                    xsrc.root, xsrc.capPtr, xsrc.capDepth,
                    seL4_AllRights) != 0) {
                vka_cspace_free(&vka, xdest.capPtr);
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            /* Map the COPY into child VSpace -- cacheable, matching the server mapping. */
            void *child_vaddr = vspace_map_pages(
                &active_procs[ci].proc.vspace,
                &xdest.capPtr, NULL,
                seL4_AllRights, 1, seL4_PageBits, 1);
            if (!child_vaddr) {
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                    xdest.capPtr, seL4_WordBits);
                vka_cspace_free(&vka, xdest.capPtr);
            } else if (*xcnt < 2) {
                xcop[(*xcnt)++] = xdest.capPtr;
            }
            seL4_SetMR(0, (seL4_Word)child_vaddr);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_WRITE_SHM: {
            /* v0.4.66/257: writer put data in the WRITE xfer page, copy to ring buffer.
             * Returns the bytes accepted (may be < wlen if the ring is full -> the client
             * advances by the returned count + retries, exactly like the MR PIPE_WRITE). */
            int pi = (int)seL4_GetMR(0);
            int wlen = (int)seL4_GetMR(1);
            if (pi < 0 || pi >= MAX_PIPES || !pipes[pi].active
                || !pipes[pi].xfer_valid_w) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            pipe_t *p = &pipes[pi];
            if (wlen > PAGE_SIZE) wlen = PAGE_SIZE;
            /* v0.4.258: ring-mode fallback (writer didn't map the ring directly) --
             * push the write xfer page through the ring instead of shm_buf. On a
             * full ring, PARK the writer (SaveCaller) rather than returning 0 and
             * letting the client spin-drop -- the reader's drain (or teardown =
             * EPIPE) resumes it. No premature-close check here: parking is safe even
             * if a stale read_closed was latched, because the writer is woken by the
             * reader's real activity / exit, not by the unreliable refcount. */
            if (pipe_is_ring(pi)) {
                int written = ring_server_push(pi, p->xfer_buf_w, wlen);
                if (written > 0) {
                    seL4_SetMR(0, (seL4_Word)written);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    wake_one_blocked_reader(pi);
                    break;
                }
                /* full: EPIPE if the reader is gone, else block until it drains */
                if (pipes[pi].read_closed) {
                    seL4_SetMR(0, (seL4_Word)-1);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                if (park_writer_save(pi, wlen, (const char *)0) != 0) {
                    seL4_SetMR(0, 0);   /* no cap slot -> client retries (rare) */
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                }
                break;   /* parked: no reply now */
            }
            int avail = PIPE_BUF_SIZE - p->count;
            int written = (wlen < avail) ? wlen : avail;
            /* Copy from the write xfer page to the ring buffer */
            for (int i = 0; i < written; i++)
                p->shm_buf[(p->head + p->count + i) % PIPE_BUF_SIZE] =
                    p->xfer_buf_w[i];
            p->count += written;
            seL4_SetMR(0, (seL4_Word)written);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            if (written > 0) wake_one_blocked_reader(pi);
            break;
        }
        case PIPE_READ_SHM: {
            /* v0.4.66: copy ring buffer to xfer page, reply with length */
            int pi = (int)seL4_GetMR(0);
            int max_len = (int)seL4_GetMR(1);
            if (pi >= 0 && pi < MAX_PIPES)
            if (pi < 0 || pi >= MAX_PIPES || !pipes[pi].active) {
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            /* v0.4.85: return -2 for !xfer_valid (distinct from EOF=0) */
            if (!pipes[pi].xfer_valid) {
                seL4_SetMR(0, (seL4_Word)(-2));
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            pipe_t *p = &pipes[pi];
            if (max_len > PAGE_SIZE) max_len = PAGE_SIZE;
            /* v0.4.258: ring mode -- serve from the ring into the xfer page; block
             * via SaveCaller (is_shm=1). The direct reader falls through to here when
             * it finds the ring empty; a fallback reader uses it for every read. */
            if (pipe_is_ring(pi)) {
                int nb = (int)seL4_GetMR(2);
                int want = max_len;
                if (want > (int)SHM_RING_DATA) want = (int)SHM_RING_DATA;
                if (want <= 0) {   /* read(fd,buf,0) returns 0 -- never park on a 0-len read */
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                int got = ring_server_pull(pi, p->xfer_buf, want);
                if (got <= 0) {
                    shm_ring_hdr_t *h = ring_hdr(pi);
                    if (h->writer_closed) {
                        seL4_SetMR(0, 0);
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                        break;
                    }
                    if (nb & 1) {
                        seL4_SetMR(0, (seL4_Word)(-1));
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                        break;
                    }
                    h->reader_waiting = 1;           /* Dekker: flag, fence, re-pull */
                    shm_ring_fence();
                    got = ring_server_pull(pi, p->xfer_buf, want);
                    if (got <= 0) {
                        if (h->writer_closed) {
                            h->reader_waiting = 0;
                            seL4_SetMR(0, 0);
                            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                            break;
                        }
                        if (park_reader_save(pi, max_len, 1, (int)badge) < 0) {
                            h->reader_waiting = 0;
                            seL4_SetMR(0, 0);
                            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                            break;
                        }
                        break;   /* parked; reader_waiting stays set */
                    }
                    h->reader_waiting = 0;
                }
                seL4_SetMR(0, (seL4_Word)got);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                wake_blocked_writer(pi, 0);   /* freed space -> let a parked writer refill */
                break;
            }
            if (p->count == 0) {
                if (p->write_closed) {
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                /* v0.4.79: O_NONBLOCK -- return -1 instead of blocking */
                {
                    int nb_flags = (int)seL4_GetMR(2);
                    if (nb_flags & 1) {
                        seL4_SetMR(0, (seL4_Word)(-1));
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                        break;
                    }
                }
                /* Block reader -- same as PIPE_READ but with is_shm flag */
                int bi = -1;
                for (int b = 0; b < MAX_PIPE_READ_BLOCKED; b++) {
                    if (!pipe_read_blocked[b].active) { bi = b; break; }
                }
                if (bi < 0) {
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                    read_reply_objs[bi].cptr, seL4_WordBits);
                seL4_CNode_SaveCaller(seL4_CapInitThreadCNode,
                    read_reply_objs[bi].cptr, seL4_WordBits);
                pipe_read_blocked[bi].active = 1;
                pipe_read_blocked[bi].pipe_id = pi;
                pipe_read_blocked[bi].max_len = max_len;
                pipe_read_blocked[bi].is_shm = 1;
                pipe_read_blocked[bi].reply_cap = read_reply_objs[bi].cptr;
                /* v0.4.87: record caller PID for signal wakeup */
                {
                    int ci = (int)badge - 1;
                    pipe_read_blocked[bi].caller_pid =
                        (ci >= 0 && ci < MAX_ACTIVE_PROCS &&
                         active_procs[ci].active) ?
                        active_procs[ci].pid : -1;
                }
                break;
            }
            int rlen = p->count < max_len ? p->count : max_len;
            /* Copy from ring buffer to xfer page */
            for (int i = 0; i < rlen; i++)
                p->xfer_buf[i] = p->shm_buf[(p->head + i) % PIPE_BUF_SIZE];
            p->head = (p->head + rlen) % PIPE_BUF_SIZE;
            p->count -= rlen;
            seL4_SetMR(0, (seL4_Word)rlen);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_MAP_RING: {
            /* v0.4.258: map the SPSC SHM-ring frame into the caller's vspace and
             * return the child vaddr (0 -> the caller falls back to the legacy IPC
             * path, whose server handlers route into the SAME ring, so data is never
             * split). Gated on pipe_ring[pi].armed (the per-pipe decision latched at
             * PIPE_CREATE) -- NOT the live g_shm_ring flag, so a toggle mid-pipeline
             * cannot leave the two ends disagreeing. The ring is mapped RW both ways
             * (the writer reads head; the reader writes head); MR1's dir is advisory. */
            int pi = (int)seL4_GetMR(0);
            (void)seL4_GetMR(1);   /* MR1 dir: advisory (ring is mapped RW both ways) */
            int ci = (int)badge - 1;
            if (pi < 0 || pi >= MAX_PIPES || !pipes[pi].active
                || !pipe_ring[pi].armed
                || ci < 0 || ci >= MAX_ACTIVE_PROCS || !active_procs[ci].active) {
                g_ring_map_fail++;
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            /* copies[] (the only cleanup record) holds 4 slots: writer + reader +
             * a couple of fork dups. Past that, decline the map -- the end falls
             * back to legacy IPC, which the server routes into this SAME ring, so
             * data is never split. Returning here (before allocating the cap) avoids
             * leaking an untracked cap copy under wide fan-out. */
            if (pipe_ring[pi].copy_count >= 4) {
                g_ring_map_fail++;
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            if (!pipe_ring[pi].valid && ring_alloc(pi) != 0) {
                g_ring_map_fail++;
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            cspacepath_t rsrc, rdest;
            vka_cspace_make_path(&vka, pipe_ring[pi].frame.cptr, &rsrc);
            if (vka_cspace_alloc_path(&vka, &rdest) != 0) {
                g_ring_map_fail++;
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            if (seL4_CNode_Copy(rdest.root, rdest.capPtr, rdest.capDepth,
                    rsrc.root, rsrc.capPtr, rsrc.capDepth, seL4_AllRights) != 0) {
                vka_cspace_free(&vka, rdest.capPtr);
                g_ring_map_fail++;
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            void *cv = vspace_map_pages(&active_procs[ci].proc.vspace,
                &rdest.capPtr, NULL, seL4_AllRights, 1, seL4_PageBits, 1 /* cacheable */);
            if (!cv) {
                seL4_CNode_Delete(seL4_CapInitThreadCNode, rdest.capPtr, seL4_WordBits);
                vka_cspace_free(&vka, rdest.capPtr);
                g_ring_map_fail++;
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            /* Room guaranteed by the copy_count >= 4 early-return above -> every
             * mapped cap is tracked in copies[] and freed in ring_free (no leak). */
            pipe_ring[pi].copies[pipe_ring[pi].copy_count++] = rdest.capPtr;
            g_ring_map_ok++;
            seL4_SetMR(0, (seL4_Word)cv);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_RING_WAKE: {
            g_ring_wake++;
            /* v0.4.258: a direct writer advanced tail and observed reader_waiting
             * -> ask the server to serve+wake the reader parked on the empty ring.
             * Idempotent: if no reader is parked (it un-parked itself, or the WAKE
             * raced ahead of the reader's PIPE_READ* park), this is a harmless no-op. */
            int pi = (int)seL4_GetMR(0);
            if (pi >= 0 && pi < MAX_PIPES && pipe_is_ring(pi))
                wake_one_blocked_reader(pi);
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_MMAP_ANON: {
            /* v0.4.104: Allocate anonymous pages on demand and map them
             * into caller's VSpace. Returns vaddr in MR0 (or 0 on fail).
             * MR0 = page count requested (max 1024 = 4MB at a time)
             * Used as a fallback when a process's static morecore is
             * exhausted. Cap freed via sel4utils_destroy_process on exit. */
            int ci = (int)badge - 1;
            int pages = (int)seL4_GetMR(0);
            if (ci < 0 || ci >= MAX_ACTIVE_PROCS
                || !active_procs[ci].active
                || pages <= 0 || pages > 1024) {
                AIOS_LOG_WARN_V("MMAP_ANON: invalid request pages=", (unsigned long)pages);
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            /* Headroom check */
            extern int vka_audit_check_headroom(int needed_pages);
            if (vka_audit_check_headroom(pages) < 0) {
                AIOS_LOG_ERROR_V("MMAP_ANON: pool too low; need=",
                                 (unsigned long)pages);
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            /* Allocate N pages and map them contiguously in caller VSpace */
            void *child_vaddr = vspace_new_pages(
                &active_procs[ci].proc.vspace,
                seL4_AllRights, pages, seL4_PageBits);
            if (!child_vaddr) {
                AIOS_LOG_ERROR_V("MMAP_ANON: vspace_new_pages failed pages=",
                                 (unsigned long)pages);
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            vka_audit_frame(VKA_SUB_OTHER, pages);
            AIOS_LOG_DEBUG_V("MMAP_ANON: allocated pages=", (unsigned long)pages);
            seL4_SetMR(0, (seL4_Word)child_vaddr);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_MUNMAP_ANON: {
            /* v0.4.128: unmap pages in caller's vspace -- complements
             * PIPE_MMAP_ANON. The caller-vspace tracking owns the cookie
             * for each frame (vspace_new_pages set it to the underlying
             * UT), so passing &vka frees the frame too. Only operates
             * on currently-mapped pages within the range; missing
             * entries are silently skipped (consistent with mprotect). */
            int ci = (int)badge - 1;
            uintptr_t va = (uintptr_t)seL4_GetMR(0);
            int npages = (int)seL4_GetMR(1);
            if (ci < 0 || ci >= MAX_ACTIVE_PROCS
                || !active_procs[ci].active
                || npages <= 0 || npages > 4096
                || (va & (PAGE_SIZE - 1)) != 0) {
                seL4_SetMR(0, (seL4_Word)-22 /* -EINVAL */);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int unmapped = 0;
            vspace_t *cvs = &active_procs[ci].proc.vspace;
            for (int i = 0; i < npages; i++) {
                uintptr_t page_va = va + (uintptr_t)i * PAGE_SIZE;
                seL4_CPtr cap = vspace_get_cap(cvs, (void *)page_va);
                if (cap == seL4_CapNull) continue;
                vspace_unmap_pages(cvs, (void *)page_va, 1, seL4_PageBits, &vka);
                unmapped++;
            }
            if (unmapped > 0) {
                vka_audit_frame_release(unmapped);
            }
            seL4_SetMR(0, 0);
            seL4_SetMR(1, (seL4_Word)unmapped);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
            break;
        }
        case PIPE_MPROTECT: {
            /* v0.4.126/127: change page rights via seL4_ARM_Page_Map
             * remap-in-place (kernel performPageInvocationMap rewrites the
             * PTE and TLB-flushes; cap stays valid). See
             * docs/NEXT_20260502c.md.
             * MR0 = vaddr (page-aligned), MR1 = num pages, MR2 = prot bits.
             * PROT_READ=1, PROT_WRITE=2, PROT_EXEC=4. PROT_NONE (prot=0)
             * remaps with all rights cleared -- subsequent accesses fault.
             * PROT_EXEC controls the ARM Execute-Never attribute.
             * Pages not currently mapped are skipped silently. */
            int ci = (int)badge - 1;
            uintptr_t va = (uintptr_t)seL4_GetMR(0);
            int npages = (int)seL4_GetMR(1);
            int prot = (int)seL4_GetMR(2);
            if (ci < 0 || ci >= MAX_ACTIVE_PROCS
                || !active_procs[ci].active
                || npages <= 0 || npages > 4096
                || (va & (PAGE_SIZE - 1)) != 0) {
                seL4_SetMR(0, (seL4_Word)-22 /* -EINVAL */);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int want_read = (prot & 1) != 0;
            int want_write = (prot & 2) != 0;
            int want_exec = (prot & 4) != 0;
            seL4_CapRights_t rights = seL4_CapRights_new(0, 0,
                want_read || want_write, want_write);
            seL4_ARM_VMAttributes attrs = want_exec
                ? seL4_ARM_Default_VMAttributes
                : (seL4_ARM_VMAttributes)(seL4_ARM_Default_VMAttributes
                                          | seL4_ARM_ExecuteNever);
            seL4_CPtr caller_pd = vspace_get_root(&active_procs[ci].proc.vspace);
            int remapped = 0, skipped = 0, errs = 0;
            for (int i = 0; i < npages; i++) {
                uintptr_t page_va = va + (uintptr_t)i * PAGE_SIZE;
                seL4_CPtr cap = vspace_get_cap(
                    &active_procs[ci].proc.vspace, (void *)page_va);
                if (cap == seL4_CapNull) { skipped++; continue; }
                int merr = seL4_ARM_Page_Map(cap, caller_pd,
                    (seL4_Word)page_va, rights, attrs);
                if (merr) { errs++; continue; }
                remapped++;
            }
            if (errs > 0) {
                AIOS_LOG_WARN_V("MPROTECT: errs=", (unsigned long)errs);
            }
            seL4_SetMR(0, errs ? (seL4_Word)-1 : 0);
            seL4_SetMR(1, (seL4_Word)remapped);
            seL4_SetMR(2, (seL4_Word)skipped);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 3));
            break;
        }
        case PIPE_MMAP_FILE: {
            /* v0.4.146: file-backed mmap (architecture B -- demand-paged).
             * Reserve the range and record a descriptor; allocate NO frames.
             * The first touch of each page faults into handle_file_mmap_fault,
             * which allocates a frame, fills it from the file via vfs_pread,
             * and resumes. MR0=pages, MR1=offset, MR2=path_len, MR3+=path.
             * Reply MR0=vaddr (0 on failure). */
            int ci = (int)badge - 1;
            int pages = (int)seL4_GetMR(0);
            long foff = (long)seL4_GetMR(1);
            int pl = (int)seL4_GetMR(2);
            char fpath[128];
            int fi = 0;
            for (int m = 3; fi < pl && fi < 127; m++) {
                seL4_Word w = seL4_GetMR(m);
                for (int b = 0; b < 8 && fi < pl && fi < 127; b++)
                    fpath[fi++] = (char)((w >> (b * 8)) & 0xFF);
            }
            fpath[fi] = 0;
            if (ci < 0 || ci >= MAX_ACTIVE_PROCS || !active_procs[ci].active
                || pages <= 0 || pages > 1024 || fi == 0) {
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int slot = -1;
            for (int i = 0; i < MAX_FILE_VMAS; i++)
                if (!file_vmas[i].active) { slot = i; break; }
            if (slot < 0) {
                AIOS_LOG_ERROR("MMAP_FILE: no free vma slot");
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            void *vout = NULL;
            reservation_t rsv = vspace_reserve_range(
                &active_procs[ci].proc.vspace, (size_t)pages * PAGE_SIZE,
                seL4_AllRights, 1, &vout);
            if (rsv.res == NULL || vout == NULL) {
                AIOS_LOG_ERROR("MMAP_FILE: reserve_range failed");
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            file_vmas[slot].active = 1;
            file_vmas[slot].ci = ci;
            file_vmas[slot].va_start = (uintptr_t)vout;
            file_vmas[slot].va_end = (uintptr_t)vout + (size_t)pages * PAGE_SIZE;
            file_vmas[slot].offset = foff;
            file_vmas[slot].reservation = rsv.res;
            for (int i = 0; i < fi && i < 127; i++)
                file_vmas[slot].path[i] = fpath[i];
            file_vmas[slot].path[fi < 127 ? fi : 127] = 0;
            AIOS_LOG_DEBUG_V("MMAP_FILE(lazy): pages=", (unsigned long)pages);
            seL4_SetMR(0, (seL4_Word)vout);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_MUNMAP_FILE: {
            /* v0.4.146: tear down a demand-paged file mapping -- unmap any
             * resident pages, release the reservation, drop the descriptor.
             * (Write-back, if needed, already happened via PIPE_MSYNC.)
             * MR0=vaddr, MR1=pages. Reply MR0=0/-EINVAL, MR1=pages freed. */
            int ci = (int)badge - 1;
            uintptr_t va = (uintptr_t)seL4_GetMR(0);
            int npages = (int)seL4_GetMR(1);
            if (ci < 0 || ci >= MAX_ACTIVE_PROCS || !active_procs[ci].active
                || npages <= 0 || (va & (PAGE_SIZE - 1)) != 0) {
                seL4_SetMR(0, (seL4_Word)-22 /* -EINVAL */);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            vspace_t *cvs = &active_procs[ci].proc.vspace;
            int unmapped = 0;
            for (int p = 0; p < npages; p++) {
                uintptr_t pva = va + (uintptr_t)p * PAGE_SIZE;
                if (vspace_get_cap(cvs, (void *)pva) == seL4_CapNull) continue;
                vspace_unmap_pages(cvs, (void *)pva, 1, seL4_PageBits, &vka);
                unmapped++;
            }
            if (unmapped > 0) {
                vka_audit_frame_release(unmapped);
                /* These pages were counted into audit_pages_allocated when
                 * faulted in (handle_file_mmap_fault); drop them here so the
                 * teardown release does not double-count. */
                if (active_procs[ci].audit_pages_allocated >= unmapped)
                    active_procs[ci].audit_pages_allocated -= unmapped;
                else
                    active_procs[ci].audit_pages_allocated = 0;
            }
            file_vma_t *v = file_vma_find(ci, va);
            if (v) {
                reservation_t rsv = { .res = v->reservation };
                vspace_free_reservation(cvs, rsv);
                v->active = 0;
            }
            seL4_SetMR(0, 0);
            seL4_SetMR(1, (seL4_Word)unmapped);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
            break;
        }
        case PIPE_MSYNC: {
            /* v0.4.145: write a file-backed mapping back to disk via vfs_pwrite.
             * Sent only for MAP_SHARED + writable maps; the client has already
             * clamped len to the file size. MR0=vaddr, MR1=len, MR2=offset,
             * MR3=path_len, MR4+=path. Reply MR0=0 / -EINVAL, MR1=bytes. */
            int ci = (int)badge - 1;
            uintptr_t va = (uintptr_t)seL4_GetMR(0);
            int len = (int)seL4_GetMR(1);
            long foff = (long)seL4_GetMR(2);
            int pl = (int)seL4_GetMR(3);
            char fpath[128];
            int fi = 0;
            for (int m = 4; fi < pl && fi < 127; m++) {
                seL4_Word w = seL4_GetMR(m);
                for (int b = 0; b < 8 && fi < pl && fi < 127; b++)
                    fpath[fi++] = (char)((w >> (b * 8)) & 0xFF);
            }
            fpath[fi] = 0;
            if (ci < 0 || ci >= MAX_ACTIVE_PROCS || !active_procs[ci].active
                || len <= 0 || fi == 0 || (va & (PAGE_SIZE - 1)) != 0) {
                seL4_SetMR(0, (seL4_Word)-22 /* -EINVAL */);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int npages = (len + (int)PAGE_SIZE - 1) / (int)PAGE_SIZE;
            int written = 0;
            for (int p = 0; p < npages; p++) {
                uintptr_t cva = va + (uintptr_t)p * PAGE_SIZE;
                int pg_len = len - p * (int)PAGE_SIZE;
                if (pg_len > (int)PAGE_SIZE) pg_len = (int)PAGE_SIZE;
                file_bounce_t fb = file_bounce_map(ci, cva);
                if (!fb.ok) continue;
                int wr = vfs_pwrite(fpath, (int)(foff + (long)p * PAGE_SIZE),
                                    (char *)fb.rptr, pg_len);
                if (wr > 0) written += wr;
                file_bounce_unmap(&fb);
            }
            seL4_SetMR(0, 0);
            seL4_SetMR(1, (seL4_Word)written);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
            break;
        }
        case PIPE_DUP_REFS: {
            /* v0.4.85: child increments refs for inherited pipe FDs
             * MR0 = pipe_id, MR1 = flags (bit0=read, bit1=write) */
            int pi = (int)seL4_GetMR(0);
            int flags = (int)seL4_GetMR(1);
            if (pi >= 0 && pi < MAX_PIPES && pipes[pi].active) {
                if (flags & 1) pipes[pi].read_refs++;
                if (flags & 2) pipes[pi].write_refs++;
                seL4_SetMR(0, 0);
            } else {
                seL4_SetMR(0, (seL4_Word)(-1));
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_SET_PIPES: {
            /* v0.4.67: update stdout/stdin pipe IDs in active_proc.
             * Sent by dup2 so server knows pipe assignments even
             * for forked children that never call exec. */
            int ci = (int)badge - 1;
            if (ci >= 0 && ci < MAX_ACTIVE_PROCS
                && active_procs[ci].active) {
                int new_stdout = (int)seL4_GetMR(0);
                int new_stdin  = (int)seL4_GetMR(1);

                if (new_stdout >= -1)
                    active_procs[ci].stdout_pipe_id = new_stdout;
                if (new_stdin >= -1)
                    active_procs[ci].stdin_pipe_id = new_stdin;
                seL4_SetMR(0, 0);
            } else {
                seL4_SetMR(0, (seL4_Word)-1);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_SET_IDENTITY: {
            /* Set the CALLER's own uid/gid in active_procs (badge-indexed, so a
             * process can only change its own slot). active_procs[].uid is the
             * authoritative identity for authz (fs writes, shutdown), so this
             * MUST be gated: only a currently-root caller may change identity.
             * Otherwise any process could send PIPE_SET_IDENTITY(0) to become
             * root (v0.4.190 privesc fix). getty drops privilege in its forked
             * child (root at fork) before exec; sshd is root when it auths. A
             * non-root caller is denied -- it cannot raise OR change its uid. */
            int ci = (int)badge - 1;
            if (ci >= 0 && ci < MAX_ACTIVE_PROCS && active_procs[ci].active
                && active_procs[ci].uid == 0) {
                active_procs[ci].uid = (uint32_t)seL4_GetMR(0);
                active_procs[ci].gid = (uint32_t)seL4_GetMR(1);
                seL4_SetMR(0, 0);
            } else {
                seL4_SetMR(0, (seL4_Word)-1);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_SIGNAL: {
            /* kill(pid, sig) -- POSIX signal delivery
             * MR0 = target pid, MR1 = signal number
             * SIGKILL(9)/SIGTERM(15): immediate process destruction
             * sig 0: existence check only (POSIX)
             * Others: set pending bit in target active_proc */
            int target_pid = (int)seL4_GetMR(0);
            int signum     = (int)seL4_GetMR(1);

            /* v0.4.87: POSIX kill(0, sig) targets foreground process */
            if (target_pid == 0 && fg_pid > 0)
                target_pid = fg_pid;

            /* Find target process */
            int ti = -1;
            for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
                if (active_procs[i].active && active_procs[i].pid == target_pid) {
                    ti = i;
                    break;
                }
            }

            if (ti < 0) {
                /* pid not found */
                seL4_SetMR(0, (seL4_Word)(-3)); /* -ESRCH */
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            if (signum == 0) {
                /* Existence check only */
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            if (signum == 9 || signum == 15) {
                /* SIGKILL / SIGTERM: destroy via reap path so
                 * zombie is created and waitpid unblocks.
                 * POSIX wait status: signal death = raw signum. */
                active_procs[ti].exit_status = signum;
                handle_child_fault(ti);
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            /* Other signals: set pending bit */
            if (signum >= 1 && signum < 32) {
                active_procs[ti].sig_pending |= (1U << (signum - 1));
                /* v0.4.87: wake process if blocked in PIPE_READ,
                 * so it can observe the signal and return EINTR */
                wake_blocked_reader_signal(target_pid);
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_DEBUG: {
            /* Dump active_procs and pipe status via serial */
            printf("[DEBUG] active_procs:\n");
            for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
                if (active_procs[i].active) {
                    printf("  [%d] pid=%d ppid=%d fope=%d spipe=%d\n",
                        i, active_procs[i].pid, active_procs[i].ppid,
                        active_procs[i].fault_on_pipe_ep,
                        active_procs[i].stdout_pipe_id);
                }
            }
            printf("[DEBUG] pipes:\n");
            for (int i = 0; i < MAX_PIPES; i++) {
                if (pipes[i].active) {
                    printf("  [%d] count=%d wc=%d rc=%d wr=%d rr=%d shm=%d\n",
                        i, pipes[i].count,
                        pipes[i].write_closed, pipes[i].read_closed,
                        pipes[i].write_refs, pipes[i].read_refs,
                        pipes[i].shm_valid);
                }
            }
            vka_audit_dump();
            printf("[DEBUG] wait_pending:\n");
            for (int i = 0; i < MAX_WAIT_PENDING; i++) {
                if (wait_pending[i].active) {
                    printf("  [%d] waiter=%d child=%d\n",
                        i, wait_pending[i].waiting_pid,
                        wait_pending[i].child_pid);
                }
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_SIG_FETCH: {
            /* Return pending signal mask for caller, then clear it.
             * Client merges into local sigstate and dispatches handlers. */
            int caller_idx = (int)badge - 1;
            uint32_t pending = 0;
            if (caller_idx >= 0 && caller_idx < MAX_ACTIVE_PROCS
                && active_procs[caller_idx].active) {
                pending = active_procs[caller_idx].sig_pending;
                active_procs[caller_idx].sig_pending = 0;
            }
            seL4_SetMR(0, (seL4_Word)pending);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_GET_TIME: {
            /* v0.4.166: return the wall-clock epoch offset (seconds). */
            seL4_SetMR(0, (seL4_Word)aios_wall_offset_sec);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_SET_TIME: {
            /* v0.4.166: set the wall-clock epoch offset (MR0). Read it before
             * the reply overwrites the message registers. */
            aios_wall_offset_sec = (long)seL4_GetMR(0);
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_SHUTDOWN: {
            /* MR0 = reboot flag (1 = reboot via watchdog, 0 = halt). Capture it
             * before the reply call overwrites the message registers. */
            int do_reboot = (int)seL4_GetMR(0);
            /* Only root (badge 0 or uid 0) can shut down */
            int shut_idx = (int)badge - 1;
            int allowed = (badge == 0);
            if (!allowed && shut_idx >= 0 && shut_idx < MAX_ACTIVE_PROCS
                && active_procs[shut_idx].active
                && active_procs[shut_idx].uid == 0) {
                allowed = 1;
            }
            if (!allowed) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            if (do_reboot) {
                printf("[shutdown] AIOS rebooting...\n");
                aios_system_reboot();
            } else {
                printf("[shutdown] AIOS powering off...\n");
                aios_system_shutdown();
            }
            break;
        }
        default:
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
    }
}
