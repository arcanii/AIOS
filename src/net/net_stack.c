/*
 * net_stack.c -- Ethernet/ARP/IPv4/ICMP protocol handlers
 *
 * Custom IP stack for AIOS. Processes packets from the rx_ring
 * and generates replies via the TX virtqueue.
 *
 * M2: ARP request/reply, ICMP echo request/reply, selftest ping.
 */
#include "aios/root_shared.h"
#include "aios/net.h"
#include "aios/config.h"
#include "plat/net_hal.h"
#include <stdio.h>
#include "arch.h"

/* be16/be32 provided by net.h */

/* -- Internet checksum (RFC 1071) -- */
uint16_t ip_checksum(const void *data, int len) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* -- Static state -- */
static uint8_t *my_ip = net_cfg_ip;  /* v0.4.80: runtime from /etc/network.conf */
static uint8_t tx_frame[1518];
static struct net_stats stats;

/* ============================================================
 * ARP cache
 * ============================================================ */
static struct arp_entry arp_cache[ARP_CACHE_SIZE];

static void arp_cache_add(const uint8_t *ip, const uint8_t *mac) {
    /* Update existing or find empty slot */
    int empty = -1;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid &&
            arp_cache[i].ip[0] == ip[0] && arp_cache[i].ip[1] == ip[1] &&
            arp_cache[i].ip[2] == ip[2] && arp_cache[i].ip[3] == ip[3]) {
            for (int j = 0; j < 6; j++) arp_cache[i].mac[j] = mac[j];
            return;
        }
        if (!arp_cache[i].valid && empty < 0) empty = i;
    }
    if (empty >= 0) {
        arp_cache[empty].valid = 1;
        for (int j = 0; j < 4; j++) arp_cache[empty].ip[j] = ip[j];
        for (int j = 0; j < 6; j++) arp_cache[empty].mac[j] = mac[j];
    }
}

int net_arp_resolved(const uint8_t *ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid &&
            arp_cache[i].ip[0] == ip[0] && arp_cache[i].ip[1] == ip[1] &&
            arp_cache[i].ip[2] == ip[2] && arp_cache[i].ip[3] == ip[3])
            return 1;
    }
    return 0;
}

int arp_cache_lookup(const uint8_t *ip, uint8_t *mac_out) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid &&
            arp_cache[i].ip[0] == ip[0] && arp_cache[i].ip[1] == ip[1] &&
            arp_cache[i].ip[2] == ip[2] && arp_cache[i].ip[3] == ip[3]) {
            for (int j = 0; j < 6; j++) mac_out[j] = arp_cache[i].mac[j];
            return 0;
        }
    }
    return -1;
}

/* ============================================================
 * TX: send an Ethernet frame via virtio-net TX queue
 * ============================================================ */
int net_tx_send(const uint8_t *frame, uint32_t len) {
    int ret = plat_net_tx(frame, len);
    if (ret == 0) {
        stats.tx_packets++;
        stats.tx_bytes += len;
    }
    return ret;
}

/* ============================================================
 * ARP: handle requests (reply) and replies (cache)
 * ============================================================ */
