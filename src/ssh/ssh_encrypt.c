/* ssh_encrypt.c -- AES-256-CTR encryption + HMAC-SHA-256 integrity
 *
 * Symmetric crypto for SSH encrypted transport (Phase 3).
 * Uses mbedTLS AES and SHA-256 primitives.
 *
 * RFC 4253 section 6 (binary packet protocol)
 * RFC 4344 (AES-CTR mode for SSH)
 * RFC 2104 (HMAC)
 */

#include "ssh_session.h"
#include <stdio.h>
#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"

/* ----------------------------------------------------------------
 * Cipher state for one direction (AES-256-CTR stream)
 * ---------------------------------------------------------------- */

typedef struct {
    mbedtls_aes_context  aes;
    uint8_t              nonce_counter[16];
    uint8_t              stream_block[16];
    size_t               nc_off;
} ssh_cipher_ctx_t;

/* Single-connection server: one context per direction */
static ssh_cipher_ctx_t  g_recv_cipher;   /* client-to-server (decrypt) */
static ssh_cipher_ctx_t  g_send_cipher;   /* server-to-client (encrypt) */

/* MAC keys */
static uint8_t g_mac_recv_key[32];   /* client-to-server integrity */
static uint8_t g_mac_send_key[32];   /* server-to-client integrity */

/* ----------------------------------------------------------------
 * Initialize cipher contexts from derived session keys
 *
 * Called after key exchange completes.  Sets s->encrypted = 1.
 * ---------------------------------------------------------------- */

int ssh_encrypt_init(ssh_session_t *s)
{
    int ret;

    /* Receive cipher (c2s): decrypt incoming packets */
    mbedtls_aes_init(&g_recv_cipher.aes);
    ret = mbedtls_aes_setkey_enc(&g_recv_cipher.aes, s->key_c2s, 256);
    if (ret != 0) {
        printf("[sshd] AES recv key setup: -0x%04x\n", (unsigned)-ret);
        return -1;
    }
    memcpy(g_recv_cipher.nonce_counter, s->iv_c2s, 16);
    memset(g_recv_cipher.stream_block, 0, 16);
    g_recv_cipher.nc_off = 0;

    /* Send cipher (s2c): encrypt outgoing packets */
    mbedtls_aes_init(&g_send_cipher.aes);
    ret = mbedtls_aes_setkey_enc(&g_send_cipher.aes, s->key_s2c, 256);
    if (ret != 0) {
        printf("[sshd] AES send key setup: -0x%04x\n", (unsigned)-ret);
        return -1;
    }
    memcpy(g_send_cipher.nonce_counter, s->iv_s2c, 16);
    memset(g_send_cipher.stream_block, 0, 16);
    g_send_cipher.nc_off = 0;

    /* MAC keys */
    memcpy(g_mac_recv_key, s->mac_c2s, 32);
    memcpy(g_mac_send_key, s->mac_s2c, 32);

    /* Mark session as encrypted */
    s->encrypted = 1;

    printf("[sshd] Encrypt init OK (AES-256-CTR + HMAC-SHA-256)\n");
    return 0;
}

/* ----------------------------------------------------------------
 * AES-256-CTR encrypt / decrypt
 *
 * CTR mode uses the same operation for both directions.
 * The nonce_counter and stream_block persist across calls,
 * maintaining the stream position across packets.
 * ---------------------------------------------------------------- */

void ssh_ctr_encrypt(const uint8_t *in, uint8_t *out, int len)
{
    mbedtls_aes_crypt_ctr(&g_send_cipher.aes, (size_t)len,
                          &g_send_cipher.nc_off,
                          g_send_cipher.nonce_counter,
                          g_send_cipher.stream_block,
                          in, out);
}

void ssh_ctr_decrypt(const uint8_t *in, uint8_t *out, int len)
{
    mbedtls_aes_crypt_ctr(&g_recv_cipher.aes, (size_t)len,
                          &g_recv_cipher.nc_off,
                          g_recv_cipher.nonce_counter,
                          g_recv_cipher.stream_block,
                          in, out);
}

/* ----------------------------------------------------------------
 * HMAC-SHA-256 (RFC 2104)
 *
 * Implemented directly with mbedTLS SHA-256.
 * SHA-256 block size = 64 bytes, output = 32 bytes.
 * ---------------------------------------------------------------- */

static void hmac_sha256(const uint8_t key[32],
                         const uint8_t *data, int datalen,
                         uint8_t mac_out[32])
{
    uint8_t k_ipad[64], k_opad[64];
    int i;

    /* Key is 32 bytes, pad to block size (64), XOR with ipad/opad */
    for (i = 0; i < 32; i++) {
        k_ipad[i] = key[i] ^ 0x36;
        k_opad[i] = key[i] ^ 0x5c;
    }
    for (i = 32; i < 64; i++) {
        k_ipad[i] = 0x36;
        k_opad[i] = 0x5c;
    }

    /* Inner hash: SHA-256(k_ipad || data) */
    uint8_t inner[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, k_ipad, 64);
    mbedtls_sha256_update(&sha, data, (size_t)datalen);
    mbedtls_sha256_finish(&sha, inner);
    mbedtls_sha256_free(&sha);

    /* Outer hash: SHA-256(k_opad || inner_hash) */
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, k_opad, 64);
    mbedtls_sha256_update(&sha, inner, 32);
    mbedtls_sha256_finish(&sha, mac_out);
    mbedtls_sha256_free(&sha);
}

/* ----------------------------------------------------------------
 * MAC compute and verify for each direction
 * ---------------------------------------------------------------- */

void ssh_mac_compute_send(const uint8_t *data, int len, uint8_t mac[32])
{
    hmac_sha256(g_mac_send_key, data, len, mac);
}

int ssh_mac_verify_recv(const uint8_t *data, int len,
                         const uint8_t expected[32])
{
    uint8_t computed[32];
    hmac_sha256(g_mac_recv_key, data, len, computed);

    /* Constant-time comparison */
    int diff = 0;
    int i;
    for (i = 0; i < 32; i++) {
        diff |= computed[i] ^ expected[i];
    }
    return diff ? -1 : 0;
}
