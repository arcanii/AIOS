#ifndef AIOS_NET_H
#define AIOS_NET_H
/*
 * AIOS net.h -- Network types, protocol headers, DMA layout
 *
 * virtio-net driver and IP stack constants for the
 * two-thread networking architecture (driver + server).
 */
#include <stdint.h>

/* -- DMA layout (128KB, size-17 untyped, 32 frames) -- */
#define NET_DMA_SIZE        0x20000
#define NET_DMA_FRAMES      32

#define NET_RX_DESC_OFF     0x00000
#define NET_RX_AVAIL_OFF    0x00100
#define NET_RX_USED_OFF     0x01000
#define NET_TX_DESC_OFF     0x02000
#define NET_TX_AVAIL_OFF    0x02100
#define NET_TX_USED_OFF     0x03000
#define NET_RX_BUF_OFF      0x04000   /* 16 x 2048 = 32KB */
#define NET_TX_BUF_OFF      0x0C000   /* 16 x 2048 = 32KB */

#define NET_QUEUE_SIZE      16
#define NET_PKT_BUF_SIZE    2048

/* -- virtio-net header (prepended to every packet) -- */
#define VIRTIO_NET_HDR_SIZE 10

struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

/* -- Feature bits -- */
#define VIRTIO_NET_F_CSUM       (1 << 0)
#define VIRTIO_NET_F_MAC        (1 << 5)
#define VIRTIO_NET_F_STATUS     (1 << 16)
#define VIRTIO_NET_F_MRG_RXBUF  (1 << 15)

/* -- Ethernet -- */
#define ETH_TYPE_ARP    0x0806
#define ETH_TYPE_IP     0x0800
#define ETH_ALEN        6

struct eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} __attribute__((packed));

/* -- ARP -- */
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

struct arp_pkt {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t op;
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
} __attribute__((packed));

/* -- IPv4 -- */
#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

struct ip_hdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src[4];
    uint8_t  dst[4];
} __attribute__((packed));

/* -- ICMP -- */
#define ICMP_ECHO_REQUEST  8
#define ICMP_ECHO_REPLY    0

struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

/* -- UDP -- */
struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

/* -- TCP -- */
struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  off_rsvd;   /* data offset in high 4 bits */
    uint8_t  flags;
    uint16_t window;
    uint16_t cksum;
    uint16_t urgent;
} __attribute__((packed));

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

#define TCP_CLOSED    0
#define TCP_LISTEN    1
#define TCP_SYN_RCVD  2
#define TCP_ESTAB     3
#define TCP_FIN_WAIT  4
#define TCP_SYN_SENT  5

/* -- RX ring (driver -> server, lock-free SPSC) -- */
#define NET_RX_RING_SIZE  32
#define NET_RX_PKT_MAX    1518

struct rx_pkt_entry {
    uint16_t len;
    uint8_t  data[NET_RX_PKT_MAX];
};

struct net_rx_ring {
    volatile uint32_t head;   /* driver writes (produces) */
    volatile uint32_t tail;   /* server reads (consumes) */
    struct rx_pkt_entry pkts[NET_RX_RING_SIZE];
};

/* -- v0.4.225: inbound-path integrity counters (/proc/netstat) --
 * Defined in net_stack.c, incremented across the rx path. Built for the
 * push-corruption hunt (docs/NEXT_20260612_net_rx_corruption.md): counter
 * deltas across a corrupt vs clean transfer localize the failing layer. */
struct net_rx_stats {
    uint32_t tcp_data_segs;       /* inbound TCP segments carrying data      */
    uint32_t tcp_cksum_drops;     /* checksum mismatch -- segment DROPPED    */
    uint32_t tcp_dup_segs;        /* pure retransmissions re-ACKed           */
    uint32_t tcp_overlap_trims;   /* partial-overlap retransmits trimmed     */
    uint32_t tcp_ooo_drops;       /* future segments dropped (no reseq buf)  */
    uint32_t tcp_window_partial;  /* segment only partially accepted (full)  */
    uint32_t ring_overflow_drops; /* driver dropped frame: SPSC ring full    */
    uint32_t fault_drops;         /* test knob: deliberately dropped         */
    uint32_t tcp_reader_handoff;  /* segment handed to a parked reader       */
    uint32_t tcp_split_deliver;   /* handoff + remainder buffered (split)    */
    /* v0.4.225 corruption-hunt probes: with a 0xFF-free payload any 0xFF in
     * the rx path is corruption. Records the FIRST stream offset where a
     * 0xFF byte was stored into the ring vs read back out of it -- isolates
     * receive-path injection from the downstream ext2 write/read. */
    uint32_t dbg_ff_store_off;    /* 0 = none seen (offset 0 unlikely)       */
    uint32_t dbg_ff_read_off;
    uint32_t dbg_store_bytes;     /* total bytes buffered into rings         */
    uint32_t dbg_read_bytes;      /* total bytes read out of rings           */
    uint32_t tcp_read_acks;       /* v0.4.226: window-reopen ACKs sent on read
                                   * (coalesced; was 1 per 900B read)        */
};
extern struct net_rx_stats net_rx_stats;

