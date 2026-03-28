/*
 * AIOS Network Server – IP/TCP/UDP stack
 *
 * Minimal network stack for AIOS. Handles:
 * - ARP (request/reply)
 * - IPv4
 * - ICMP (ping)
 * - UDP
 * - TCP (basic, for HTTP)
 * - HTTP server (port 80) for status page
 */
#include <stdint.h>
#include <microkit.h>
#include "aios/channels.h"
#include "aios/ipc.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

uintptr_t net_data;

/* ── Network configuration ───────────────────────────── */
static uint8_t my_mac[6];
static uint8_t my_ip[4]      = {10, 0, 2, 15};   /* QEMU user-mode default */
static uint8_t gateway_ip[4] = {10, 0, 2, 2};
static uint8_t netmask[4]    = {255, 255, 255, 0};

/* Check if IP is on our local subnet */
static __attribute__((unused)) int is_local_subnet(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++)
        if ((ip[i] & netmask[i]) != (my_ip[i] & netmask[i])) return 0;
    return 1;
}
static __attribute__((unused)) const uint8_t *next_hop(const uint8_t dst[4]) {
    return is_local_subnet(dst) ? dst : gateway_ip;
}


/* ── Helpers ─────────────────────────────────────────── */
static void my_memcpy(void *d, const void *s, uint32_t n) {
    uint8_t *dd = d; const uint8_t *ss = s; while (n--) *dd++ = *ss++;
}
static __attribute__((unused)) void my_memset(void *d, int c, uint32_t n) {
    uint8_t *dd = d; while (n--) *dd++ = c;
}
static int my_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *aa = a, *bb = b;
    while (n--) { if (*aa != *bb) return *aa - *bb; aa++; bb++; }
    return 0;
}

static uint16_t htons(uint16_t x) { return (x >> 8) | (x << 8); }
static uint16_t ntohs(uint16_t x) { return htons(x); }
static uint32_t htonl(uint32_t x) {
    return ((x >> 24) & 0xff) | ((x >> 8) & 0xff00) |
           ((x << 8) & 0xff0000) | ((x << 24) & 0xff000000);
}
static uint32_t ntohl(uint32_t x) { return htonl(x); }

/* ── Ethernet ────────────────────────────────────────── */
#define ETH_HDR_LEN  14
#define ETH_ARP      0x0806
#define ETH_IP       0x0800

struct eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} __attribute__((packed));

/* ── ARP ─────────────────────────────────────────────── */
#define ARP_REQUEST 1
#define ARP_REPLY   2

struct arp_pkt {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} __attribute__((packed));

/* ── IP ──────────────────────────────────────────────── */
struct ip_hdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t len;
    uint16_t id;
    uint16_t frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t cksum;
    uint8_t  src[4];
    uint8_t  dst[4];
} __attribute__((packed));

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

/* ── ICMP ────────────────────────────────────────────── */
struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t cksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

/* ── TCP ─────────────────────────────────────────────── */
struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  off_rsvd;  /* data offset in high 4 bits */
    uint8_t  flags;
    uint16_t window;
    uint16_t cksum;
    uint16_t urgent;
} __attribute__((packed));

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* ── Packet buffer ───────────────────────────────────── */
static uint8_t pkt_buf[2048];
static uint8_t tx_buf[2048];

/* ── TCP connection state ────────────────────────────── */
#define TCP_STATE_CLOSED   0
#define TCP_STATE_LISTEN   1
#define TCP_STATE_SYN_RCVD 2
#define TCP_STATE_ESTAB    3
#define TCP_STATE_FIN_WAIT 4

struct tcp_conn {
    int     state;
    uint8_t remote_ip[4];
    uint8_t remote_mac[6];
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
};

static struct tcp_conn http_conn = { .state = TCP_STATE_LISTEN, .local_port = 80 };

/* ── Checksum ────────────────────────────────────────── */
static uint16_t ip_checksum(const void *data, int len) {
    const uint16_t *p = data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return ~sum;
}

