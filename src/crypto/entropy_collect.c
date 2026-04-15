/* entropy_collect.c -- Entropy collection for AIOS crypto_server
 *
 * Primary source: ARM generic timer (CNTPCT_EL0) jitter.
 * Secondary source: IPC message arrival timing deltas.
 *
 * On QEMU the jitter quality is lower than bare metal, but
 * the LSBs still vary due to instruction timing variability.
 * For higher quality, add virtio-rng to the QEMU command line:
 *   -device virtio-rng-device
 * and feed its output via CRYPTO_OP_RESEED.
 */

#include "entropy_collect.h"
#include <string.h>

/* ---- ARM generic timer access ---- */

static inline uint64_t read_cntpct(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(val));
    return val;
}

/* ---- Jitter-based collection ---- */

size_t entropy_collect_jitter(uint8_t *buf, size_t len)
{
    size_t collected = 0;
    uint64_t prev = read_cntpct();

    /* Zero the output buffer so XOR accumulation is clean */
    memset(buf, 0, len);

    while (collected < len) {
        /* Busywork loop to create timing variation.
         * The volatile qualifier prevents the compiler from
         * optimising this away. */
        volatile uint32_t dummy = 0;
        for (volatile int i = 0; i < 100; i++)
            dummy += i * i;
        (void)dummy;

        uint64_t now = read_cntpct();
        uint64_t delta = now - prev;
        prev = now;

        /* Low bits of the delta carry the jitter.
         * We XOR them into the output buffer one byte at a time.
         * Multiple passes over the same byte improve quality. */
        buf[collected] ^= (uint8_t)(delta & 0xFF);
        collected++;
    }

    return collected;
}

/* ---- IPC timing entropy ---- */

static uint64_t ipc_last_time = 0;
static uint8_t  ipc_accum     = 0;
static int      ipc_bits      = 0;

void entropy_ipc_reset(void)
{
    ipc_last_time = read_cntpct();
    ipc_accum     = 0;
    ipc_bits      = 0;
}

void entropy_feed_ipc_timing(chacha20_csprng_t *ctx)
{
    uint64_t now   = read_cntpct();
    uint64_t delta = now - ipc_last_time;
    ipc_last_time  = now;

    /* Accumulate the LSB of each inter-message delta.
     * After 8 bits, feed the byte into the CSPRNG. */
    ipc_accum = (uint8_t)((ipc_accum << 1) | (delta & 1));
    ipc_bits++;

    if (ipc_bits >= 8) {
        csprng_reseed(ctx, &ipc_accum, 1);
        ipc_accum = 0;
        ipc_bits  = 0;
    }
}
