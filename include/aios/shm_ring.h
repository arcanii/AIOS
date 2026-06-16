#ifndef AIOS_SHM_RING_H
#define AIOS_SHM_RING_H

/*
 * shm_ring.h -- direct SPSC shared-memory ring for AIOS pipes (v0.4.258)
 *
 * A ring-mode pipe lives in ONE 4 KB frame that the writer AND the reader both
 * map (each in its own vspace), plus the root pipe_server (for setup/teardown
 * and the rare server-mediated fallback). The header sits at offset 0; a
 * power-of-two byte ring follows at SHM_RING_DATAOFF.
 *
 * Data flows entirely in USERSPACE on the producer/consumer cores: the writer
 * copies bytes into the ring and advances `tail`; the reader copies them out
 * and advances `head`. The pipe_server (core 0) is touched only at the
 * empty/full transitions -- a reader that finds the ring empty blocks via a
 * seL4_Call (reusing PIPE_READ_SHM); a writer that fills it wakes a parked
 * reader via PIPE_RING_WAKE. A steadily-flowing pipe does ~zero syscalls for
 * the data, so `cat (core 1) | wc (core 2)` truly parallelizes instead of
 * serializing every chunk through the core-0 server + seL4 big kernel lock.
 *
 * SINGLE-PRODUCER SINGLE-CONSUMER: exactly one entity advances `tail` (the
 * writer's userspace, OR the server on its behalf if the writer never mapped
 * the ring -- never both at once) and exactly one advances `head`. Because each
 * index has a single writer, no atomics are needed -- only barriers.
 *
 * head/tail are FREE-RUNNING uint32 counters (NOT pre-masked); the byte index
 * is (counter & SHM_RING_MASK). used = tail - head (mod 2^32, correct as long
 * as the producer never runs more than 2^32 bytes ahead -- impossible with a
 * <=64 KB ring). space = SHM_RING_DATA - used. SHM_RING_DATA MUST be a power of
 * two for the mask trick.
 *
 * CROSS-CORE COHERENCY (the #1 risk; QEMU CANNOT model it -- see
 * [[feedback_pipe_shm_cache]]): with /proc/coresched.1 the writer and reader run
 * on different Cortex-A72 cores. The frame is mapped CACHEABLE INNER-SHAREABLE
 * on every end. Publishing data needs a store-release (the data stores must be
 * globally visible BEFORE the tail bump that advertises them); observing data
 * needs a load-acquire (read the index, THEN the bytes it covers). The
 * block/wake handshake needs a full StoreLoad barrier on both sides so a wakeup
 * is never lost. We use explicit dmb barriers + `volatile` index fields rather
 * than relying on the compiler's atomics.
 */

#include <stdint.h>
#include <stddef.h>

#define SHM_RING_MAGIC    0x52494e47u          /* 'RING' */
#define SHM_RING_DATAOFF  128u                  /* header bytes: two 64-B cache lines */
#define SHM_RING_DATA     2048u                 /* data ring bytes -- MUST be a power of two */
#define SHM_RING_MASK     (SHM_RING_DATA - 1u)
#define SHM_RING_BYTES    (SHM_RING_DATAOFF + SHM_RING_DATA)   /* total used in the page */

/*
 * head and tail live in SEPARATE 64-byte cache lines so the writer's tail store
 * and the reader's head store do not ping-pong one shared line across cores
 * (false sharing). Fields read by the peer (writer_closed/reader_closed,
 * *_waiting) sit next to the index THEIR owner writes.
 */
typedef struct {
    /* ---- writer cache line (line 0) ---- */
    volatile uint32_t tail;            /* writer-owned: total bytes ever written */
    volatile uint32_t writer_waiting;  /* reserved: writer true-block (legacy writer parks server-side) */
    volatile uint32_t writer_closed;   /* writer end gone -> reader sees EOF after draining */
    uint32_t          magic;           /* SHM_RING_MAGIC (set once at init) */
    uint32_t          size;            /* SHM_RING_DATA (set once at init) */
    uint32_t          _pad0[11];       /* pad to 64 bytes */
    /* ---- reader cache line (line 1) ---- */
    volatile uint32_t head;            /* reader-owned: total bytes ever consumed */
    volatile uint32_t reader_waiting;  /* reader parked in the server (server-managed flag) */
    volatile uint32_t reader_closed;   /* reader end gone -> writer sees EPIPE */
    uint32_t          _pad1[13];       /* pad to 64 bytes */
} shm_ring_hdr_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(shm_ring_hdr_t) == SHM_RING_DATAOFF, "shm_ring_hdr_t must be 128 bytes");
_Static_assert((SHM_RING_DATA & (SHM_RING_DATA - 1u)) == 0u, "SHM_RING_DATA must be a power of two");
_Static_assert(SHM_RING_BYTES <= 4096u, "ring header + data must fit one 4 KB page");
#endif

/* ---- A72 inner-shareable barriers ---- */

/* Store-release: order prior data stores BEFORE the following index store, so a
 * consumer that observes the new index also observes the bytes it covers. */
static inline void shm_ring_publish(void) { __asm__ volatile("dmb ishst" ::: "memory"); }

/* Load-acquire: order the prior index load BEFORE the following data loads, so
 * the bytes read are at least as new as the index that authorized reading them. */
static inline void shm_ring_observe(void) { __asm__ volatile("dmb ishld" ::: "memory"); }

/* Full StoreLoad barrier for the block/wake (Dekker) handshake: the producer
 * publishes tail THEN reads reader_waiting; the consumer (or the server on its
 * behalf) sets reader_waiting THEN re-reads tail. The full barrier guarantees at
 * least one side sees the other's store -> no lost wakeup. */
static inline void shm_ring_fence(void) { __asm__ volatile("dmb ish" ::: "memory"); }

static inline char *shm_ring_data(void *ring) { return (char *)ring + SHM_RING_DATAOFF; }
static inline uint32_t shm_ring_used(uint32_t tail, uint32_t head)  { return tail - head; }
static inline uint32_t shm_ring_space(uint32_t tail, uint32_t head) { return SHM_RING_DATA - (tail - head); }

#endif /* AIOS_SHM_RING_H */
