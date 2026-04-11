/* ssh_crypto.c -- mbedTLS crypto wrappers for AIOS SSH server
 *
 * CTR-DRBG random, ECDSA-P256 host key, x25519 ECDH key exchange,
 * exchange hash computation, SSH wire format encoding.
 */

#include "ssh_session.h"
#include <stdio.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"
#include "mbedtls/bignum.h"

/* ----------------------------------------------------------------
 * Global crypto state (single-connection server)
 * ---------------------------------------------------------------- */

static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_ecdsa_context    g_hostkey;

/* Ephemeral x25519 state (per key exchange) */
static mbedtls_ecp_group  g_ecdh_grp;
static mbedtls_mpi        g_ecdh_d;      /* our private scalar */
static mbedtls_ecp_point  g_ecdh_Q;      /* our public point */

/* ----------------------------------------------------------------
 * Init: seed DRBG, generate host key
 * ---------------------------------------------------------------- */

int ssh_crypto_init(void)
{
    int ret;

    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);

    ret = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func,
                                 &g_entropy,
                                 (const uint8_t *)"aios-sshd", 9);
    if (ret != 0) {
        printf("[sshd] DRBG seed failed: -0x%04x\n", (unsigned)-ret);
        return -1;
    }

    /* Generate ECDSA P-256 host key (in memory, regenerated each boot) */
    mbedtls_ecdsa_init(&g_hostkey);
    ret = mbedtls_ecdsa_genkey(&g_hostkey, MBEDTLS_ECP_DP_SECP256R1,
                                mbedtls_ctr_drbg_random, &g_drbg);
    if (ret != 0) {
        printf("[sshd] Host key gen failed: -0x%04x\n", (unsigned)-ret);
        return -1;
    }

    printf("[sshd] Crypto init OK (ECDSA-P256 host key generated)\n");
    return 0;
}

/* ----------------------------------------------------------------
 * Random bytes via CTR-DRBG
 * ---------------------------------------------------------------- */

void ssh_random_bytes(uint8_t *buf, int len)
{
    mbedtls_ctr_drbg_random(&g_drbg, buf, (size_t)len);
}

/* ----------------------------------------------------------------
 * Host key: export public key blob in SSH wire format
 *
 * Format (ecdsa-sha2-nistp256):
 *   string  "ecdsa-sha2-nistp256"
 *   string  "nistp256"
 *   string  Q  (0x04 || x[32] || y[32] = 65 bytes)
 * ---------------------------------------------------------------- */

int ssh_hostkey_get_blob(uint8_t *buf, int *len)
{
    int off = 0;

    ssh_put_namelist(buf, "ecdsa-sha2-nistp256", &off);
    ssh_put_namelist(buf, "nistp256", &off);

    /* Export uncompressed public point */
    uint8_t point[65];
    size_t plen = 0;
    int ret = mbedtls_ecp_point_write_binary(
        &g_hostkey.grp, &g_hostkey.Q,
        MBEDTLS_ECP_PF_UNCOMPRESSED,
        &plen, point, sizeof(point));
    if (ret != 0 || plen != 65) {
        printf("[sshd] Host key export failed: %d\n", ret);
        return -1;
    }

    ssh_put_string(buf, point, (uint32_t)plen, &off);
    *len = off;
    return 0;
}

/* ----------------------------------------------------------------
 * Host key: sign hash, produce SSH signature blob
 *
 * SSH signature format (ecdsa-sha2-nistp256):
 *   string  "ecdsa-sha2-nistp256"
 *   string  inner_sig
 *
 * inner_sig:
 *   string  r  (big-endian, minimal, leading 0x00 if MSB set)
 *   string  s  (same)
 * ---------------------------------------------------------------- */

