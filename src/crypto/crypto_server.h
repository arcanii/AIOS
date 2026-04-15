#ifndef CRYPTO_SERVER_H
#define CRYPTO_SERVER_H

#include <sel4/sel4.h>

/* IPC opcodes for the crypto server */
#define CRYPTO_OP_RANDOM    1   /* Request random bytes */
#define CRYPTO_OP_RESEED    2   /* Inject external entropy */
#define CRYPTO_OP_STATUS    3   /* Query server status */

/* Status reply fields (returned in MRs) */
#define CRYPTO_STATUS_RESEED_COUNT  0   /* MR0: total reseed count */
#define CRYPTO_STATUS_BLOCKS_GEN    1   /* MR1: blocks generated since last reseed */

/* Reseed after this many 64-byte blocks have been generated.
 * 4096 blocks = 256 KiB of output between reseeds. */
#define CRYPTO_RESEED_INTERVAL  4096

/* Maximum bytes per single CRYPTO_OP_RANDOM request.
 * Limited by seL4 MR capacity (seL4_MsgMaxLength words). */
#define CRYPTO_MAX_RANDOM_BYTES  (seL4_MsgMaxLength * sizeof(seL4_Word))

/* Entry point -- does not return */
void crypto_server_main(void *arg0, void *arg1, void *ipc_buf);

#endif /* CRYPTO_SERVER_H */