static uint16_t tcp_checksum(const uint8_t *src_ip, const uint8_t *dst_ip,
                             const void *tcp_seg, int tcp_len) {
    uint32_t sum = 0;
    /* Pseudo-header */
    const uint16_t *s = (const uint16_t *)src_ip;
    sum += s[0]; sum += s[1];
    s = (const uint16_t *)dst_ip;
    sum += s[0]; sum += s[1];
    sum += htons(IP_PROTO_TCP);
    sum += htons(tcp_len);
    /* TCP segment */
    const uint16_t *p = tcp_seg;
    int len = tcp_len;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return ~sum;
}

/* ── Send packet via net_driver ──────────────────────── */
static void send_frame(const uint8_t *data, uint32_t len) {
    volatile uint8_t *dst = (volatile uint8_t *)(net_data + NET_PKT_DATA);
    for (uint32_t i = 0; i < len; i++) dst[i] = data[i];
    *(volatile uint32_t *)(net_data + NET_PKT_LEN) = len;
    *(volatile uint32_t *)(net_data + NET_CMD) = NET_CMD_SEND;
    microkit_notify(CH_NET);
}

/* ── Build Ethernet + IP header ──────────────────────── */
static int build_ip_packet(uint8_t *buf, const uint8_t *dst_mac,
                           const uint8_t *dst_ip, uint8_t proto,
                           const uint8_t *payload, int payload_len) {
    struct eth_hdr *eth = (struct eth_hdr *)buf;
    my_memcpy(eth->dst, dst_mac, 6);
    my_memcpy(eth->src, my_mac, 6);
    eth->type = htons(ETH_IP);

    struct ip_hdr *ip = (struct ip_hdr *)(buf + ETH_HDR_LEN);
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->len = htons(20 + payload_len);
    ip->id = htons(1);
    ip->frag = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->cksum = 0;
    my_memcpy(ip->src, my_ip, 4);
    my_memcpy(ip->dst, dst_ip, 4);
    ip->cksum = ip_checksum(ip, 20);

    my_memcpy(buf + ETH_HDR_LEN + 20, payload, payload_len);
    return ETH_HDR_LEN + 20 + payload_len;
}

/* ── Handle ARP ──────────────────────────────────────── */
static void handle_arp(const uint8_t *pkt, uint32_t len) {
    if (len < ETH_HDR_LEN + sizeof(struct arp_pkt)) return;
    struct eth_hdr *eth = (struct eth_hdr *)pkt;
    struct arp_pkt *arp = (struct arp_pkt *)(pkt + ETH_HDR_LEN);

    if (ntohs(arp->oper) != ARP_REQUEST) return;
    if (my_memcmp(arp->tpa, my_ip, 4) != 0) return;

    /* Build ARP reply */
    struct eth_hdr *reth = (struct eth_hdr *)tx_buf;
    my_memcpy(reth->dst, eth->src, 6);
    my_memcpy(reth->src, my_mac, 6);
    reth->type = htons(ETH_ARP);

    struct arp_pkt *rarp = (struct arp_pkt *)(tx_buf + ETH_HDR_LEN);
    rarp->htype = htons(1);
    rarp->ptype = htons(0x0800);
    rarp->hlen = 6;
    rarp->plen = 4;
    rarp->oper = htons(ARP_REPLY);
    my_memcpy(rarp->sha, my_mac, 6);
    my_memcpy(rarp->spa, my_ip, 4);
    my_memcpy(rarp->tha, arp->sha, 6);
    my_memcpy(rarp->tpa, arp->spa, 4);

    send_frame(tx_buf, ETH_HDR_LEN + sizeof(struct arp_pkt));
}

/* ── Handle ICMP ─────────────────────────────────────── */
static void handle_icmp(const uint8_t *pkt, uint32_t len) {
    struct eth_hdr *eth = (struct eth_hdr *)pkt;
    struct ip_hdr *ip = (struct ip_hdr *)(pkt + ETH_HDR_LEN);
    int ip_hlen = (ip->ver_ihl & 0xf) * 4;
    struct icmp_hdr *icmp = (struct icmp_hdr *)(pkt + ETH_HDR_LEN + ip_hlen);

    if (icmp->type != 8) return; /* only echo request */

    int icmp_len = ntohs(ip->len) - ip_hlen;

    /* Build echo reply */
    uint8_t icmp_reply[1500];
    my_memcpy(icmp_reply, icmp, icmp_len);
    ((struct icmp_hdr *)icmp_reply)->type = 0; /* echo reply */
    ((struct icmp_hdr *)icmp_reply)->cksum = 0;
    ((struct icmp_hdr *)icmp_reply)->cksum = ip_checksum(icmp_reply, icmp_len);

    int total = build_ip_packet(tx_buf, eth->src, ip->src, IP_PROTO_ICMP,
                                icmp_reply, icmp_len);
    send_frame(tx_buf, total);
}

