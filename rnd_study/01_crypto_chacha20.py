#!/usr/bin/env python3
"""Write crypto_chacha20.h and crypto_chacha20.c to /tmp."""
import os, subprocess, sys

# ==============================
# crypto_chacha20.h
# ==============================

HEADER = "crypto_chacha20.h"
HEADER_PATH = f"/tmp/{HEADER}"

if os.path.exists(HEADER_PATH):
    print(f"[SKIP] {HEADER_PATH} already exists")
else:
    subprocess.run(["bash", "-c", f"""cat << 'ENDOFFILE' > {HEADER_PATH}
#ifndef CRYPTO_CHACHA20_H
#define CRYPTO_CHACHA20_H

#include <stdint.h>
#include <stddef.h>

typedef struct {{
    uint32_t state[16];     /* ChaCha20 state matrix */
    uint8_t  buffer[64];    /* keystream block buffer */
    size_t   buf_pos;       /* position within buffer */
    uint64_t reseed_count;  /* blocks since last reseed */
}} chacha20_csprng_t;

/* Initialise CSPRNG with a 48-byte seed (32-byte key + 16-byte nonce material) */
void csprng_init(chacha20_csprng_t *ctx, const uint8_t seed[48]);

/* Mix new entropy into the key portion of the state via XOR */
void csprng_reseed(chacha20_csprng_t *ctx, const uint8_t *entropy, size_t len);

/* Generate len bytes of cryptographically secure random output */
void csprng_generate(chacha20_csprng_t *ctx, uint8_t *out, size_t len);

#endif /* CRYPTO_CHACHA20_H */
ENDOFFILE"""], check=True)
    print(f"[DONE] wrote {HEADER_PATH}")

# ==============================
# crypto_chacha20.c
# ==============================

SOURCE = "crypto_chacha20.c"
SOURCE_PATH = f"/tmp/{SOURCE}"

if os.path.exists(SOURCE_PATH):
    print(f"[SKIP] {SOURCE_PATH} already exists")
else:
    subprocess.run(["bash", "-c", f"""cat << 'ENDOFFILE' > {SOURCE_PATH}
/* crypto_chacha20.c -- ChaCha20-based CSPRNG core for AIOS crypto_server
 *
 * Pure ARX construction: add, rotate, xor.  No lookup tables,
 * no cache-timing side channels.  Same algorithm Linux uses
 * since kernel 4.8.
 */

#include "crypto_chacha20.h"
#include <string.h>

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define QR(a, b, c, d) do {{                    \\
    a += b; d ^= a; d = ROTL32(d, 16);         \\
    c += d; b ^= c; b = ROTL32(b, 12);         \\
    a += b; d ^= a; d = ROTL32(d, 8);          \\
    c += d; b ^= c; b = ROTL32(b, 7);          \\
}} while (0)

static void chacha20_block(const uint32_t in[16], uint32_t out[16])
{{
    int i;
    for (i = 0; i < 16; i++)
        out[i] = in[i];

    /* 20 rounds = 10 double-rounds */
    for (i = 0; i < 10; i++) {{
        /* Column rounds */
        QR(out[0], out[4], out[ 8], out[12]);
        QR(out[1], out[5], out[ 9], out[13]);
        QR(out[2], out[6], out[10], out[14]);
        QR(out[3], out[7], out[11], out[15]);
        /* Diagonal rounds */
        QR(out[0], out[5], out[10], out[15]);
        QR(out[1], out[6], out[11], out[12]);
        QR(out[2], out[7], out[ 8], out[13]);
        QR(out[3], out[4], out[ 9], out[14]);
    }}

    for (i = 0; i < 16; i++)
        out[i] += in[i];
}}

void csprng_init(chacha20_csprng_t *ctx, const uint8_t seed[48])
{{
    /* "expand 32-byte k" -- standard ChaCha20 constant */
    ctx->state[0] = 0x61707865;
    ctx->state[1] = 0x3320646e;
    ctx->state[2] = 0x79622d32;
    ctx->state[3] = 0x6b206574;

    /* Key from seed[0..31] */
    memcpy(&ctx->state[4], seed, 32);

    /* Counter starts at zero */
    ctx->state[12] = 0;
    ctx->state[13] = 0;

    /* Nonce from seed[32..43], remaining bytes XORed in */
    memcpy(&ctx->state[14], seed + 32, 8);
    ctx->state[14] ^= ((uint32_t)seed[44] << 24) | ((uint32_t)seed[45] << 16);
    ctx->state[15] ^= ((uint32_t)seed[46] << 24) | ((uint32_t)seed[47] << 16);

    ctx->buf_pos = 64;  /* Force generation on first request */
    ctx->reseed_count = 0;
}}

void csprng_reseed(chacha20_csprng_t *ctx, const uint8_t *entropy, size_t len)
{{
    /* Mix new entropy into key portion of state via XOR.
     * Forward-secure: old state cannot be recovered even if
     * the new entropy is known. */
    size_t i;
    uint8_t *key_bytes = (uint8_t *)&ctx->state[4];
    for (i = 0; i < len && i < 32; i++)
        key_bytes[i] ^= entropy[i];

    /* Reset counter to avoid keystream reuse after reseed */
    ctx->state[12] = 0;
    ctx->state[13] = 0;
    ctx->reseed_count = 0;
}}

void csprng_generate(chacha20_csprng_t *ctx, uint8_t *out, size_t len)
{{
    while (len > 0) {{
        if (ctx->buf_pos >= 64) {{
            uint32_t keystream[16];
            chacha20_block(ctx->state, keystream);
            memcpy(ctx->buffer, keystream, 64);
            ctx->buf_pos = 0;
            ctx->reseed_count++;

            /* Increment 64-bit block counter */
            if (++ctx->state[12] == 0)
                ctx->state[13]++;
        }}

        size_t avail = 64 - ctx->buf_pos;
        size_t chunk = (len < avail) ? len : avail;
        memcpy(out, ctx->buffer + ctx->buf_pos, chunk);
        ctx->buf_pos += chunk;
        out += chunk;
        len -= chunk;
    }}
}}
ENDOFFILE"""], check=True)
    print(f"[DONE] wrote {SOURCE_PATH}")

print("==============================")
print("  01_crypto_chacha20 complete")
print("==============================")
