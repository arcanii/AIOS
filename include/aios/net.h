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
int  net_udp_send(int sock_id, uint16_t local_port,
                  uint32_t dst_ip, uint16_t dst_port,
                  const uint8_t *data, int len);
void net_udp_deliver(uint16_t dst_port, uint16_t src_port,
                     const uint8_t *src_ip,
                     const uint8_t *data, uint32_t len);

#endif /* AIOS_NET_H */
