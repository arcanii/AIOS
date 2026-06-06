/* nslookup.c -- minimal DNS A-record resolver for AIOS.
 *
 *   nslookup <hostname> [dns-server-ip]
 *
 * Sends a UDP DNS query (type A, class IN, recursion desired) to the server and
 * prints the resolved IPv4 address(es). Same socket path as sntp.c (UDP, bind,
 * O_NONBLOCK + poll recv). AIOS has no DHCP-captured resolver yet, so the server
 * defaults to a public one (8.8.8.8); pass it explicitly for a local resolver
 * (QEMU SLIRP DNS is 10.0.2.3). The query/parse core is the reusable bit a libc
 * gethostbyname() can later sit on top of.
 *
 * Build: ./scripts/aios-cc src/apps/nslookup.c -o build-04/sbase/nslookup
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DNS_PORT        53
#define DNS_LOCAL_PORT  50153
#define POLL_TRIES      200
#define POLL_NS         (5 * 1000 * 1000)   /* 5 ms -> ~1s total */

/* Encode "www.example.com" as DNS labels (3www7example3com0). Returns length. */
static int encode_qname(const char *host, uint8_t *out)
{
    int oi = 0;
    const char *p = host;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int len = (int)(dot - p);
        if (len <= 0 || len > 63) return -1;
        out[oi++] = (uint8_t)len;
        for (int i = 0; i < len; i++) out[oi++] = (uint8_t)p[i];
        p = (*dot == '.') ? dot + 1 : dot;
    }
    out[oi++] = 0;   /* root label */
    return oi;
}

/* Skip a DNS name (labels or a 0xC0 compression pointer) at msg[off]. */
static int skip_name(const uint8_t *msg, int off, int len)
{
    while (off < len) {
        uint8_t b = msg[off];
        if (b == 0) return off + 1;
        if ((b & 0xC0) == 0xC0) return off + 2;   /* compression pointer */
        off += 1 + b;
    }
    return off;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: nslookup <hostname> [dns-server-ip]\n");
        return 2;
    }
    const char *host = argv[1];
    const char *server = (argc > 2) ? argv[2] : "8.8.8.8";

    /* --- build the query --- */
    uint8_t q[512];
    int qi = 0;
    q[qi++] = 0x12; q[qi++] = 0x34;       /* ID */
    q[qi++] = 0x01; q[qi++] = 0x00;       /* flags: recursion desired */
    q[qi++] = 0x00; q[qi++] = 0x01;       /* QDCOUNT = 1 */
    q[qi++] = 0x00; q[qi++] = 0x00;       /* ANCOUNT = 0 */
    q[qi++] = 0x00; q[qi++] = 0x00;       /* NSCOUNT = 0 */
    q[qi++] = 0x00; q[qi++] = 0x00;       /* ARCOUNT = 0 */
    int nl = encode_qname(host, q + qi);
    if (nl < 0) { printf("nslookup: bad hostname\n"); return 2; }
    qi += nl;
    q[qi++] = 0x00; q[qi++] = 0x01;       /* QTYPE = A */
    q[qi++] = 0x00; q[qi++] = 0x01;       /* QCLASS = IN */

    /* --- send (mirror sntp.c) --- */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { printf("nslookup: socket failed\n"); return 1; }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(DNS_LOCAL_PORT);
    local.sin_addr.s_addr = 0;
    bind(fd, (struct sockaddr *)&local, sizeof(local));

    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(DNS_PORT);
    srv.sin_addr.s_addr = inet_addr(server);

    if (sendto(fd, q, qi, 0, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        printf("nslookup: send to %s failed\n", server); close(fd); return 1;
    }

    /* --- poll for the response --- */
    uint8_t r[512];
    int n = -1;
    for (int i = 0; i < POLL_TRIES; i++) {
        n = (int)recvfrom(fd, r, sizeof(r), 0, (void *)0, (void *)0);
        if (n > 12) break;
        struct timespec ts = { 0, POLL_NS };
        nanosleep(&ts, (void *)0);
    }
    close(fd);
    if (n <= 12) { printf("nslookup: no response from %s\n", server); return 1; }

    /* --- parse --- */
    int rcode = r[3] & 0x0F;
    int ancount = (r[6] << 8) | r[7];
    if (rcode != 0) { printf("nslookup: %s: server error (rcode=%d)\n", host, rcode); return 1; }
    if (ancount == 0) { printf("nslookup: %s has no A record\n", host); return 1; }

    int off = skip_name(r, 12, n) + 4;    /* past the echoed question */

    int found = 0;
    for (int a = 0; a < ancount && off + 10 <= n; a++) {
        off = skip_name(r, off, n);
        if (off + 10 > n) break;
        int type  = (r[off] << 8) | r[off + 1];
        int rdlen = (r[off + 8] << 8) | r[off + 9];
        off += 10;
        if (type == 1 && rdlen == 4 && off + 4 <= n) {
            printf("%s -> %u.%u.%u.%u\n", host, r[off], r[off + 1], r[off + 2], r[off + 3]);
            found++;
        }
        off += rdlen;
    }
    if (!found) { printf("nslookup: %s: no A record in answer\n", host); return 1; }
    return 0;
}
