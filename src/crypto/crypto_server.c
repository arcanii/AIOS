/* crypto_server.c -- AIOS Cryptographic Services Server
 *
 * Provides a CSPRNG service to the rest of the system via seL4 IPC.
 * Backs /dev/urandom reads and getrandom() syscalls.
 *
 * Architecture:
 *   - ChaCha20-based CSPRNG (same as Linux 4.8+)
 *   - Entropy from ARM timer jitter + IPC timing deltas
 *   - Auto-reseed every CRYPTO_RESEED_INTERVAL blocks
 *   - Forward-secure: reseed XORs into key, old state unrecoverable
 *
 * IPC protocol:
 *   CRYPTO_OP_RANDOM:  MR0=opcode, MR1=nbytes
 *                      Reply: MR0..MRn = random data
 *   CRYPTO_OP_RESEED:  MR0=opcode, MR1..MR4 = entropy bytes
 *                      Reply: (empty, acknowledgement)
 *   CRYPTO_OP_STATUS:  MR0=opcode
 *                      Reply: MR0=total_reseeds, MR1=blocks_since_reseed
 */

#include <sel4/sel4.h>
#include "crypto_server.h"
#include "crypto_chacha20.h"
#include "entropy_collect.h"

#include <stdio.h>
#define LOG_MODULE "crypto"
#define LOG_LEVEL LOG_LEVEL_DEBUG
#include "aios/aios_log.h"

static chacha20_csprng_t g_csprng;
static uint64_t          g_total_reseeds = 0;

static void do_auto_reseed(void)
{
    uint8_t fresh[32];
    entropy_collect_jitter(fresh, 32);
    csprng_reseed(&g_csprng, fresh, 32);
    g_total_reseeds++;
}

static seL4_MessageInfo_t handle_random(seL4_MessageInfo_t info)
{
    seL4_Word nbytes = seL4_GetMR(1);

    /* Clamp to MR capacity */
    if (nbytes > CRYPTO_MAX_RANDOM_BYTES)
        nbytes = CRYPTO_MAX_RANDOM_BYTES;
    if (nbytes == 0)
        nbytes = sizeof(seL4_Word);  /* Minimum: one word */

    /* Auto-reseed if threshold reached */
    if (g_csprng.reseed_count >= CRYPTO_RESEED_INTERVAL)
        do_auto_reseed();

    /* Generate random bytes into a local buffer */
    uint8_t buf[CRYPTO_MAX_RANDOM_BYTES];
    csprng_generate(&g_csprng, buf, nbytes);

    /* Pack into message registers */
    seL4_Word *words = (seL4_Word *)buf;
    size_t nwords = (nbytes + sizeof(seL4_Word) - 1) / sizeof(seL4_Word);
    for (size_t i = 0; i < nwords; i++)
        seL4_SetMR(i, words[i]);

    return seL4_MessageInfo_new(0, 0, 0, (seL4_Uint64)nwords);
}

static seL4_MessageInfo_t handle_reseed(seL4_MessageInfo_t info)
{
    /* Read external entropy from MRs 1..N */
    size_t nwords = seL4_MessageInfo_get_length(info);
    if (nwords > 5) nwords = 5;  /* MR0 is opcode, MR1..MR4 = 32 bytes max */

    uint8_t ext[32];
    size_t ext_len = 0;
    for (size_t i = 1; i < nwords && ext_len < 32; i++) {
        seL4_Word w = seL4_GetMR(i);
        size_t remain = 32 - ext_len;
        size_t chunk = sizeof(seL4_Word);
        if (chunk > remain) chunk = remain;
        __builtin_memcpy(ext + ext_len, &w, chunk);
        ext_len += chunk;
    }

    if (ext_len > 0) {
        csprng_reseed(&g_csprng, ext, ext_len);
        g_total_reseeds++;
    }

    return seL4_MessageInfo_new(0, 0, 0, 0);
}

static seL4_MessageInfo_t handle_status(void)
{
    seL4_SetMR(0, (seL4_Word)g_total_reseeds);
    seL4_SetMR(1, (seL4_Word)g_csprng.reseed_count);
    return seL4_MessageInfo_new(0, 0, 0, 2);
}

void crypto_server_main(void *arg0, void *arg1, void *ipc_buf)
{
    (void)arg1; (void)ipc_buf;
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    /* Collect initial seed from timer jitter -- 48 bytes */
    uint8_t seed[48];
    entropy_collect_jitter(seed, 48);
    csprng_init(&g_csprng, seed);

    /* Initialise IPC timing entropy state */
    entropy_ipc_reset();

    printf("[crypto] server started, initial seed collected\n");

    seL4_Word badge;
    seL4_MessageInfo_t info = seL4_Recv(ep, &badge);

    for (;;) {
        /* Feed IPC arrival timing into the entropy pool */
        entropy_feed_ipc_timing(&g_csprng);

        seL4_Word op = seL4_GetMR(0);
        seL4_MessageInfo_t reply;

        switch (op) {
        case CRYPTO_OP_RANDOM:
            reply = handle_random(info);
            break;

        case CRYPTO_OP_RESEED:
            reply = handle_reseed(info);
            break;

        case CRYPTO_OP_STATUS:
            reply = handle_status();
            break;

        default:
            /* Unknown opcode -- return empty reply */
            reply = seL4_MessageInfo_new(0, 0, 0, 0);
            break;
        }

        info = seL4_ReplyRecv(ep, reply, &badge);
    }
}