int ssh_hostkey_sign(const uint8_t *hash, int hashlen,
                      uint8_t *sigbuf, int *siglen)
{
    mbedtls_mpi r, sv;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&sv);

    /* ECDSA standard: sign SHA-256(H), not H directly.
     * OpenSSH ssh_ecdsa_sign() hashes the exchange hash H with SHA-256
     * before calling ECDSA_do_sign. We must do the same. */
    uint8_t digest[32];
    mbedtls_sha256(hash, (size_t)hashlen, digest, 0);

    int ret = mbedtls_ecdsa_sign(&g_hostkey.grp, &r, &sv, &g_hostkey.d,
                                  digest, 32,
                                  mbedtls_ctr_drbg_random, &g_drbg);
    if (ret != 0) {
        printf("[sshd] ECDSA sign failed: -0x%04x\n", (unsigned)-ret);
        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&sv);
        return -1;
    }

    /* Encode r as SSH string (big-endian, leading 0 if MSB set) */
    uint8_t rbuf[33], sbuf[33];
    size_t rlen = mbedtls_mpi_size(&r);
    size_t slen = mbedtls_mpi_size(&sv);

    mbedtls_mpi_write_binary(&r, rbuf + 1, rlen);
    mbedtls_mpi_write_binary(&sv, sbuf + 1, slen);

    /* Add leading zero byte if MSB set (positive mpint convention) */
    int r_off = 1, s_off = 1;
    if (rbuf[1] & 0x80) { rbuf[0] = 0; r_off = 0; rlen++; }
    if (sbuf[1] & 0x80) { sbuf[0] = 0; s_off = 0; slen++; }

    /* Build inner_sig: string r || string s */
    uint8_t inner[80];
    int ioff = 0;
    ssh_put_string(inner, rbuf + r_off, (uint32_t)rlen, &ioff);
    ssh_put_string(inner, sbuf + s_off, (uint32_t)slen, &ioff);

    /* Build outer: string "ecdsa-sha2-nistp256" || string inner_sig */
    int off = 0;
    ssh_put_namelist(sigbuf, "ecdsa-sha2-nistp256", &off);
    ssh_put_string(sigbuf, inner, (uint32_t)ioff, &off);

    *siglen = off;

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&sv);
    return 0;
}

/* ----------------------------------------------------------------
 * x25519 ECDH: generate ephemeral keypair
 * Returns 32-byte public key in little-endian (RFC 7748) format.
 * ---------------------------------------------------------------- */

int ssh_ecdh_generate(uint8_t pub_out[32])
{
    mbedtls_ecp_group_init(&g_ecdh_grp);
    mbedtls_mpi_init(&g_ecdh_d);
    mbedtls_ecp_point_init(&g_ecdh_Q);

    int ret = mbedtls_ecp_group_load(&g_ecdh_grp,
                                      MBEDTLS_ECP_DP_CURVE25519);
    if (ret != 0) {
        printf("[sshd] x25519 group load: -0x%04x\n", (unsigned)-ret);
        return -1;
    }

    ret = mbedtls_ecp_gen_keypair(&g_ecdh_grp, &g_ecdh_d, &g_ecdh_Q,
                                   mbedtls_ctr_drbg_random, &g_drbg);
    if (ret != 0) {
        printf("[sshd] x25519 keygen: -0x%04x\n", (unsigned)-ret);
        return -1;
    }

    /* Export public key as 32 bytes little-endian (x25519 wire format) */
    ret = mbedtls_mpi_write_binary_le(&g_ecdh_Q.X, pub_out, 32);
    if (ret != 0) {
        printf("[sshd] x25519 pub export: -0x%04x\n", (unsigned)-ret);
        return -1;
    }

    return 0;
}

/* ----------------------------------------------------------------
 * x25519 ECDH: compute shared secret from peer public key
 *
 * peer_pub: 32 bytes little-endian (from client KEX_ECDH_INIT)
 * secret_out: receives big-endian shared secret
 * secret_len: set to actual length (may be < 32 if leading zeros)
 * ---------------------------------------------------------------- */

