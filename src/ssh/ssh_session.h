/* ssh_session.h -- AIOS SSH server shared definitions
 *
 * Constants, message types, session state, wire-format helpers.
 * All SSH source files include this header.
 */
#ifndef SSH_SESSION_H
#define SSH_SESSION_H

#include <stdint.h>
#include <stddef.h>

/* ----------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------- */

#define SSH_VERSION_STRING  "SSH-2.0-AIOS_0.4"
#define SSH_BUF_SIZE        4096    /* packet buffer (keep small for AIOS) */
#define SSH_MAX_PAYLOAD     2048    /* max payload we accept */
#define SOCK_CHUNK          900     /* AIOS socket max per read/write */
#define SSH_MAX_VERSION_LEN 255     /* RFC 4253 section 4.2 */
#define SSH_MAC_LEN         32      /* HMAC-SHA-256 output */

/* Listening port (use 2222 during development, 22 for production) */
#define SSHD_PORT           2222

/* ----------------------------------------------------------------
 * SSH message type numbers (RFC 4253, 4252, 4254)
 * ---------------------------------------------------------------- */

/* Transport layer (RFC 4253) */
#define SSH_MSG_DISCONNECT              1
#define SSH_MSG_IGNORE                  2
#define SSH_MSG_UNIMPLEMENTED           3
#define SSH_MSG_DEBUG                   4
#define SSH_MSG_SERVICE_REQUEST         5
#define SSH_MSG_SERVICE_ACCEPT          6
#define SSH_MSG_EXT_INFO                7
#define SSH_MSG_KEXINIT                20
#define SSH_MSG_NEWKEYS                21

/* Key exchange method-specific (30-49) */
#define SSH_MSG_KEX_ECDH_INIT          30
#define SSH_MSG_KEX_ECDH_REPLY         31

/* User authentication (RFC 4252) */
#define SSH_MSG_USERAUTH_REQUEST       50
#define SSH_MSG_USERAUTH_FAILURE       51
#define SSH_MSG_USERAUTH_SUCCESS       52
#define SSH_MSG_USERAUTH_BANNER        53

/* Connection protocol (RFC 4254) */
#define SSH_MSG_GLOBAL_REQUEST         80
#define SSH_MSG_REQUEST_SUCCESS        81
#define SSH_MSG_REQUEST_FAILURE        82
#define SSH_MSG_CHANNEL_OPEN           90
#define SSH_MSG_CHANNEL_OPEN_CONFIRM   91
#define SSH_MSG_CHANNEL_OPEN_FAILURE   92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST  93
#define SSH_MSG_CHANNEL_DATA           94
#define SSH_MSG_CHANNEL_EXTENDED_DATA  95
#define SSH_MSG_CHANNEL_EOF            96
#define SSH_MSG_CHANNEL_CLOSE          97
#define SSH_MSG_CHANNEL_REQUEST        98
#define SSH_MSG_CHANNEL_SUCCESS        99
#define SSH_MSG_CHANNEL_FAILURE       100

/* SSH disconnect reason codes (RFC 4253 section 11.1) */
#define SSH_DISCONNECT_PROTOCOL_ERROR           2
#define SSH_DISCONNECT_KEY_EXCHANGE_FAILED       3
#define SSH_DISCONNECT_MAC_ERROR                5
#define SSH_DISCONNECT_SERVICE_NOT_AVAILABLE     7
#define SSH_DISCONNECT_BY_APPLICATION           11

/* ----------------------------------------------------------------
 * Algorithm name strings (what we offer in KEXINIT)
 * ---------------------------------------------------------------- */

/* Full list sent in KEXINIT (includes extensions + aliases) */
#define SSH_KEX_LIST        "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-s-v00@openssh.com"
/* Actual algorithm to match during negotiation */
#define SSH_KEX_ALGO        "curve25519-sha256"
#define SSH_HOSTKEY_ALGO    "ecdsa-sha2-nistp256"
#define SSH_CIPHER_ALGO     "aes256-ctr"
#define SSH_MAC_ALGO        "hmac-sha2-256"
#define SSH_COMPRESS_ALGO   "none"

/* ----------------------------------------------------------------
 * Input buffer -- accumulates bytes from 900B socket reads
 * ---------------------------------------------------------------- */

typedef struct {
    uint8_t  buf[SSH_BUF_SIZE];
    int      len;       /* bytes currently in buf[] */
} ssh_inbuf_t;