/* Test knob: drop every Nth inbound TCP data segment (0 = off). Set via
 * /proc/netstat.drop.N -- forces retransmission storms deterministically
 * so the resequencing/overlap paths can be exercised on QEMU. */
extern uint32_t net_fault_drop_nth;

/* -- ARP cache -- */
#define ARP_CACHE_SIZE  16

struct arp_entry {
    int     valid;
    uint8_t ip[4];
    uint8_t mac[6];
};

/* -- Statistics -- */
struct net_stats {
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_dropped;
    uint32_t tx_dropped;
    uint32_t arp_requests;
    uint32_t arp_replies;
    uint32_t icmp_echo;
    uint32_t udp_datagrams;
};

/* -- Static network config (QEMU user-mode) -- */
#define NET_IP_A  10
#define NET_IP_B  0
#define NET_IP_C  2
#define NET_IP_D  15

#define NET_GW_A  10
#define NET_GW_B  0
#define NET_GW_C  2
#define NET_GW_D  2

#define NET_MASK  0xFFFFFF00

/* Byte-order helpers */
static inline uint16_t be16(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}
static inline uint32_t be32(uint32_t x) {
    return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) |
           ((x << 8) & 0xFF0000) | ((x << 24) & 0xFF000000);
}

/* Shared checksum (defined in net_stack.c) */
uint16_t ip_checksum(const void *data, int len);

/* Protocol stack (src/net/net_stack.c) */
void net_handle_packet(const uint8_t *data, uint32_t len);
int net_tx_send(const uint8_t *frame, uint32_t len);
void net_send_gratuitous_arp(void);

/* TCP (src/net/net_tcp.c) */
void handle_tcp(const uint8_t *pkt, uint32_t len,
                const struct ip_hdr *ip, const struct eth_hdr *eth);
/* v0.4.86: set before net_tcp_send for dynamic window */
extern uint16_t tcp_tx_window;

void net_tcp_send(const uint8_t *dst_ip, const uint8_t *dst_mac,
                  uint16_t src_port, uint16_t dst_port,
                  uint32_t seq, uint32_t ack_val, uint8_t flags,
                  const uint8_t *data, int data_len);
uint16_t tcp_checksum(const uint8_t *src_ip, const uint8_t *dst_ip,
                      const void *tcp_seg, int tcp_len);
void net_tcp_deliver(const uint8_t *src_ip, const uint8_t *src_mac,
                     uint16_t src_port, uint16_t dst_port,
                     uint32_t seq, uint32_t ack_val, uint8_t flags,
                     const uint8_t *data, int data_len);
void net_send_arp_request(const uint8_t *target_ip);
void net_send_ping(const uint8_t *dst_ip);
int  net_arp_resolved(const uint8_t *ip);
int  arp_cache_lookup(const uint8_t *ip, uint8_t *mac_out);
int  net_udp_send(int sock_id, uint16_t local_port,
                  uint32_t dst_ip, uint16_t dst_port,
                  const uint8_t *data, int len);
void net_udp_deliver(uint16_t dst_port, uint16_t src_port,
                     const uint8_t *src_ip,
                     const uint8_t *data, uint32_t len);

/* DHCP client (src/net/net_dhcp.c) -- run once at net_server startup */
extern int net_dhcp_pending;   /* 1 while a lease is being acquired */
int  net_dhcp_acquire(void);   /* 0 = bound (net_cfg_* updated), -1 = timeout */
void net_dhcp_input(const uint8_t *p, uint32_t len, const uint8_t *src_ip);

/* v0.4.158: DHCP diagnostic counters (read via /proc/genet.ip; one short line
 * survives the lossy mini-UART). Pinpoint where DHCP fails. */
extern int dhcp_replies, dhcp_offers, dhcp_acks, dhcp_naks, dhcp_mismatch, dhcp_bound;

#endif /* AIOS_NET_H */