/* ── HTTP response ───────────────────────────────────── */
static const char http_response[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html><html><head><title>AIOS Status</title>"
    "<style>body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;padding:20px;}"
    "h1{color:#00ff88;}table{border-collapse:collapse;}"
    "td,th{border:1px solid #333;padding:8px;}"
    "</style></head><body>"
    "<h1>AIOS System Status</h1>"
    "<table>"
    "<tr><th>Component</th><th>Status</th></tr>"
    "<tr><td>Kernel</td><td>seL4 14.0.0</td></tr>"
    "<tr><td>Framework</td><td>Microkit 2.1.0</td></tr>"
    "<tr><td>Arch</td><td>AArch64 (Cortex-A53)</td></tr>"
    "<tr><td>Network</td><td>virtio-net (active)</td></tr>"
    "<tr><td>Filesystem</td><td>FAT16 (128 MiB)</td></tr>"
    "<tr><td>LLM</td><td>25M param C code model</td></tr>"
    "<tr><td>Shell</td><td>Interactive (PPC syscalls)</td></tr>"
    "</table>"
    "<p>AIOS — AI Operating System on seL4 microkernel</p>"
    "</body></html>";

static int http_response_len = sizeof(http_response) - 1;

/* ── Handle TCP ──────────────────────────────────────── */
static void send_tcp(const uint8_t *dst_mac, const uint8_t *dst_ip,
                     uint16_t src_port, uint16_t dst_port,
                     uint32_t seq, uint32_t ack, uint8_t flags,
                     const uint8_t *data, int data_len) {
    uint8_t seg[1500];
    struct tcp_hdr *tcp = (struct tcp_hdr *)seg;
    tcp->src_port = htons(src_port);
    tcp->dst_port = htons(dst_port);
    tcp->seq = htonl(seq);
    tcp->ack = htonl(ack);
    tcp->off_rsvd = (5 << 4); /* 20 bytes, no options */
    tcp->flags = flags;
    tcp->window = htons(4096);
    tcp->cksum = 0;
    tcp->urgent = 0;
    if (data_len > 0) my_memcpy(seg + 20, data, data_len);

    int tcp_len = 20 + data_len;
    tcp->cksum = tcp_checksum(my_ip, dst_ip, seg, tcp_len);

    int total = build_ip_packet(tx_buf, dst_mac, dst_ip, IP_PROTO_TCP, seg, tcp_len);
    send_frame(tx_buf, total);
}

static void handle_tcp(const uint8_t *pkt, uint32_t len) {
    struct eth_hdr *eth = (struct eth_hdr *)pkt;
    struct ip_hdr *ip = (struct ip_hdr *)(pkt + ETH_HDR_LEN);
    int ip_hlen = (ip->ver_ihl & 0xf) * 4;
    struct tcp_hdr *tcp = (struct tcp_hdr *)(pkt + ETH_HDR_LEN + ip_hlen);
    int tcp_hlen = (tcp->off_rsvd >> 4) * 4;
    int ip_total = ntohs(ip->len);
    int tcp_data_len = ip_total - ip_hlen - tcp_hlen;

    uint16_t dst_port = ntohs(tcp->dst_port);
    uint16_t src_port = ntohs(tcp->src_port);
    uint32_t their_seq = ntohl(tcp->seq);
    (void)ntohl(tcp->ack);

    if (dst_port != 80) return; /* only HTTP */

    struct tcp_conn *c = &http_conn;

    if (tcp->flags & TCP_RST) {
        c->state = TCP_STATE_LISTEN;
        return;
    }

    if (tcp->flags & TCP_SYN) {
        /* SYN received — send SYN+ACK */
        my_memcpy(c->remote_ip, ip->src, 4);
        my_memcpy(c->remote_mac, eth->src, 6);
        c->remote_port = src_port;
        c->snd_nxt = 1000;
        c->rcv_nxt = their_seq + 1;
        c->state = TCP_STATE_SYN_RCVD;

        send_tcp(c->remote_mac, c->remote_ip, 80, c->remote_port,
                 c->snd_nxt, c->rcv_nxt, TCP_SYN | TCP_ACK, NULL, 0);
        c->snd_nxt++;
        return;
    }

    if (c->state == TCP_STATE_SYN_RCVD && (tcp->flags & TCP_ACK)) {
        c->state = TCP_STATE_ESTAB;
    }

    if (c->state == TCP_STATE_ESTAB) {
        if (tcp_data_len > 0) {
            c->rcv_nxt += tcp_data_len;

            /* Send HTTP response */
            send_tcp(c->remote_mac, c->remote_ip, 80, c->remote_port,
                     c->snd_nxt, c->rcv_nxt, TCP_ACK | TCP_PSH,
                     (const uint8_t *)http_response, http_response_len);
            c->snd_nxt += http_response_len;

            /* Send FIN */
            send_tcp(c->remote_mac, c->remote_ip, 80, c->remote_port,
                     c->snd_nxt, c->rcv_nxt, TCP_ACK | TCP_FIN, NULL, 0);
            c->snd_nxt++;
            c->state = TCP_STATE_FIN_WAIT;
        }
    }

    if (c->state == TCP_STATE_FIN_WAIT && (tcp->flags & (TCP_FIN | TCP_ACK))) {
        if (tcp->flags & TCP_FIN) {
            c->rcv_nxt++;
            send_tcp(c->remote_mac, c->remote_ip, 80, c->remote_port,
                     c->snd_nxt, c->rcv_nxt, TCP_ACK, NULL, 0);
        }
        c->state = TCP_STATE_LISTEN;
    }
}

/* ── Handle IP ───────────────────────────────────────── */
static void handle_ip(const uint8_t *pkt, uint32_t len) {
    struct ip_hdr *ip = (struct ip_hdr *)(pkt + ETH_HDR_LEN);
    if (my_memcmp(ip->dst, my_ip, 4) != 0) return;

    switch (ip->proto) {
    case IP_PROTO_ICMP: handle_icmp(pkt, len); break;
    case IP_PROTO_TCP:  handle_tcp(pkt, len); break;
    default: break;
    }
}

/* ── Main packet handler ─────────────────────────────── */
static void handle_packet(const uint8_t *pkt, uint32_t len) {
    if (len < ETH_HDR_LEN) return;
    struct eth_hdr *eth = (struct eth_hdr *)pkt;
    uint16_t type = ntohs(eth->type);

    switch (type) {
    case ETH_ARP: handle_arp(pkt, len); break;
    case ETH_IP:  handle_ip(pkt, len); break;
    default: break;
    }
}

/* ── Microkit entry points ───────────────────────────── */

void init(void) {
    /* Get MAC from net_driver */
    *(volatile uint32_t *)(net_data + NET_CMD) = NET_CMD_GET_MAC;
    microkit_notify(CH_NET);
    /* Small delay for driver to respond */
    for (volatile int i = 0; i < 100000; i++);
    volatile uint8_t *mac = (volatile uint8_t *)(net_data + NET_MAC_OFF);
    for (int i = 0; i < 6; i++) my_mac[i] = mac[i];

    microkit_dbg_puts("NET_SRV: IP stack ready, IP=10.0.2.15\n");
}

void notified(microkit_channel ch) {
    if (ch == CH_NET) {
        uint32_t cmd = *(volatile uint32_t *)(net_data + NET_CMD);
        if (cmd == NET_CMD_RECV) {
            uint32_t pkt_len = *(volatile uint32_t *)(net_data + NET_PKT_LEN);
            volatile uint8_t *src = (volatile uint8_t *)(net_data + NET_PKT_DATA);
            if (pkt_len > 0 && pkt_len <= sizeof(pkt_buf)) {
                for (uint32_t i = 0; i < pkt_len; i++) pkt_buf[i] = src[i];
                handle_packet(pkt_buf, pkt_len);
            }
        }
    }
}
