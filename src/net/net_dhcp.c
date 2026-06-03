/*
 * net_dhcp.c -- minimal DHCP client (v0.4.152)
 *
 * Runs once at net_server startup, before the stack commits to an IP:
 *   DISCOVER -> OFFER -> REQUEST -> ACK
 * On ACK it writes net_cfg_ip/gw/mask in place (my_ip in net_stack.c points at
 * net_cfg_ip, so the running stack picks up the lease). Falls back to the
 * static /etc/network.conf config on timeout.
 *
 * Packets go from 0.0.0.0:68 to 255.255.255.255:67 with the BOOTP broadcast
 * flag set. net_stack.c accepts broadcast / is promiscuous while
 * net_dhcp_pending and routes UDP dst port 68 to net_dhcp_input().
 *
 * Shares the same primitives as the rest of the stack (net_tx_send,
 * ip_checksum, net_mac, net_cfg_*); works unchanged on QEMU virtio-net and
 * RPi4 GENET. Timeout uses the ARM generic timer (CNTPCT_EL0).
 */
#include "aios/root_shared.h"
#include "aios/net.h"
#include "aios/config.h"
#include <stdio.h>

#define DHCP_SPORT   68
#define DHCP_DPORT   67
#define BOOTREQUEST  1
#define BOOTREPLY    2
#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

#define O_SUBNET    1
#define O_ROUTER    3
#define O_DNS       6
#define O_REQIP     50
#define O_MSGTYPE   53
#define O_SERVERID  54
#define O_PARAMS    55
#define O_END       255

/* BOOTP fixed header is 236 bytes; magic cookie + options follow. We always
 * send a 300-byte DHCP payload (the BOOTP minimum some servers insist on). */
#define DHCP_PAYLOAD 300

int net_dhcp_pending;            /* read by net_stack.c handle_ipv4 / handle_udp */

static int      dhcp_state;      /* 0 idle, 1 discover sent, 2 request sent, 3 bound */
static uint32_t dhcp_xid;
static uint8_t  dhcp_yiaddr[4];  /* offered IP */
static uint8_t  dhcp_server[4];  /* opt 54 (server id) */
static uint8_t  dhcp_router[4];  /* opt 3 */
static uint8_t  dhcp_mask[4];    /* opt 1 */
static int      have_router, have_mask;

static uint8_t  dhcp_tx[1518];

static inline uint64_t read_cntpct(void) {
    uint64_t v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v;
}
static inline uint64_t read_cntfrq(void) {
    uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f;
}

/* Fill the BOOTP fixed header + magic cookie into a pre-zeroed buffer.
 * Returns a pointer to the first option byte. */
static uint8_t *dhcp_build_base(uint8_t *o) {
    o[0] = BOOTREQUEST; o[1] = 1; o[2] = 6; o[3] = 0;   /* op/htype/hlen/hops */
    o[4] = (dhcp_xid >> 24) & 0xFF; o[5] = (dhcp_xid >> 16) & 0xFF;
    o[6] = (dhcp_xid >> 8)  & 0xFF; o[7] =  dhcp_xid        & 0xFF;
    o[10] = 0x80; o[11] = 0x00;                          /* flags: broadcast */
    for (int i = 0; i < 6; i++) o[28 + i] = net_mac[i];  /* chaddr */
    o[236] = 0x63; o[237] = 0x82; o[238] = 0x53; o[239] = 0x63;  /* magic cookie */
    return o + 240;
}

static void dhcp_send(uint8_t msgtype) {
    uint8_t dhcp[DHCP_PAYLOAD];
    for (int i = 0; i < DHCP_PAYLOAD; i++) dhcp[i] = 0;
    uint8_t *opt = dhcp_build_base(dhcp);

    *opt++ = O_MSGTYPE; *opt++ = 1; *opt++ = msgtype;
    if (msgtype == DHCPREQUEST) {
        *opt++ = O_REQIP;    *opt++ = 4; for (int i = 0; i < 4; i++) *opt++ = dhcp_yiaddr[i];
        *opt++ = O_SERVERID; *opt++ = 4; for (int i = 0; i < 4; i++) *opt++ = dhcp_server[i];
    }
    *opt++ = O_PARAMS; *opt++ = 3; *opt++ = O_SUBNET; *opt++ = O_ROUTER; *opt++ = O_DNS;
    *opt++ = O_END;

    uint32_t udp_total = sizeof(struct udp_hdr) + DHCP_PAYLOAD;
    uint32_t ip_total  = sizeof(struct ip_hdr) + udp_total;
    uint32_t frame_len = sizeof(struct eth_hdr) + ip_total;

    struct eth_hdr *eth = (struct eth_hdr *)dhcp_tx;
    for (int i = 0; i < 6; i++) { eth->dst[i] = 0xFF; eth->src[i] = net_mac[i]; }
    eth->type = be16(ETH_TYPE_IP);

    struct ip_hdr *ip = (struct ip_hdr *)(dhcp_tx + sizeof(struct eth_hdr));
    ip->ver_ihl = 0x45; ip->tos = 0;
    ip->total_len = be16((uint16_t)ip_total);
    ip->id = 0; ip->flags_frag = 0;
    ip->ttl = 64; ip->protocol = IP_PROTO_UDP; ip->checksum = 0;
    for (int i = 0; i < 4; i++) { ip->src[i] = 0; ip->dst[i] = 0xFF; }
    ip->checksum = ip_checksum(ip, sizeof(struct ip_hdr));

    struct udp_hdr *udp = (struct udp_hdr *)(dhcp_tx + sizeof(struct eth_hdr) + sizeof(struct ip_hdr));
    udp->src_port = be16(DHCP_SPORT);
    udp->dst_port = be16(DHCP_DPORT);
    udp->length   = be16((uint16_t)udp_total);
    udp->checksum = 0;   /* optional for IPv4 */

    uint8_t *pld = dhcp_tx + sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + sizeof(struct udp_hdr);
    for (int i = 0; i < DHCP_PAYLOAD; i++) pld[i] = dhcp[i];

    net_tx_send(dhcp_tx, frame_len);
}

