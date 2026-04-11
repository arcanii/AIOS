/* ssh_kex.c -- SSH KEXINIT exchange and algorithm negotiation
 *
 * Builds our KEXINIT message, sends it, receives the client KEXINIT,
 * and negotiates common algorithms (first match wins).
 *
 * RFC 4253 section 7.1
 */

#include "ssh_session.h"
#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Helper: check if needle appears in a comma-separated name-list
 * Returns 1 if found, 0 if not.
 * ---------------------------------------------------------------- */

static int namelist_contains(const uint8_t *list, uint32_t listlen,
                              const char *needle)
{
    int nlen = 0;
    const char *p = needle;
    while (*p) { nlen++; p++; }

    int i = 0;
    while (i < (int)listlen) {
        /* Find end of current name (comma or end of list) */
        int start = i;
        while (i < (int)listlen && list[i] != ',') i++;
        int namelen = i - start;

        if (namelen == nlen && memcmp(list + start, needle, nlen) == 0) {
            return 1;
        }
        i++; /* skip comma */
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Build and send our KEXINIT (RFC 4253 section 7.1)
 *
 * Format:
 *   byte      SSH_MSG_KEXINIT (20)
 *   byte[16]  cookie (random)
 *   string    kex_algorithms
 *   string    server_host_key_algorithms
 *   string    encryption_algorithms_client_to_server
 *   string    encryption_algorithms_server_to_client
 *   string    mac_algorithms_client_to_server
 *   string    mac_algorithms_server_to_client
 *   string    compression_algorithms_client_to_server
 *   string    compression_algorithms_server_to_client
 *   string    languages_client_to_server
 *   string    languages_server_to_client
 *   boolean   first_kex_packet_follows (FALSE)
 *   uint32    reserved (0)
 * ---------------------------------------------------------------- */

int ssh_send_kexinit(ssh_session_t *s)
{
    uint8_t payload[1024];
    int off = 0;

    /* Message type */
    payload[off++] = SSH_MSG_KEXINIT;

    /* 16-byte random cookie */
    {
        uint64_t st;
        __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(st));
        int i;
        for (i = 0; i < 16; i++) {
            st += 0x9E3779B97F4A7C15ULL;
            uint64_t z = st;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            z = z ^ (z >> 31);
            payload[off++] = (uint8_t)(z & 0xFF);
        }
    }

    /* Algorithm name-lists (use full list with extensions for KEXINIT) */
    ssh_put_namelist(payload, SSH_KEX_LIST,      &off);  /* kex */
    ssh_put_namelist(payload, SSH_HOSTKEY_ALGO,   &off);  /* hostkey */
    ssh_put_namelist(payload, SSH_CIPHER_ALGO,    &off);  /* enc c2s */
    ssh_put_namelist(payload, SSH_CIPHER_ALGO,    &off);  /* enc s2c */
    ssh_put_namelist(payload, SSH_MAC_ALGO,       &off);  /* mac c2s */
    ssh_put_namelist(payload, SSH_MAC_ALGO,       &off);  /* mac s2c */
    ssh_put_namelist(payload, SSH_COMPRESS_ALGO,  &off);  /* comp c2s */
    ssh_put_namelist(payload, SSH_COMPRESS_ALGO,  &off);  /* comp s2c */
    ssh_put_namelist(payload, "",                 &off);  /* lang c2s */
    ssh_put_namelist(payload, "",                 &off);  /* lang s2c */

    /* first_kex_packet_follows = FALSE */
    payload[off++] = 0;

    /* reserved uint32 = 0 */
    ssh_put_u32(payload + off, 0);
    off += 4;

    /* Save a copy of our KEXINIT payload (needed for exchange hash) */
    if (off > (int)sizeof(s->server_kexinit)) {
        printf("[sshd] KEXINIT too large\n");
        return -1;
    }
    memcpy(s->server_kexinit, payload, off);
    s->server_kexinit_len = off;

    /* Send as SSH packet */
    if (ssh_write_packet(s, payload, off) < 0) {
        printf("[sshd] KEXINIT send failed\n");
        return -1;
    }

    printf("[sshd] KEXINIT sent (%d bytes)\n", off);
    return 0;
}

/* ----------------------------------------------------------------
 * Receive and parse client KEXINIT
 * ---------------------------------------------------------------- */

int ssh_recv_kexinit(ssh_session_t *s)
{
    uint8_t payload[SSH_BUF_SIZE];
    int plen = 0;

    if (ssh_read_packet(s, payload, &plen) < 0) {
        printf("[sshd] KEXINIT read failed\n");
        return -1;
    }

    if (plen < 17 || payload[0] != SSH_MSG_KEXINIT) {
        printf("[sshd] expected KEXINIT, got msg type %d\n",
               plen > 0 ? payload[0] : -1);
        return -1;
    }

    /* Save full payload (needed for exchange hash) */
    if (plen > (int)sizeof(s->client_kexinit)) {
        printf("[sshd] client KEXINIT too large: %d\n", plen);
        return -1;
    }
    memcpy(s->client_kexinit, payload, plen);
    s->client_kexinit_len = plen;

    printf("[sshd] KEXINIT received (%d bytes)\n", plen);

    /* Parse the 10 name-lists to display negotiation info */
    int off = 17;   /* skip msg_type(1) + cookie(16) */

    static const char *names[] = {
        "kex", "hostkey", "cipher_c2s", "cipher_s2c",
        "mac_c2s", "mac_s2c", "comp_c2s", "comp_s2c",
        "lang_c2s", "lang_s2c"
    };
    static const char *ours[] = {
        SSH_KEX_ALGO, SSH_HOSTKEY_ALGO,
        SSH_CIPHER_ALGO, SSH_CIPHER_ALGO,
        SSH_MAC_ALGO, SSH_MAC_ALGO,
        SSH_COMPRESS_ALGO, SSH_COMPRESS_ALGO,
        "", ""
    };

    int i;
    int negotiate_ok = 1;

    for (i = 0; i < 10; i++) {
        const uint8_t *list;
        uint32_t listlen;
        if (ssh_get_string(payload, plen, &off, &list, &listlen) < 0) {
            printf("[sshd] KEXINIT parse error at field %d\n", i);
            return -1;
        }

        /* Check if our algorithm is in the client list */
        const char *our = ours[i];
        int ourlen = 0;
        { const char *p = our; while (*p) { ourlen++; p++; } }

        if (ourlen == 0) {
            /* Empty list on our side (mac with GCM, or languages) */
            printf("[sshd]   %-12s (not negotiated)\n", names[i]);
            continue;
        }

        int match = namelist_contains(list, listlen, our);
        printf("[sshd]   %-12s %s %s\n", names[i], our,
               match ? "OK" : "MISSING");

        /* kex, hostkey, cipher_c2s, cipher_s2c, comp must match */
        if (!match && i < 4) {
            negotiate_ok = 0;
        }
        /* compression: check for "none" */
        if (i >= 6 && i <= 7 && !match) {
            if (!namelist_contains(list, listlen, "none")) {
                negotiate_ok = 0;
            }
        }
    }

    if (!negotiate_ok) {
        printf("[sshd] Algorithm negotiation failed\n");
        return -1;
    }

    printf("[sshd] Algorithm negotiation OK\n");

    /* Strict KEX (Terrapin countermeasure): reset sequence numbers
     * after both sides have sent/received KEXINIT */
    s->seq_recv = 0;
    s->seq_send = 0;
    printf("[sshd] Strict KEX: sequence numbers reset\n");

    return 0;
}

/* ----------------------------------------------------------------
 * Run full KEXINIT exchange
 * ---------------------------------------------------------------- */

int ssh_do_kexinit(ssh_session_t *s)
{
    /* Server sends KEXINIT first (RFC says either side can go first) */
    if (ssh_send_kexinit(s) < 0) return -1;
    if (ssh_recv_kexinit(s) < 0) return -1;
    return 0;
}

/* ----------------------------------------------------------------
 * ECDH key exchange: curve25519-sha256 (RFC 8731)
 *
 * 1. Receive SSH_MSG_KEX_ECDH_INIT (client pub key, 32 bytes)
 * 2. Generate server ephemeral x25519 keypair
 * 3. Compute shared secret K
 * 4. Compute exchange hash H
 * 5. Sign H with host key
 * 6. Send SSH_MSG_KEX_ECDH_REPLY (host key blob, server pub, sig)
 * 7. Send SSH_MSG_NEWKEYS
 * 8. Receive SSH_MSG_NEWKEYS
 * 9. Derive session keys
 * ---------------------------------------------------------------- */

int ssh_do_kex_exchange(ssh_session_t *s)
{
    uint8_t payload[SSH_BUF_SIZE];
    int plen = 0;

    /* Step 1: Receive KEX_ECDH_INIT from client */
    if (ssh_read_packet(s, payload, &plen) < 0) {
        printf("[sshd] KEX_ECDH_INIT read failed\n");
        return -1;
    }

    if (plen < 1 || payload[0] != SSH_MSG_KEX_ECDH_INIT) {
        printf("[sshd] Expected KEX_ECDH_INIT (30), got %d\n",
               plen > 0 ? payload[0] : -1);
        return -1;
    }

    /* Extract client public key (SSH string: 4-byte len + 32 bytes) */
    int off = 1;
    const uint8_t *client_pub;
    uint32_t client_pub_len;
    if (ssh_get_string(payload, plen, &off, &client_pub, &client_pub_len) < 0
        || client_pub_len != 32) {
        printf("[sshd] Bad client public key (len %u)\n",
               (unsigned)client_pub_len);
        return -1;
    }

    printf("[sshd] KEX_ECDH_INIT: client pub %02x%02x%02x%02x...\n",
           client_pub[0], client_pub[1], client_pub[2], client_pub[3]);

    /* Step 2: Generate server ephemeral x25519 keypair */
    uint8_t server_pub[32];
    if (ssh_ecdh_generate(server_pub) < 0) {
        printf("[sshd] Server keygen failed\n");
        return -1;
    }

    printf("[sshd] Server pub: %02x%02x%02x%02x...\n",
           server_pub[0], server_pub[1], server_pub[2], server_pub[3]);

    /* Step 3: Compute shared secret K */
    uint8_t shared_secret[32];
    int shared_len = 0;
    if (ssh_ecdh_shared_secret(client_pub, shared_secret, &shared_len) < 0) {
        printf("[sshd] ECDH shared secret failed\n");
        return -1;
    }

    printf("[sshd] Shared secret: %02x%02x%02x%02x... (%d bytes)\n",
           shared_secret[0], shared_secret[1],
           shared_secret[2], shared_secret[3], shared_len);

    /* Step 4: Get host public key blob */
    uint8_t hostkey_blob[256];
    int hostkey_blob_len = 0;
    if (ssh_hostkey_get_blob(hostkey_blob, &hostkey_blob_len) < 0) {
        printf("[sshd] Host key blob failed\n");
        return -1;
    }

    /* Step 5: Compute exchange hash H */
    if (ssh_compute_exchange_hash(s,
            hostkey_blob, hostkey_blob_len,
            client_pub, 32,
            server_pub, 32,
            shared_secret, shared_len,
            s->exchange_hash) < 0) {
        printf("[sshd] Exchange hash failed\n");
        return -1;
    }

    printf("[sshd] Exchange hash H: %02x%02x%02x%02x...\n",
           s->exchange_hash[0], s->exchange_hash[1],
           s->exchange_hash[2], s->exchange_hash[3]);

    /* Step 6: Sign exchange hash with host key */
    uint8_t sig_blob[256];
    int sig_len = 0;
    if (ssh_hostkey_sign(s->exchange_hash, 32, sig_blob, &sig_len) < 0) {
        printf("[sshd] Host key sign failed\n");
        return -1;
    }

    /* Step 7: Build and send SSH_MSG_KEX_ECDH_REPLY */
    {
        uint8_t reply[1024];
        int roff = 0;

        reply[roff++] = SSH_MSG_KEX_ECDH_REPLY;

        /* string K_S (host public key blob) */
        ssh_put_string(reply, hostkey_blob, (uint32_t)hostkey_blob_len, &roff);

        /* string f (server ephemeral public key, 32 bytes) */
        ssh_put_string(reply, server_pub, 32, &roff);

        /* string signature blob */
        ssh_put_string(reply, sig_blob, (uint32_t)sig_len, &roff);

        if (ssh_write_packet(s, reply, roff) < 0) {
            printf("[sshd] KEX_ECDH_REPLY send failed\n");
            return -1;
        }

        printf("[sshd] KEX_ECDH_REPLY sent (%d bytes)\n", roff);
    }

    /* Step 8: Send SSH_MSG_NEWKEYS */
    {
        uint8_t nk[1];
        nk[0] = SSH_MSG_NEWKEYS;
        if (ssh_write_packet(s, nk, 1) < 0) {
            printf("[sshd] NEWKEYS send failed\n");
            return -1;
        }
        printf("[sshd] NEWKEYS sent\n");
    }

    /* Step 9: Receive SSH_MSG_NEWKEYS from client */
    if (ssh_read_packet(s, payload, &plen) < 0) {
        printf("[sshd] NEWKEYS read failed\n");
        return -1;
    }

    if (plen < 1 || payload[0] != SSH_MSG_NEWKEYS) {
        printf("[sshd] Expected NEWKEYS (21), got %d\n",
               plen > 0 ? payload[0] : -1);
        return -1;
    }

    printf("[sshd] NEWKEYS received -- key exchange complete\n");

    /* Step 10: Derive session keys */
    memcpy(s->shared_secret, shared_secret, shared_len);
    s->shared_secret_len = shared_len;

    if (ssh_derive_keys(s, shared_secret, shared_len) < 0) {
        printf("[sshd] Key derivation failed\n");
        return -1;
    }

    /* Strict KEX: reset sequence numbers after NEWKEYS exchange */
    s->seq_recv = 0;
    s->seq_send = 0;
    printf("[sshd] Strict KEX: post-NEWKEYS seq reset\n");

    ssh_ecdh_cleanup();

    printf("[sshd] === Key exchange complete, encryption ready ===\n");
    return 0;
}