/* ----------------------------------------------------------------
 * Session state
 * ---------------------------------------------------------------- */

typedef struct {
    int          sockfd;            /* connected client socket fd */

    /* Version strings (without trailing CR LF) */
    char         client_version[SSH_MAX_VERSION_LEN + 1];
    char         server_version[SSH_MAX_VERSION_LEN + 1];

    /* KEXINIT payloads (saved for exchange hash computation) */
    uint8_t      client_kexinit[SSH_BUF_SIZE];
    int          client_kexinit_len;
    uint8_t      server_kexinit[SSH_BUF_SIZE];
    int          server_kexinit_len;

    /* Packet sequence counters */
    uint32_t     seq_recv;
    uint32_t     seq_send;

    /* Packet reassembly buffer */
    ssh_inbuf_t  inbuf;

    /* Key exchange results */
    uint8_t      session_id[32];
    int          session_id_set;
    uint8_t      exchange_hash[32];
    uint8_t      shared_secret[32];
    int          shared_secret_len;

    /* Derived session keys (for Phase 3 encryption) */
    uint8_t      iv_c2s[16];
    uint8_t      iv_s2c[16];
    uint8_t      key_c2s[32];
    uint8_t      key_s2c[32];
    uint8_t      mac_c2s[32];        /* Integrity key (letter E) */
    uint8_t      mac_s2c[32];        /* Integrity key (letter F) */
    int          encrypted;          /* 1 after encrypt_init */

    /* Auth state (populated in Phase 3) */
    uint32_t     uid;
    uint32_t     gid;
    uint32_t     auth_token;
    int          authenticated;

    /* Channel state */
    uint32_t     client_channel;
    uint32_t     server_channel;
    int          channel_open;

    /* Channel flow control (Phase 5) */
    uint32_t     client_window;     /* bytes client willing to receive */
    uint32_t     client_max_pkt;    /* max packet size client accepts */
    uint32_t     server_window;     /* bytes we are willing to receive */

    /* PTY state */
    uint32_t     term_width;
    uint32_t     term_height;
    int          has_pty;

    /* Peek byte for encrypted read (one-byte lookahead) */
    int          has_peek;
    uint8_t      peek_byte;

    /* Non-blocking packet reassembly state */
    uint8_t      nb_dec_hdr[4];   /* decrypted header cache */
    uint32_t     nb_pkt_len;      /* packet length from header */
    int          nb_hdr_done;     /* header decrypted flag */
} ssh_session_t;

/* ----------------------------------------------------------------
 * Wire format helpers (implemented in ssh_transport.c)
 * ---------------------------------------------------------------- */

/* Write big-endian uint32 to buffer */
static inline void ssh_put_u32(uint8_t *buf, uint32_t v)
{
    buf[0] = (uint8_t)(v >> 24);
    buf[1] = (uint8_t)(v >> 16);
    buf[2] = (uint8_t)(v >>  8);
    buf[3] = (uint8_t)(v);
}

/* Read big-endian uint32 from buffer */
static inline uint32_t ssh_get_u32(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] <<  8) |
           ((uint32_t)buf[3]);
}

/* Write SSH string (4-byte length + data) at offset, advance offset */
static inline void ssh_put_string(uint8_t *buf, const void *data,
                                   uint32_t len, int *offset)
{
    ssh_put_u32(buf + *offset, len);
    *offset += 4;
    if (len > 0 && data) {
        __builtin_memcpy(buf + *offset, data, len);
        *offset += (int)len;
    }
}

/* Write SSH name-list (same format as string) */
static inline void ssh_put_namelist(uint8_t *buf, const char *names,
                                     int *offset)
{
    uint32_t len = 0;
    const char *p = names;
    while (*p) { len++; p++; }
    ssh_put_string(buf, names, len, offset);
}

/* Read SSH string at offset, return pointer + length, advance offset.
 * Returns 0 on success, -1 if buffer too short. */
static inline int ssh_get_string(const uint8_t *buf, int buflen,
                                  int *offset,
                                  const uint8_t **out, uint32_t *outlen)
{
    if (*offset + 4 > buflen) return -1;
    uint32_t len = ssh_get_u32(buf + *offset);
    *offset += 4;
    if (*offset + (int)len > buflen) return -1;
    *out = buf + *offset;
    *outlen = len;
    *offset += (int)len;
    return 0;
}

/* ----------------------------------------------------------------
 * Function declarations -- ssh_transport.c
 * ---------------------------------------------------------------- */

