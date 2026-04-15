#ifndef CRYPTO_CHACHA20_H
#define CRYPTO_CHACHA20_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[16];     /* ChaCha20 state matrix */
    uint8_t  buffer[64];    /* keystream block buffer */
    size_t   buf_pos;       /* position within buffer */
    uint64_t reseed_count;  /* blocks since last reseed */
} chacha20_csprng_t;

/* Initialise CSPRNG with a 48-byte seed (32-byte key + 16-byte nonce material) */
void csprng_init(chacha20_csprng_t *ctx, const uint8_t seed[48]);

/* Mix new entropy into the key portion of the state via XOR */
void csprng_reseed(chacha20_csprng_t *ctx, const uint8_t *entropy, size_t len);

/* Generate len bytes of cryptographically secure random output */
void csprng_generate(chacha20_csprng_t *ctx, uint8_t *out, size_t len);

#endif /* CRYPTO_CHACHA20_H */
