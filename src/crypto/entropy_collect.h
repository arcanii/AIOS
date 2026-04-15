#ifndef ENTROPY_COLLECT_H
#define ENTROPY_COLLECT_H

#include <stdint.h>
#include <stddef.h>
#include "crypto_chacha20.h"

/* Collect entropy by sampling ARM generic timer jitter.
 * Writes up to len bytes into buf.  Returns bytes written. */
size_t entropy_collect_jitter(uint8_t *buf, size_t len);

/* Feed IPC arrival timing into the CSPRNG entropy pool.
 * Call this on every incoming IPC message in the server loop. */
void entropy_feed_ipc_timing(chacha20_csprng_t *ctx);

/* Reset the IPC timing state (call once at server startup) */
void entropy_ipc_reset(void);

#endif /* ENTROPY_COLLECT_H */