static void handle_arp(const uint8_t *pkt, uint32_t len,
                       const struct eth_hdr *req_eth) {
    if (len < sizeof(struct arp_pkt)) return;
    const struct arp_pkt *arp = (const struct arp_pkt *)pkt;
    uint16_t op = be16(arp->op);

    if (op == ARP_OP_REPLY) {
        /* Cache the sender MAC/IP from the reply */
        arp_cache_add(arp->sender_ip, arp->sender_mac);
        printf("[net] ARP reply: %d.%d.%d.%d -> %02x:%02x:%02x:%02x:%02x:%02x\n",
               arp->sender_ip[0], arp->sender_ip[1],
               arp->sender_ip[2], arp->sender_ip[3],
               arp->sender_mac[0], arp->sender_mac[1],
               arp->sender_mac[2], arp->sender_mac[3],
               arp->sender_mac[4], arp->sender_mac[5]);
        return;
    }

    if (op != ARP_OP_REQUEST) return;

    /* Only reply if target IP matches ours */
    if (arp->target_ip[0] != my_ip[0] || arp->target_ip[1] != my_ip[1] ||
        arp->target_ip[2] != my_ip[2] || arp->target_ip[3] != my_ip[3])
        return;

    stats.arp_requests++;

    /* Build ARP reply */
    struct eth_hdr *eth = (struct eth_hdr *)tx_frame;
    struct arp_pkt *rep = (struct arp_pkt *)(tx_frame + sizeof(struct eth_hdr));

    for (int i = 0; i < 6; i++) {
        eth->dst[i] = arp->sender_mac[i];
        eth->src[i] = net_mac[i];
    }
    eth->type = be16(ETH_TYPE_ARP);

    rep->hw_type    = be16(1);
    rep->proto_type = be16(0x0800);
    rep->hw_len     = 6;
    rep->proto_len  = 4;
    rep->op         = be16(ARP_OP_REPLY);
    for (int i = 0; i < 6; i++) rep->sender_mac[i] = net_mac[i];
    for (int i = 0; i < 4; i++) rep->sender_ip[i]  = my_ip[i];
    for (int i = 0; i < 6; i++) rep->target_mac[i] = arp->sender_mac[i];
    for (int i = 0; i < 4; i++) rep->target_ip[i]  = arp->sender_ip[i];

    net_tx_send(tx_frame, sizeof(struct eth_hdr) + sizeof(struct arp_pkt));
    stats.arp_replies++;
    printf("[net] ARP who-has reply -> %d.%d.%d.%d\n",
           arp->sender_ip[0], arp->sender_ip[1],
           arp->sender_ip[2], arp->sender_ip[3]);
}

/* ============================================================
 * ICMP: echo reply (respond to pings) + echo reply handler
 * ============================================================ */
static void handle_icmp(const uint8_t *pkt, uint32_t len,
                        const struct ip_hdr *req_ip,
                        const struct eth_hdr *req_eth) {
    if (len < sizeof(struct icmp_hdr)) return;
    const struct icmp_hdr *icmp = (const struct icmp_hdr *)pkt;

    if (icmp->type == ICMP_ECHO_REPLY) {
        /* Reply to a ping WE sent */
        printf("[net] PING reply from %d.%d.%d.%d seq=%u\n",
               req_ip->src[0], req_ip->src[1],
               req_ip->src[2], req_ip->src[3],
               be16(icmp->seq));
        stats.icmp_echo++;
        return;
    }

    if (icmp->type != ICMP_ECHO_REQUEST) return;

    stats.icmp_echo++;

    uint32_t icmp_total = len;
    uint32_t ip_total   = sizeof(struct ip_hdr) + icmp_total;
    uint32_t frame_len  = sizeof(struct eth_hdr) + ip_total;
    if (frame_len > sizeof(tx_frame)) return;

    struct eth_hdr *eth = (struct eth_hdr *)tx_frame;
    for (int i = 0; i < 6; i++) {
        eth->dst[i] = req_eth->src[i];
        eth->src[i] = net_mac[i];
    }
    eth->type = be16(ETH_TYPE_IP);

    struct ip_hdr *ip = (struct ip_hdr *)(tx_frame + sizeof(struct eth_hdr));
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = be16((uint16_t)ip_total);
    ip->id         = req_ip->id;
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = IP_PROTO_ICMP;
    ip->checksum   = 0;
    for (int i = 0; i < 4; i++) {
        ip->src[i] = my_ip[i];
        ip->dst[i] = req_ip->src[i];
    }
    ip->checksum = ip_checksum(ip, sizeof(struct ip_hdr));

    uint8_t *rep_icmp = tx_frame + sizeof(struct eth_hdr) + sizeof(struct ip_hdr);
    for (uint32_t i = 0; i < icmp_total; i++) rep_icmp[i] = pkt[i];
    rep_icmp[0] = ICMP_ECHO_REPLY;
    rep_icmp[2] = 0; rep_icmp[3] = 0;
    uint16_t ck = ip_checksum(rep_icmp, icmp_total);
    rep_icmp[2] = ((uint8_t *)&ck)[0];
    rep_icmp[3] = ((uint8_t *)&ck)[1];

    net_tx_send(tx_frame, frame_len);
    printf("[net] ICMP echo reply -> %d.%d.%d.%d\n",
           req_ip->src[0], req_ip->src[1],
           req_ip->src[2], req_ip->src[3]);
}

