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
