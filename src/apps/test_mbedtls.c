/* test_mbedtls.c -- tiny link+run smoke test for libmbedcrypto.a
 *
 * Exercises AES-256-CTR, SHA-256, and CTR-DRBG (which pulls the AIOS
 * /dev/urandom entropy shim mbedtls_hardware_poll). PASS = all three run.
 * Built by scripts/build_apps.py alongside sshd (same mbedTLS flags); used
 * by scripts/ssh_qemu_test.py as the runtime crypto smoke before the SSH test.
 */
#include <stdio.h>
#include <string.h>
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

int main(void)
{
    int rc, fails = 0;

    /* SHA-256 known-answer: sha256("abc") */
    unsigned char dig[32];
    static const unsigned char want[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
    rc = mbedtls_sha256((const unsigned char *)"abc", 3, dig, 0);
    if (rc != 0 || memcmp(dig, want, 32) != 0) { printf("SHA-256: FAIL\n"); fails++; }
    else printf("SHA-256: PASS\n");

    /* AES-256-CTR encrypt then decrypt round-trip */
    unsigned char key[32], nonce[16], sc[16], ct[32], pt[32];
    unsigned char in[32] = "abcdefghijklmnopqrstuvwxyz01234";
    size_t off;
    memset(key, 0x2b, 32);
    mbedtls_aes_context aes; mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 256);
    memset(nonce, 0, 16); memset(sc, 0, 16); off = 0;
    rc = mbedtls_aes_crypt_ctr(&aes, 32, &off, nonce, sc, in, ct);
    mbedtls_aes_setkey_enc(&aes, key, 256);  /* CTR decrypt = same op, fresh state */
    memset(nonce, 0, 16); memset(sc, 0, 16); off = 0;
    rc |= mbedtls_aes_crypt_ctr(&aes, 32, &off, nonce, sc, ct, pt);
    mbedtls_aes_free(&aes);
    if (rc != 0 || memcmp(pt, in, 32) != 0) { printf("AES-256-CTR: FAIL\n"); fails++; }
    else printf("AES-256-CTR: PASS\n");

    /* CTR-DRBG seed (drives entropy -> mbedtls_hardware_poll -> /dev/urandom) */
    mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&ent); mbedtls_ctr_drbg_init(&drbg);
    rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                               (const unsigned char *)"aios-smoke", 10);
    unsigned char r1[16] = {0}, r2[16] = {0};
    if (rc == 0) rc = mbedtls_ctr_drbg_random(&drbg, r1, 16);
    if (rc == 0) rc = mbedtls_ctr_drbg_random(&drbg, r2, 16);
    mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent);
    if (rc != 0 || memcmp(r1, r2, 16) == 0) { printf("CTR-DRBG: FAIL (rc=%d)\n", rc); fails++; }
    else printf("CTR-DRBG: PASS\n");

    printf(fails == 0 ? "=== mbedtls smoke: PASS ===\n" : "=== mbedtls smoke: FAIL ===\n");
    return fails;
}