/* ============================================================
 * UDP handler: deliver to net_server socket table
 * ============================================================ */
static void handle_udp(const uint8_t *pkt, uint32_t len,
                       const struct ip_hdr *ip,
                       const struct eth_hdr *eth) {
    if (len < sizeof(struct udp_hdr)) return;
    const struct udp_hdr *udp = (const struct udp_hdr *)pkt;

    uint16_t dst_port = be16(udp->dst_port);
    uint16_t src_port = be16(udp->src_port);
    uint16_t udp_len  = be16(udp->length);
    if (udp_len < sizeof(struct udp_hdr)) return;

    const uint8_t *payload = pkt + sizeof(struct udp_hdr);
    uint32_t payload_len = udp_len - sizeof(struct udp_hdr);
    if (payload_len > len - sizeof(struct udp_hdr))
        payload_len = len - sizeof(struct udp_hdr);

    net_udp_deliver(dst_port, src_port, ip->src, payload, payload_len);
    stats.udp_datagrams++;
}

/* ============================================================
 * IPv4: dispatch by protocol
 * ============================================================ */
static void handle_ipv4(const uint8_t *pkt, uint32_t len,
                        const struct eth_hdr *eth) {
    if (len < sizeof(struct ip_hdr)) return;
    const struct ip_hdr *ip = (const struct ip_hdr *)pkt;

    if ((ip->ver_ihl >> 4) != 4) return;

    if (ip->dst[0] != my_ip[0] || ip->dst[1] != my_ip[1] ||
        ip->dst[2] != my_ip[2] || ip->dst[3] != my_ip[3])
        return;

    uint32_t ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl > len) return;

    /* Use IP total_len for payload size (not Ethernet length which
     * includes padding for frames shorter than 60 bytes). Without this,
     * TCP handshake ACKs appear to carry 6 bytes of garbage padding. */
    uint32_t ip_total = be16(ip->total_len);
    uint32_t ip_payload = (ip_total > ihl) ? ip_total - ihl : 0;
    if (ip_payload > len - ihl) ip_payload = len - ihl;

    switch (ip->protocol) {
        case IP_PROTO_ICMP:
            handle_icmp(pkt + ihl, ip_payload, ip, eth);
            break;
        case IP_PROTO_UDP:
            handle_udp(pkt + ihl, ip_payload, ip, eth);
            break;
        case IP_PROTO_TCP:
            handle_tcp(pkt + ihl, ip_payload, ip, eth);
            break;
    }
}

/* ============================================================
 * Main packet dispatcher
 * ============================================================ */
void net_handle_packet(const uint8_t *data, uint32_t len) {
    if (len < sizeof(struct eth_hdr)) return;

    stats.rx_packets++;
    stats.rx_bytes += len;

    const struct eth_hdr *eth = (const struct eth_hdr *)data;
    uint16_t type = be16(eth->type);
    const uint8_t *payload = data + sizeof(struct eth_hdr);
    uint32_t payload_len = len - sizeof(struct eth_hdr);

    switch (type) {
        case ETH_TYPE_ARP:
            handle_arp(payload, payload_len, eth);
            break;
        case ETH_TYPE_IP:
            handle_ipv4(payload, payload_len, eth);
            break;
    }
}

/* ============================================================
 * Gratuitous ARP -- announce our MAC on startup
 * ============================================================ */