/* Called from net_stack.c handle_udp for dst port 68. */
void net_dhcp_input(const uint8_t *p, uint32_t len, const uint8_t *src_ip) {
    (void)src_ip;
    if (!net_dhcp_pending || len < 240) return;
    if (p[0] != BOOTREPLY) return;
    uint32_t xid = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
                   ((uint32_t)p[6] << 8) | p[7];
    if (xid != dhcp_xid) return;
    if (!(p[236] == 0x63 && p[237] == 0x82 && p[238] == 0x53 && p[239] == 0x63)) return;

    uint8_t msgtype = 0, r_router[4], r_mask[4], r_server[4];
    int g_router = 0, g_mask = 0, g_server = 0;
    uint32_t i = 240;
    while (i < len) {
        uint8_t code = p[i++];
        if (code == O_END) break;
        if (code == 0) continue;          /* pad */
        if (i >= len) break;
        uint8_t olen = p[i++];
        if (i + olen > len) break;
        if      (code == O_MSGTYPE  && olen >= 1) msgtype = p[i];
        else if (code == O_ROUTER   && olen >= 4) { for (int k=0;k<4;k++) r_router[k]=p[i+k]; g_router=1; }
        else if (code == O_SUBNET   && olen >= 4) { for (int k=0;k<4;k++) r_mask[k]=p[i+k];   g_mask=1; }
        else if (code == O_SERVERID && olen >= 4) { for (int k=0;k<4;k++) r_server[k]=p[i+k]; g_server=1; }
        i += olen;
    }

    if (msgtype == DHCPOFFER && dhcp_state == 1) {
        for (int k = 0; k < 4; k++) dhcp_yiaddr[k] = p[16 + k];   /* yiaddr */
        if (g_server) for (int k=0;k<4;k++) dhcp_server[k] = r_server[k];
        if (g_router) { for (int k=0;k<4;k++) dhcp_router[k]=r_router[k]; have_router=1; }
        if (g_mask)   { for (int k=0;k<4;k++) dhcp_mask[k]=r_mask[k];     have_mask=1; }
        printf("[net] DHCP OFFER %d.%d.%d.%d from %d.%d.%d.%d\n",
               dhcp_yiaddr[0], dhcp_yiaddr[1], dhcp_yiaddr[2], dhcp_yiaddr[3],
               dhcp_server[0], dhcp_server[1], dhcp_server[2], dhcp_server[3]);
        dhcp_send(DHCPREQUEST);
        dhcp_state = 2;
    } else if (msgtype == DHCPACK && dhcp_state == 2) {
        for (int k = 0; k < 4; k++) net_cfg_ip[k] = p[16 + k];   /* yiaddr from ACK */
        if (have_router) for (int k=0;k<4;k++) net_cfg_gw[k]   = dhcp_router[k];
        if (have_mask)   for (int k=0;k<4;k++) net_cfg_mask[k] = dhcp_mask[k];
        dhcp_state = 3;
        printf("[net] DHCP ACK: ip=%d.%d.%d.%d gw=%d.%d.%d.%d mask=%d.%d.%d.%d\n",
               net_cfg_ip[0], net_cfg_ip[1], net_cfg_ip[2], net_cfg_ip[3],
               net_cfg_gw[0], net_cfg_gw[1], net_cfg_gw[2], net_cfg_gw[3],
               net_cfg_mask[0], net_cfg_mask[1], net_cfg_mask[2], net_cfg_mask[3]);
    } else if (msgtype == DHCPNAK) {
        printf("[net] DHCP NAK; will retry\n");
        dhcp_state = 0;
    }
}

/* Drain rx_ring once, feeding the dispatcher (routes port 68 -> net_dhcp_input). */
static void dhcp_poll_rx(void) {
    while (net_rx_ring.tail != net_rx_ring.head) {
        uint32_t t = net_rx_ring.tail % NET_RX_RING_SIZE;
        struct rx_pkt_entry *entry = &net_rx_ring.pkts[t];
        if (entry->len > 0) net_handle_packet(entry->data, entry->len);
        __asm__ volatile("dmb sy" ::: "memory");
        net_rx_ring.tail++;
    }
}

/* Acquire a lease. Returns 0 if bound, -1 on timeout. ~1s per attempt. */
int net_dhcp_acquire(void) {
    if (!net_available) return -1;

    uint64_t freq = read_cntfrq();
    if (freq == 0) freq = 62500000;   /* RPi4/QEMU generic-timer fallback */

    dhcp_xid = 0xA1050000u | ((uint32_t)net_mac[4] << 8) | net_mac[5];
    have_router = have_mask = 0;
    net_dhcp_pending = 1;

    const int ATTEMPTS = 4;
    for (int a = 0; a < ATTEMPTS && dhcp_state != 3; a++) {
        dhcp_xid++;
        dhcp_state = 1;
        dhcp_send(DHCPDISCOVER);
        uint64_t deadline = read_cntpct() + freq;   /* ~1 second */
        while (read_cntpct() < deadline && dhcp_state != 3) {
            dhcp_poll_rx();
            seL4_Yield();
        }
    }

    net_dhcp_pending = 0;
    return (dhcp_state == 3) ? 0 : -1;
}
