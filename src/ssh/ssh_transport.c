/* ssh_transport.c -- SSH binary packet transport layer
 *
 * Handles version exchange, packet framing, and the 900-byte
 * chunked I/O required by AIOS socket read/write limits.
 *
 * RFC 4253 sections 4.2 (version), 6 (binary packet protocol)
 */

#include "ssh_session.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

/* ----------------------------------------------------------------
 * Low-level socket I/O with 900-byte chunking
 * ---------------------------------------------------------------- */

/* Read exactly n bytes from socket, chunking at SOCK_CHUNK.
 * Returns 0 on success, -1 on EOF/error. */
static int sock_read_exact(int fd, uint8_t *buf, int n)
{
    int total = 0;
    while (total < n) {
        int want = n - total;
        if (want > SOCK_CHUNK) want = SOCK_CHUNK;
        int got = (int)read(fd, buf + total, want);
        if (got <= 0) return -1;
        total += got;
    }
    return 0;
}

/* Write exactly n bytes to socket.
 * AIOS posix_file.c already chunks writes at 900 bytes internally,
 * but we call write in a loop for safety. */
static int sock_write_all(int fd, const uint8_t *buf, int n)
{
    int total = 0;
    while (total < n) {
        int chunk = n - total;
        if (chunk > SOCK_CHUNK) chunk = SOCK_CHUNK;
        int sent = (int)write(fd, buf + total, chunk);
        if (sent <= 0) return -1;
        total += sent;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Version exchange (RFC 4253 section 4.2)
 *
 * Both sides send: SSH-2.0-softwareversion\r\n
 * We send ours, then read the client line byte-by-byte until \n.
 * Store version WITHOUT trailing \r\n.
 * ---------------------------------------------------------------- */

int ssh_version_exchange(ssh_session_t *s)
{
    /* Build and send our version string */
    char vbuf[SSH_MAX_VERSION_LEN + 3]; /* room for \r\n\0 */
    int vlen = snprintf(vbuf, sizeof(vbuf), "%s\r\n", SSH_VERSION_STRING);
    if (sock_write_all(s->sockfd, (const uint8_t *)vbuf, vlen) < 0) {
        printf("[sshd] version send failed\n");
        return -1;
    }

    /* Save our version (without CRLF) */
    snprintf(s->server_version, sizeof(s->server_version),
             "%s", SSH_VERSION_STRING);

    /* Read client version string byte by byte until \n
     * RFC allows lines before the SSH- line, but most clients
     * send SSH-2.0-... as the first line. */
    char line[SSH_MAX_VERSION_LEN + 2];
    int pos = 0;
    int found = 0;

    while (!found) {
        pos = 0;
        while (pos < (int)sizeof(line) - 1) {
            uint8_t ch;
            if (sock_read_exact(s->sockfd, &ch, 1) < 0) {
                printf("[sshd] version read failed\n");
                return -1;
            }
            line[pos++] = (char)ch;
            if (ch == '\n') break;
        }
        line[pos] = '\0';

        /* Check if this line starts with SSH- */
        if (pos >= 4 && line[0]=='S' && line[1]=='S' &&
            line[2]=='H' && line[3]=='-') {
            found = 1;
        }
        /* Otherwise skip the line (RFC 4253 allows leading lines) */
    }

    /* Strip trailing \r\n */
    while (pos > 0 && (line[pos-1]=='\n' || line[pos-1]=='\r')) {
        line[--pos] = '\0';
    }

    /* Copy to session (without CRLF) */
    if (pos >= (int)sizeof(s->client_version)) {
        printf("[sshd] client version too long\n");
        return -1;
    }
    memcpy(s->client_version, line, pos + 1);

    printf("[sshd] Client: %s\n", s->client_version);
    printf("[sshd] Server: %s\n", s->server_version);
    return 0;
}

/* ----------------------------------------------------------------
 * Read one SSH binary packet (encrypted mode)
 *
 * AES-256-CTR decryption + HMAC-SHA-256 verification.
 * The CTR stream is continuous across packets (not reset).
 *
 * Wire format: [encrypted(4 + pkt_len)] [MAC(32)]
 * MAC input:   seq_num(4) || plaintext(4 + pkt_len)
 * ---------------------------------------------------------------- */

static int ssh_read_encrypted(ssh_session_t *s,
                               uint8_t *payload, int *payload_len)
{
    /* 1. Read and decrypt 4-byte packet length */
    uint8_t enc_hdr[4], dec_hdr[4];
    if (s->has_peek) {
        /* Use peeked byte as first byte of encrypted header */
        enc_hdr[0] = s->peek_byte;
        s->has_peek = 0;
        if (sock_read_exact(s->sockfd, enc_hdr + 1, 3) < 0) {
            printf("[sshd] enc-read: header(peek) failed\n");
            return -1;
        }
    } else {
        if (sock_read_exact(s->sockfd, enc_hdr, 4) < 0) {
            printf("[sshd] enc-read: header failed\n");
            return -1;
        }
    }
    ssh_ctr_decrypt(enc_hdr, dec_hdr, 4);

    uint32_t pkt_len = ssh_get_u32(dec_hdr);
    if (pkt_len < 2 || pkt_len > SSH_BUF_SIZE - 4) {
        printf("[sshd] enc-read: bad pkt_len %u\n", (unsigned)pkt_len);
        return -1;
    }

    /* 2. Read and decrypt body (padding_length + payload + padding) */
    uint8_t enc_body[SSH_BUF_SIZE];
    uint8_t dec_body[SSH_BUF_SIZE];
    if (sock_read_exact(s->sockfd, enc_body, (int)pkt_len) < 0) {
        printf("[sshd] enc-read: body failed\n");
        return -1;
    }
    ssh_ctr_decrypt(enc_body, dec_body, (int)pkt_len);

    /* 3. Read MAC (plaintext, 32 bytes) */
    uint8_t mac_recv[SSH_MAC_LEN];
    if (sock_read_exact(s->sockfd, mac_recv, SSH_MAC_LEN) < 0) {
        printf("[sshd] enc-read: MAC read failed\n");
        return -1;
    }

    /* 4. Verify MAC: HMAC(key, seq(4) || pkt_length(4) || body(pkt_len)) */
    uint8_t mac_input[SSH_BUF_SIZE + 8];
    int mi = 0;
    ssh_put_u32(mac_input + mi, s->seq_recv);  mi += 4;
    memcpy(mac_input + mi, dec_hdr, 4);        mi += 4;
    memcpy(mac_input + mi, dec_body, pkt_len); mi += (int)pkt_len;

    if (ssh_mac_verify_recv(mac_input, mi, mac_recv) < 0) {
        printf("[sshd] MAC FAILED (seq %u)\n", (unsigned)s->seq_recv);
        return -1;
    }

    /* 5. Extract payload from body */
    uint8_t pad_len = dec_body[0];
    int pload_len = (int)pkt_len - (int)pad_len - 1;
    if (pload_len < 0 || pload_len > SSH_MAX_PAYLOAD) {
        printf("[sshd] enc-read: bad payload len %d\n", pload_len);
        return -1;
    }

    memcpy(payload, dec_body + 1, pload_len);
    *payload_len = pload_len;

    s->seq_recv++;
    return 0;
}

/* ----------------------------------------------------------------
 * Write one SSH binary packet (encrypted mode)
 *
 * 1. Build plaintext (16-byte aligned padding for AES)
 * 2. Compute MAC over seq_num + plaintext
 * 3. Encrypt plaintext with AES-256-CTR
 * 4. Send encrypted + MAC
 * ---------------------------------------------------------------- */

static int ssh_write_encrypted(ssh_session_t *s,
                                const uint8_t *payload, int payload_len)
{
    uint8_t pkt[SSH_BUF_SIZE + 64];

    /* Padding: 16-byte block alignment for AES (RFC 4253 section 6) */
    int block = 16;
    int total_unpadded = 4 + 1 + payload_len;
    int pad_len = block - (total_unpadded % block);
    if (pad_len < 4) pad_len += block;

    uint32_t pkt_len = (uint32_t)(1 + payload_len + pad_len);

    /* Build plaintext */
    int off = 0;
    ssh_put_u32(pkt + off, pkt_len);          off += 4;
    pkt[off++] = (uint8_t)pad_len;
    memcpy(pkt + off, payload, payload_len);  off += payload_len;
    ssh_random_bytes(pkt + off, pad_len);     off += pad_len;

    /* Compute MAC: HMAC(key, seq(4) || plaintext(off)) */
    uint8_t mac_input[SSH_BUF_SIZE + 68];
    ssh_put_u32(mac_input, s->seq_send);
    memcpy(mac_input + 4, pkt, off);

    uint8_t mac[SSH_MAC_LEN];
    ssh_mac_compute_send(mac_input, 4 + off, mac);

    /* Encrypt plaintext */
    uint8_t enc[SSH_BUF_SIZE + 64];
    ssh_ctr_encrypt(pkt, enc, off);

    /* Send: encrypted packet + MAC */
    if (sock_write_all(s->sockfd, enc, off) < 0) {
        printf("[sshd] enc-write: data failed\n");
        return -1;
    }
    if (sock_write_all(s->sockfd, mac, SSH_MAC_LEN) < 0) {
        printf("[sshd] enc-write: MAC failed\n");
        return -1;
    }

    s->seq_send++;
    return 0;
}

/* ----------------------------------------------------------------
 * Read one SSH binary packet (plaintext mode)
 *
 * Packet format (RFC 4253 section 6):
 *   uint32  packet_length  (excludes itself and MAC)
 *   byte    padding_length
 *   byte[]  payload        (packet_length - padding_length - 1)
 *   byte[]  random_padding (padding_length bytes)
 *   [MAC]   (none in plaintext mode)
 *
 * Returns payload in caller buffer, sets payload_len.
 * Returns 0 on success, -1 on error.
 * ---------------------------------------------------------------- */

int ssh_read_packet(ssh_session_t *s, uint8_t *payload, int *payload_len)
{
    if (s->encrypted)
        return ssh_read_encrypted(s, payload, payload_len);

    uint8_t hdr[4];

    /* Read 4-byte packet length */
    if (sock_read_exact(s->sockfd, hdr, 4) < 0) {
        return -1;
    }

    uint32_t pkt_len = ssh_get_u32(hdr);

    /* Sanity check */
    if (pkt_len < 2 || pkt_len > SSH_BUF_SIZE - 4) {
        printf("[sshd] bad packet_length: %u\n", (unsigned)pkt_len);
        return -1;
    }

    /* Read the rest of the packet (padding_length + payload + padding) */
    uint8_t body[SSH_BUF_SIZE];
    if (sock_read_exact(s->sockfd, body, (int)pkt_len) < 0) {
        printf("[sshd] packet body read failed\n");
        return -1;
    }

    uint8_t pad_len = body[0];
    int pload_len = (int)pkt_len - (int)pad_len - 1;

    if (pload_len < 0 || pload_len > SSH_MAX_PAYLOAD) {
        printf("[sshd] bad payload length: %d\n", pload_len);
        return -1;
    }

    memcpy(payload, body + 1, pload_len);
    *payload_len = pload_len;

    s->seq_recv++;
    return 0;
}

/* ----------------------------------------------------------------
 * Write one SSH binary packet (plaintext mode)
 *
 * Constructs: [packet_length][padding_length][payload][padding]
 * Padding: total (excl packet_length) aligned to 8, min 4 pad.
 * Writes complete packet in 900-byte chunks.
 * Returns 0 on success, -1 on error.
 * ---------------------------------------------------------------- */

int ssh_write_packet(ssh_session_t *s, const uint8_t *payload, int payload_len)
{
    if (s->encrypted)
        return ssh_write_encrypted(s, payload, payload_len);

    uint8_t pkt[SSH_BUF_SIZE];

    /* Calculate padding: total of (packet_length_field + padding_len_byte +
     * payload + padding) must be a multiple of 8.  RFC 4253 section 6. */
    int block = 8;
    int total_unpadded = 4 + 1 + payload_len;  /* 4-byte len + 1-byte padlen + payload */
    int pad_len = block - (total_unpadded % block);
    if (pad_len < 4) pad_len += block;

    uint32_t pkt_len = (uint32_t)(1 + payload_len + pad_len);

    /* Build packet */
    int off = 0;
    ssh_put_u32(pkt + off, pkt_len);    off += 4;
    pkt[off++] = (uint8_t)pad_len;
    memcpy(pkt + off, payload, payload_len);  off += payload_len;

    /* Random padding (use simple PRNG -- not security-critical in plaintext) */
    {
        uint64_t st;
        __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(st));
        int i;
        for (i = 0; i < pad_len; i++) {
            st += 0x9E3779B97F4A7C15ULL;
            uint64_t z = st;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            z = z ^ (z >> 31);
            pkt[off++] = (uint8_t)(z & 0xFF);
        }
    }

    /* Write complete packet */
    if (sock_write_all(s->sockfd, pkt, off) < 0) {
        printf("[sshd] packet write failed\n");
        return -1;
    }

    s->seq_send++;
    return 0;
}


/* ----------------------------------------------------------------
 * Non-blocking packet read (buffer-based)
 *
 * Socket must be in O_NONBLOCK mode. Accumulates raw bytes in
 * session inbuf. When a complete packet is available, decrypts
 * and returns it. Never blocks.
 *
 * Returns: 0 = packet read, 1 = incomplete/no data, -1 = error
 * ---------------------------------------------------------------- */

int ssh_read_packet_nb(ssh_session_t *s, uint8_t *payload, int *payload_len)
{
    /* Accumulate bytes from socket into inbuf */
    if (s->inbuf.len < SSH_BUF_SIZE) {
        int space = SSH_BUF_SIZE - s->inbuf.len;
        if (space > SOCK_CHUNK) space = SOCK_CHUNK;
        int got = (int)read(s->sockfd, s->inbuf.buf + s->inbuf.len, space);
        if (got > 0) s->inbuf.len += got;
        else if (got == 0) return -1;  /* EOF */
    }

    if (!s->encrypted) {
        /* Plaintext: 4-byte length header */
        if (s->inbuf.len < 4) return 1;
        uint32_t pkt_len = ssh_get_u32(s->inbuf.buf);
        if (pkt_len < 2 || pkt_len > SSH_BUF_SIZE - 4) return -1;
        int total = 4 + (int)pkt_len;
        if (s->inbuf.len < total) return 1;
        uint8_t pad_len = s->inbuf.buf[4];
        int pload_len = (int)pkt_len - (int)pad_len - 1;
        if (pload_len < 0 || pload_len > SSH_MAX_PAYLOAD) return -1;
        memcpy(payload, s->inbuf.buf + 5, pload_len);
        *payload_len = pload_len;
        s->seq_recv++;
        int remain = s->inbuf.len - total;
        if (remain > 0)
            __builtin_memmove(s->inbuf.buf, s->inbuf.buf + total, remain);
        s->inbuf.len = remain;
        return 0;
    }

    /* Encrypted: need 4-byte header first */
    if (s->inbuf.len < 4) return 1;

    /* Decrypt header once to learn packet length */
    if (!s->nb_hdr_done) {
        ssh_ctr_decrypt(s->inbuf.buf, s->nb_dec_hdr, 4);
        s->nb_pkt_len = ssh_get_u32(s->nb_dec_hdr);
        s->nb_hdr_done = 1;
        if (s->nb_pkt_len < 2 || s->nb_pkt_len > SSH_BUF_SIZE - 4)
            return -1;
    }

    /* Wait for full packet: header(4) + body(pkt_len) + MAC(32) */
    int total = 4 + (int)s->nb_pkt_len + SSH_MAC_LEN;
    if (s->inbuf.len < total) return 1;

    /* Decrypt body */
    uint8_t dec_body[SSH_BUF_SIZE];
    ssh_ctr_decrypt(s->inbuf.buf + 4, dec_body, (int)s->nb_pkt_len);

    /* Verify MAC: HMAC(key, seq(4) || dec_hdr(4) || dec_body(pkt_len)) */
    uint8_t mac_in[SSH_BUF_SIZE + 8];
    int mi = 0;
    ssh_put_u32(mac_in + mi, s->seq_recv);              mi += 4;
    __builtin_memcpy(mac_in + mi, s->nb_dec_hdr, 4);    mi += 4;
    __builtin_memcpy(mac_in + mi, dec_body, s->nb_pkt_len);
    mi += (int)s->nb_pkt_len;

    uint8_t *mac_recv = s->inbuf.buf + 4 + (int)s->nb_pkt_len;
    if (ssh_mac_verify_recv(mac_in, mi, mac_recv) < 0) {
        printf("[sshd] MAC FAILED (nb, seq %u)\n", (unsigned)s->seq_recv);
        return -1;
    }

    /* Extract payload */
    uint8_t pad_len = dec_body[0];
    int pload_len = (int)s->nb_pkt_len - (int)pad_len - 1;
    if (pload_len < 0 || pload_len > SSH_MAX_PAYLOAD) return -1;
    memcpy(payload, dec_body + 1, pload_len);
    *payload_len = pload_len;
    s->seq_recv++;

    /* Shift remaining bytes in inbuf */
    int remain = s->inbuf.len - total;
    if (remain > 0)
        __builtin_memmove(s->inbuf.buf, s->inbuf.buf + total, remain);
    s->inbuf.len = remain;
    s->nb_hdr_done = 0;

    return 0;
}


/* ----------------------------------------------------------------
 * Send SSH_MSG_DISCONNECT
 * ---------------------------------------------------------------- */

int ssh_disconnect(ssh_session_t *s, uint32_t reason, const char *desc)
{
    uint8_t payload[512];
    int off = 0;

    payload[off++] = SSH_MSG_DISCONNECT;
    ssh_put_u32(payload + off, reason);     off += 4;

    /* description string */
    int dlen = 0;
    if (desc) {
        const char *p = desc;
        while (*p) { dlen++; p++; }
    }
    ssh_put_string(payload, desc, (uint32_t)dlen, &off);

    /* language tag (empty) */
    ssh_put_u32(payload + off, 0);          off += 4;

    ssh_write_packet(s, payload, off);
    return 0;
}