/* Exchange version strings with client */
int ssh_version_exchange(ssh_session_t *s);

/* Read one SSH binary packet, return payload and length.
 * payload buffer must be at least SSH_BUF_SIZE bytes. */
int ssh_read_packet(ssh_session_t *s, uint8_t *payload, int *payload_len);

/* Non-blocking variant: returns 0 on success, 1 if no data, -1 on error */
int ssh_read_packet_nb(ssh_session_t *s, uint8_t *payload, int *payload_len);

/* Write one SSH binary packet from payload.
 * Handles padding and 900-byte chunked socket writes. */
int ssh_write_packet(ssh_session_t *s, const uint8_t *payload, int payload_len);

/* Send SSH_MSG_DISCONNECT and close */
int ssh_disconnect(ssh_session_t *s, uint32_t reason, const char *desc);

/* ----------------------------------------------------------------
 * Function declarations -- ssh_kex.c
 * ---------------------------------------------------------------- */

/* Build and send our KEXINIT, save payload copy */
int ssh_send_kexinit(ssh_session_t *s);

/* Read and parse client KEXINIT, save payload copy */
int ssh_recv_kexinit(ssh_session_t *s);

/* Run full KEXINIT exchange (send + recv + negotiate) */
int ssh_do_kexinit(ssh_session_t *s);

/* Run ECDH key exchange after KEXINIT (KEX_ECDH_INIT/REPLY + NEWKEYS) */
int ssh_do_kex_exchange(ssh_session_t *s);

/* ----------------------------------------------------------------
 * Function declarations -- ssh_crypto.c
 * ---------------------------------------------------------------- */

/* Initialize crypto: seed DRBG, generate host key */
int  ssh_crypto_init(void);

/* Generate random bytes */
void ssh_random_bytes(uint8_t *buf, int len);

/* Export host public key blob in SSH wire format */
int  ssh_hostkey_get_blob(uint8_t *buf, int *len);

/* Sign hash with host key, produce SSH signature blob */
int  ssh_hostkey_sign(const uint8_t *hash, int hashlen,
                       uint8_t *sigbuf, int *siglen);

/* Generate x25519 ephemeral keypair, return 32-byte public key */
int  ssh_ecdh_generate(uint8_t pub_out[32]);

/* Compute shared secret from peer x25519 public key */
int  ssh_ecdh_shared_secret(const uint8_t peer_pub[32],
                             uint8_t *secret_out, int *secret_len);

/* Compute exchange hash H */
int  ssh_compute_exchange_hash(ssh_session_t *s,
                                const uint8_t *hostkey_blob, int hostkey_blob_len,
                                const uint8_t *client_pub, int client_pub_len,
                                const uint8_t *server_pub, int server_pub_len,
                                const uint8_t *shared_secret, int shared_secret_len,
                                uint8_t hash_out[32]);

/* Derive session keys from shared secret and exchange hash */
int  ssh_derive_keys(ssh_session_t *s,
                      const uint8_t *shared_secret, int shared_secret_len);

/* Cleanup ECDH state between connections */
void ssh_ecdh_cleanup(void);

/* ----------------------------------------------------------------
 * Function declarations -- ssh_auth.c
 * ---------------------------------------------------------------- */

/* Run SSH user authentication (RFC 4252) */
int  ssh_do_userauth(ssh_session_t *s);

/* ----------------------------------------------------------------
 * Function declarations -- ssh_channel.c
 * ---------------------------------------------------------------- */

/* Run SSH session channel: open, pty-req, shell, data relay (RFC 4254) */
int  ssh_do_channel(ssh_session_t *s);

/* ----------------------------------------------------------------
 * Function declarations -- ssh_encrypt.c
 * ---------------------------------------------------------------- */

/* Initialize AES-256-CTR + HMAC-SHA-256 from session keys */
int  ssh_encrypt_init(ssh_session_t *s);

/* Encrypt data (send / s2c direction) */
void ssh_ctr_encrypt(const uint8_t *in, uint8_t *out, int len);

/* Decrypt data (recv / c2s direction) */
void ssh_ctr_decrypt(const uint8_t *in, uint8_t *out, int len);

/* Compute MAC for send direction */
void ssh_mac_compute_send(const uint8_t *data, int len, uint8_t mac[32]);

/* Verify MAC for recv direction (0 = OK, -1 = fail) */
int  ssh_mac_verify_recv(const uint8_t *data, int len,
                         const uint8_t expected[32]);

#endif /* SSH_SESSION_H */
