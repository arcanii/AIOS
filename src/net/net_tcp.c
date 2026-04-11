/*
 * net_tcp.c -- TCP protocol handling
 *
 * Parses incoming TCP segments, dispatches to net_tcp_deliver()
 * in net_server.c for state machine processing. Builds outgoing
 * TCP segments with proper checksums.
 *
 * Ported from ref/v03x/src/net_server.c (handle_tcp, send_tcp).
 */
#include "aios/root_shared.h"
#include "aios/net.h"
#include "aios/config.h"
#include "virtio.h"
#include <stdio.h>

/* -- TCP pseudo-header checksum -- */
uint16_t tcp_checksum(const uint8_t *src_ip, const uint8_t *dst_ip,
                      const void *tcp_seg, int tcp_len) {
    uint32_t sum = 0;
    /* Pseudo-header: src_ip + dst_ip + proto + length */
    const uint16_t *s = (const uint16_t *)src_ip;
    sum += s[0]; sum += s[1];
    s = (const uint16_t *)dst_ip;
    sum += s[0]; sum += s[1];
    sum += be16(IP_PROTO_TCP);
    sum += be16((uint16_t)tcp_len);
    /* TCP segment */
    const uint16_t *p = (const uint16_t *)tcp_seg;
    int len = tcp_len;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* -- Build and send a TCP segment -- */
static uint8_t tcp_tx_frame[1518];

void net_tcp_send(const uint8_t *dst_ip, const uint8_t *dst_mac,
                  uint16_t src_port, uint16_t dst_port,
                  uint32_t seq, uint32_t ack_val, uint8_t flags,
                  const uint8_t *data, int data_len) {
    uint8_t my_ip[4] = { net_cfg_ip[0], net_cfg_ip[1], net_cfg_ip[2], net_cfg_ip[3] };
    int tcp_len = 20 + data_len;
    int ip_total = 20 + tcp_len;
    int frame_len = 14 + ip_total; /* ETH(14) + IP(20) + TCP(20) + data */

    if (frame_len > (int)sizeof(tcp_tx_frame)) return;

    /* Ethernet */
    struct eth_hdr *eth = (struct eth_hdr *)tcp_tx_frame;
    for (int i = 0; i < 6; i++) { eth->dst[i] = dst_mac[i]; eth->src[i] = net_mac[i]; }
    eth->type = be16(ETH_TYPE_IP);

    /* IPv4 */
    struct ip_hdr *ip = (struct ip_hdr *)(tcp_tx_frame + 14);
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = be16((uint16_t)ip_total);
    ip->id         = be16(1);
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = IP_PROTO_TCP;
    ip->checksum   = 0;
    for (int i = 0; i < 4; i++) { ip->src[i] = my_ip[i]; ip->dst[i] = dst_ip[i]; }
    ip->checksum = ip_checksum(ip, 20);

    /* TCP */
    uint8_t *seg = tcp_tx_frame + 14 + 20;
    struct tcp_hdr *tcp = (struct tcp_hdr *)seg;
    tcp->src_port = be16(src_port);
    tcp->dst_port = be16(dst_port);
    tcp->seq      = be32(seq);
    tcp->ack      = be32(ack_val);
    tcp->off_rsvd = (5 << 4); /* 20 bytes header, no options */
    tcp->flags    = flags;
    tcp->window   = be16(2048);
    tcp->cksum    = 0;
    tcp->urgent   = 0;
    if (data_len > 0) {
        for (int i = 0; i < data_len; i++) seg[20 + i] = data[i];
    }
    tcp->cksum = tcp_checksum(my_ip, dst_ip, seg, tcp_len);

    net_tx_send(tcp_tx_frame, (uint32_t)frame_len);
}

/* -- Parse incoming TCP segment, dispatch to net_server -- */
void handle_tcp(const uint8_t *pkt, uint32_t len,
                const struct ip_hdr *req_ip,
                const struct eth_hdr *req_eth) {
    if (len < sizeof(struct tcp_hdr)) return;

    const struct tcp_hdr *tcp = (const struct tcp_hdr *)pkt;
    int tcp_hlen = (tcp->off_rsvd >> 4) * 4;
    if (tcp_hlen < 20 || (uint32_t)tcp_hlen > len) return;

    uint16_t dst_port  = be16(tcp->dst_port);
    uint16_t src_port  = be16(tcp->src_port);
    uint32_t their_seq = be32(tcp->seq);
    uint32_t their_ack = be32(tcp->ack);
    uint8_t  flags     = tcp->flags;

    const uint8_t *data = pkt + tcp_hlen;
    int data_len = (int)len - tcp_hlen;
    if (data_len < 0) data_len = 0;

    net_tcp_deliver(req_ip->src, req_eth->src,
                    src_port, dst_port,
                    their_seq, their_ack, flags,
                    data, data_len);
}