void net_send_gratuitous_arp(void) {
    struct eth_hdr *eth = (struct eth_hdr *)tx_frame;
    struct arp_pkt *arp = (struct arp_pkt *)(tx_frame + sizeof(struct eth_hdr));

    for (int i = 0; i < 6; i++) eth->dst[i] = 0xFF;
    for (int i = 0; i < 6; i++) eth->src[i] = net_mac[i];
    eth->type = be16(ETH_TYPE_ARP);

    arp->hw_type    = be16(1);
    arp->proto_type = be16(0x0800);
    arp->hw_len     = 6;
    arp->proto_len  = 4;
    arp->op         = be16(ARP_OP_REQUEST);
    for (int i = 0; i < 6; i++) arp->sender_mac[i] = net_mac[i];
    arp->sender_ip[0] = net_cfg_ip[0]; arp->sender_ip[1] = net_cfg_ip[1];
    arp->sender_ip[2] = net_cfg_ip[2]; arp->sender_ip[3] = net_cfg_ip[3];
    for (int i = 0; i < 6; i++) arp->target_mac[i] = 0x00;
    arp->target_ip[0] = net_cfg_ip[0]; arp->target_ip[1] = net_cfg_ip[1];
    arp->target_ip[2] = net_cfg_ip[2]; arp->target_ip[3] = net_cfg_ip[3];

    net_tx_send(tx_frame, sizeof(struct eth_hdr) + sizeof(struct arp_pkt));
    printf("[net] Gratuitous ARP sent for %d.%d.%d.%d\n",
           net_cfg_ip[0], net_cfg_ip[1], net_cfg_ip[2], net_cfg_ip[3]);
}

/* ============================================================
 * ARP request -- resolve a target IP
 * ============================================================ */
void net_send_arp_request(const uint8_t *target_ip) {
    struct eth_hdr *eth = (struct eth_hdr *)tx_frame;
    struct arp_pkt *arp = (struct arp_pkt *)(tx_frame + sizeof(struct eth_hdr));

    for (int i = 0; i < 6; i++) eth->dst[i] = 0xFF;
    for (int i = 0; i < 6; i++) eth->src[i] = net_mac[i];
    eth->type = be16(ETH_TYPE_ARP);

    arp->hw_type    = be16(1);
    arp->proto_type = be16(0x0800);
    arp->hw_len     = 6;
    arp->proto_len  = 4;
    arp->op         = be16(ARP_OP_REQUEST);
    for (int i = 0; i < 6; i++) arp->sender_mac[i] = net_mac[i];
    arp->sender_ip[0] = net_cfg_ip[0]; arp->sender_ip[1] = net_cfg_ip[1];
    arp->sender_ip[2] = net_cfg_ip[2]; arp->sender_ip[3] = net_cfg_ip[3];
    for (int i = 0; i < 6; i++) arp->target_mac[i] = 0x00;
    for (int i = 0; i < 4; i++) arp->target_ip[i] = target_ip[i];

    net_tx_send(tx_frame, sizeof(struct eth_hdr) + sizeof(struct arp_pkt));
    printf("[net] ARP request: who has %d.%d.%d.%d?\n",
           target_ip[0], target_ip[1], target_ip[2], target_ip[3]);
}

/* ============================================================
 * ICMP echo request -- ping a target
 * ============================================================ */