int ssh_ecdh_shared_secret(const uint8_t peer_pub[32],
                            uint8_t *secret_out, int *secret_len)
{
    mbedtls_ecp_point peer_Q, result;
    mbedtls_ecp_point_init(&peer_Q);
    mbedtls_ecp_point_init(&result);

    /* Import peer public key (little-endian x-coordinate) */
    int ret = mbedtls_mpi_read_binary_le(&peer_Q.X, peer_pub, 32);
    if (ret != 0) goto fail;
    ret = mbedtls_mpi_lset(&peer_Q.Z, 1);
    if (ret != 0) goto fail;

    /* Scalar multiply: result = our_private * peer_public */
    ret = mbedtls_ecp_mul(&g_ecdh_grp, &result, &g_ecdh_d, &peer_Q,
                           mbedtls_ctr_drbg_random, &g_drbg);
    if (ret != 0) {
        printf("[sshd] x25519 mul: -0x%04x\n", (unsigned)-ret);
        goto fail;
    }

    /* Export shared secret as little-endian (raw x25519 / RFC 7748 bytes).
     * OpenSSH passes raw crypto_scalarmult output directly to
     * sshbuf_put_bignum2_bytes without byte reversal. */
    ret = mbedtls_mpi_write_binary_le(&result.X, secret_out, 32);
    *secret_len = 32;

    mbedtls_ecp_point_free(&peer_Q);
    mbedtls_ecp_point_free(&result);
    return 0;

fail:
    mbedtls_ecp_point_free(&peer_Q);
    mbedtls_ecp_point_free(&result);
    return -1;
}

/* ----------------------------------------------------------------
 * Exchange hash H computation (RFC 4253 section 8)
 *
 * H = SHA-256(
 *   string V_C   (client version, no CRLF)
 *   string V_S   (server version, no CRLF)
 *   string I_C   (client KEXINIT payload)
 *   string I_S   (server KEXINIT payload)
 *   string K_S   (host public key blob)
 *   string Q_C   (client x25519 pub, 32 bytes)
 *   string Q_S   (server x25519 pub, 32 bytes)
 *   mpint  K     (shared secret, big-endian)
 * )
 *
 * Each "string" is [uint32 length][data].
 * mpint K: [uint32 length][big-endian bytes, leading 0x00 if MSB set].
 * ---------------------------------------------------------------- */

static void hash_string(mbedtls_sha256_context *ctx,
                         const void *data, uint32_t len)
{
    uint8_t lbuf[4];
    ssh_put_u32(lbuf, len);
    mbedtls_sha256_update(ctx, lbuf, 4);
    if (len > 0) {
        mbedtls_sha256_update(ctx, (const uint8_t *)data, len);
    }
}

static void hash_mpint(mbedtls_sha256_context *ctx,
                        const uint8_t *data, int datalen)
{
    /* Strip leading zeros (mpint is minimal) */
    while (datalen > 1 && data[0] == 0) {
        data++;
        datalen--;
    }

    /* Need leading 0x00 if MSB is set (positive value convention) */
    int need_pad = (data[0] & 0x80) ? 1 : 0;
    uint32_t wire_len = (uint32_t)(datalen + need_pad);

    uint8_t lbuf[4];
    ssh_put_u32(lbuf, wire_len);
    mbedtls_sha256_update(ctx, lbuf, 4);
    if (need_pad) {
        uint8_t zero = 0;
        mbedtls_sha256_update(ctx, &zero, 1);
    }
    mbedtls_sha256_update(ctx, data, (size_t)datalen);
}

int ssh_compute_exchange_hash(ssh_session_t *s,
                               const uint8_t *hostkey_blob, int hostkey_blob_len,
                               const uint8_t *client_pub, int client_pub_len,
                               const uint8_t *server_pub, int server_pub_len,
                               const uint8_t *shared_secret, int shared_secret_len,
                               uint8_t hash_out[32])
{
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);  /* 0 = SHA-256 */

    /* V_C (client version string, no CRLF) */
    int vc_len = 0;
    { const char *p = s->client_version; while (*p) { vc_len++; p++; } }
    hash_string(&sha, s->client_version, (uint32_t)vc_len);

    /* V_S (server version string, no CRLF) */
    int vs_len = 0;
    { const char *p = s->server_version; while (*p) { vs_len++; p++; } }
    hash_string(&sha, s->server_version, (uint32_t)vs_len);

    /* I_C (client KEXINIT payload, starts with SSH_MSG_KEXINIT byte) */
    hash_string(&sha, s->client_kexinit, (uint32_t)s->client_kexinit_len);

    /* I_S (server KEXINIT payload, starts with SSH_MSG_KEXINIT byte) */
    hash_string(&sha, s->server_kexinit, (uint32_t)s->server_kexinit_len);

    /* K_S (host public key blob) */
    hash_string(&sha, hostkey_blob, (uint32_t)hostkey_blob_len);

    /* Q_C (client ephemeral public key) */
    hash_string(&sha, client_pub, (uint32_t)client_pub_len);

    /* Q_S (server ephemeral public key) */
    hash_string(&sha, server_pub, (uint32_t)server_pub_len);

    /* K (shared secret as mpint) */
    hash_mpint(&sha, shared_secret, shared_secret_len);

    mbedtls_sha256_finish(&sha, hash_out);
    mbedtls_sha256_free(&sha);
    return 0;
}