void net_send_ping(const uint8_t *dst_ip) {
    uint8_t dst_mac[6];
    if (arp_cache_lookup(dst_ip, dst_mac) != 0) {
        printf("[net] ping: no ARP entry for %d.%d.%d.%d\n",
               dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
        return;
    }

    uint32_t payload_sz = 32;
    uint32_t icmp_len = sizeof(struct icmp_hdr) + payload_sz;
    uint32_t ip_total = sizeof(struct ip_hdr) + icmp_len;
    uint32_t frame_len = sizeof(struct eth_hdr) + ip_total;

    struct eth_hdr *eth = (struct eth_hdr *)tx_frame;
    for (int i = 0; i < 6; i++) { eth->dst[i] = dst_mac[i]; eth->src[i] = net_mac[i]; }
    eth->type = be16(ETH_TYPE_IP);

    struct ip_hdr *ip = (struct ip_hdr *)(tx_frame + sizeof(struct eth_hdr));
    ip->ver_ihl = 0x45; ip->tos = 0;
    ip->total_len = be16((uint16_t)ip_total);
    ip->id = be16(1); ip->flags_frag = 0;
    ip->ttl = 64; ip->protocol = IP_PROTO_ICMP; ip->checksum = 0;
    ip->src[0] = net_cfg_ip[0]; ip->src[1] = net_cfg_ip[1];
    ip->src[2] = net_cfg_ip[2]; ip->src[3] = net_cfg_ip[3];
    for (int i = 0; i < 4; i++) ip->dst[i] = dst_ip[i];
    ip->checksum = ip_checksum(ip, sizeof(struct ip_hdr));

    uint8_t *icmp = tx_frame + sizeof(struct eth_hdr) + sizeof(struct ip_hdr);
    icmp[0] = ICMP_ECHO_REQUEST; icmp[1] = 0;
    icmp[2] = 0; icmp[3] = 0;
    icmp[4] = 0xAB; icmp[5] = 0xCD;  /* id = 0xABCD */
    icmp[6] = 0x00; icmp[7] = 0x01;  /* seq = 1 */
    for (uint32_t i = 8; i < icmp_len; i++) icmp[i] = (uint8_t)(i & 0xFF);
    uint16_t ck = ip_checksum(icmp, icmp_len);
    icmp[2] = ((uint8_t *)&ck)[0];
    icmp[3] = ((uint8_t *)&ck)[1];

    net_tx_send(tx_frame, frame_len);
    printf("[net] PING %d.%d.%d.%d seq=1\n",
           dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
}
/* ============================================================
 * UDP send: build ETH+IP+UDP frame and transmit
 * ============================================================ */
int net_udp_send(int sock_id, uint16_t local_port,
                 uint32_t dst_ip_word, uint16_t dst_port,
                 const uint8_t *data, int len) {
    uint8_t dst_ip[4] = {
        (dst_ip_word >> 24) & 0xFF, (dst_ip_word >> 16) & 0xFF,
        (dst_ip_word >> 8)  & 0xFF,  dst_ip_word        & 0xFF
    };

    /* Resolve MAC (use gateway if not on local subnet) */
    uint8_t dst_mac[6];
    uint8_t gw[4] = { net_cfg_gw[0], net_cfg_gw[1], net_cfg_gw[2], net_cfg_gw[3] };
    const uint8_t *lookup_ip = dst_ip;
    /* Simple: always route through gateway for now */
    if (arp_cache_lookup(dst_ip, dst_mac) != 0) {
        lookup_ip = gw;
        if (arp_cache_lookup(gw, dst_mac) != 0)
            return -1; /* no route */
    }

    uint32_t udp_total = sizeof(struct udp_hdr) + len;
    uint32_t ip_total  = sizeof(struct ip_hdr) + udp_total;
    uint32_t frame_len = sizeof(struct eth_hdr) + ip_total;
    if (frame_len > 1518) return -1;

    /* Ethernet */
    struct eth_hdr *eth = (struct eth_hdr *)tx_frame;
    for (int i = 0; i < 6; i++) { eth->dst[i] = dst_mac[i]; eth->src[i] = net_mac[i]; }
    eth->type = be16(ETH_TYPE_IP);

    /* IPv4 */
    struct ip_hdr *ip = (struct ip_hdr *)(tx_frame + sizeof(struct eth_hdr));
    ip->ver_ihl = 0x45; ip->tos = 0;
    ip->total_len = be16((uint16_t)ip_total);
    ip->id = 0; ip->flags_frag = 0;
    ip->ttl = 64; ip->protocol = IP_PROTO_UDP; ip->checksum = 0;
    ip->src[0] = net_cfg_ip[0]; ip->src[1] = net_cfg_ip[1];
    ip->src[2] = net_cfg_ip[2]; ip->src[3] = net_cfg_ip[3];
    for (int i = 0; i < 4; i++) ip->dst[i] = dst_ip[i];
    ip->checksum = ip_checksum(ip, sizeof(struct ip_hdr));

    /* UDP */
    struct udp_hdr *udp = (struct udp_hdr *)(tx_frame + sizeof(struct eth_hdr) + sizeof(struct ip_hdr));
    udp->src_port = be16(local_port);
    udp->dst_port = be16(dst_port);
    udp->length   = be16((uint16_t)udp_total);
    udp->checksum = 0; /* optional for IPv4 */

    /* Payload */
    uint8_t *pld = tx_frame + sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + sizeof(struct udp_hdr);
    for (int i = 0; i < len; i++) pld[i] = data[i];

    return net_tx_send(tx_frame, frame_len);
}