/* ----------------------------------------------------------------
 * Key derivation (RFC 4253 section 7.2)
 *
 * key = SHA-256(mpint_K || H || letter || session_id)
 *
 * For AES-256-GCM: 32-byte key, 12-byte IV per direction.
 * ---------------------------------------------------------------- */

int ssh_derive_keys(ssh_session_t *s,
                     const uint8_t *shared_secret, int shared_secret_len)
{
    /* Set session_id = H from first key exchange */
    if (!s->session_id_set) {
        memcpy(s->session_id, s->exchange_hash, 32);
        s->session_id_set = 1;
    }

    /* Derive one key: SHA-256(mpint_K || H || letter || session_id) */
    /* We build the mpint_K encoding once */
    uint8_t mpint_buf[37]; /* 4-byte len + up to 33 bytes */
    int mpint_off = 0;

    /* Strip leading zeros from shared_secret */
    const uint8_t *kp = shared_secret;
    int klen = shared_secret_len;
    while (klen > 1 && kp[0] == 0) { kp++; klen--; }
    int need_pad = (kp[0] & 0x80) ? 1 : 0;
    uint32_t mpint_len = (uint32_t)(klen + need_pad);
    ssh_put_u32(mpint_buf, mpint_len);
    mpint_off = 4;
    if (need_pad) mpint_buf[mpint_off++] = 0;
    memcpy(mpint_buf + mpint_off, kp, klen);
    mpint_off += klen;

    /* Derive each key: letters A-F */
    static const char letters[] = "ABCDEF";
    uint8_t keys[6][32];  /* 6 derived keys, 32 bytes each */
    int i;

    for (i = 0; i < 6; i++) {
        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);

        mbedtls_sha256_update(&sha, mpint_buf, (size_t)mpint_off);
        mbedtls_sha256_update(&sha, s->exchange_hash, 32);
        mbedtls_sha256_update(&sha, (const uint8_t *)&letters[i], 1);
        mbedtls_sha256_update(&sha, s->session_id, 32);

        mbedtls_sha256_finish(&sha, keys[i]);
        mbedtls_sha256_free(&sha);
    }

    /* A = IV c2s (12 bytes), B = IV s2c (12 bytes) */
    /* C = key c2s (32 bytes), D = key s2c (32 bytes) */
    /* E = integrity c2s, F = integrity s2c (not needed for GCM) */

    printf("[sshd] Session keys derived:\n");
    printf("[sshd]   IV  c2s: %02x%02x%02x%02x...  IV  s2c: %02x%02x%02x%02x...\n",
           keys[0][0], keys[0][1], keys[0][2], keys[0][3],
           keys[1][0], keys[1][1], keys[1][2], keys[1][3]);
    printf("[sshd]   Key c2s: %02x%02x%02x%02x...  Key s2c: %02x%02x%02x%02x...\n",
           keys[2][0], keys[2][1], keys[2][2], keys[2][3],
           keys[3][0], keys[3][1], keys[3][2], keys[3][3]);

    /* Store in session for Phase 3 encryption */
    memcpy(s->iv_c2s,  keys[0], 12);
    memcpy(s->iv_s2c,  keys[1], 12);
    memcpy(s->key_c2s, keys[2], 32);
    memcpy(s->key_s2c, keys[3], 32);

    return 0;
}

/* ----------------------------------------------------------------
 * Cleanup ECDH state between connections
 * ---------------------------------------------------------------- */

void ssh_ecdh_cleanup(void)
{
    mbedtls_ecp_group_free(&g_ecdh_grp);
    mbedtls_mpi_free(&g_ecdh_d);
    mbedtls_ecp_point_free(&g_ecdh_Q);
}
