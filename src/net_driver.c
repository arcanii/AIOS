/*
 * AIOS Network Driver – virtio-net (legacy MMIO)
 *
 * Provides Ethernet frame send/receive via virtio-net.
 * Uses two virtqueues: 0=RX, 1=TX.
 * Communicates with net_server via shared memory + notifications.
 */
#include <stdint.h>
#include <microkit.h>
#include "aios/channels.h"
#include "aios/ipc.h"
#include "virtio.h"

/* ── Memory regions ──────────────────────────────────── */
uintptr_t virtio_net_base_vaddr;
uintptr_t net_data;         /* shared with net_server */
uintptr_t net_dma_base;     /* DMA region for virtqueues */

#define NET_DMA_PADDR  0xB0100000
#define NET_DMA_SIZE   0x20000    /* 128 KiB */

/* DMA layout:
 * 0x0000 - 0x00FF: RX descriptors (16 entries * 16 bytes)
 * 0x0100 - 0x01FF: RX avail ring
 * 0x1000 - 0x10FF: RX used ring
 * 0x2000 - 0x20FF: TX descriptors
 * 0x2100 - 0x21FF: TX avail ring
 * 0x3000 - 0x30FF: TX used ring
 * 0x4000 - 0x0FFFF: RX packet buffers (12 * 2048 = 24 KiB)
 * 0x10000 - 0x1BFFF: TX packet buffers (12 * 2048 = 24 KiB... actually we reuse)
 */

#define RX_DESC_OFF    0x0000
#define RX_AVAIL_OFF   0x0100
#define RX_USED_OFF    0x1000
#define TX_DESC_OFF    0x2000
#define TX_AVAIL_OFF   0x2100
#define TX_USED_OFF    0x3000
#define RX_BUF_OFF     0x4000
#define TX_BUF_OFF     0x10000

#define QUEUE_SIZE     16
#define PKT_BUF_SIZE   2048

/* virtio-net header (must prepend every packet) */
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    /* uint16_t num_buffers;  -- only for mergeable rx buffers */
} __attribute__((packed));

#define VIRTIO_NET_HDR_SIZE  10
#define VIRTIO_NET_DEVICE_ID 1

/* ── Register access ─────────────────────────────────── */
static uint32_t vio_read32(uint32_t off) {
    return *(volatile uint32_t *)(virtio_net_base_vaddr + 0xc00 + off);
}
static void vio_write32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(virtio_net_base_vaddr + 0xc00 + off) = val;
}

/* ── Virtqueue state ─────────────────────────────────── */
static volatile struct virtq_desc  *rx_descs;
static volatile struct virtq_avail *rx_avail;
static volatile struct virtq_used  *rx_used;
static volatile struct virtq_desc  *tx_descs;
static volatile struct virtq_avail *tx_avail;
static volatile struct virtq_used  *tx_used;

static uint16_t rx_last_used = 0;
static uint16_t tx_last_used __attribute__((unused)) = 0;

static uint8_t mac_addr[6];

/* ── net_data IPC layout ─────────────────────────────── */



/* ── Helpers ─────────────────────────────────────────── */
static void my_memset(void *dst, int c, unsigned long n) {
    uint8_t *d = dst; while (n--) *d++ = c;
}
static void my_memcpy(void *dst, const void *src, unsigned long n) {
    uint8_t *d = dst; const uint8_t *s = src; while (n--) *d++ = *s++;
}

/* ── Setup RX virtqueue ──────────────────────────────── */
static void rx_replenish(void) {
    for (int i = 0; i < QUEUE_SIZE; i++) {
        uint64_t buf_pa = NET_DMA_PADDR + RX_BUF_OFF + i * PKT_BUF_SIZE;
        rx_descs[i].addr  = buf_pa;
        rx_descs[i].len   = PKT_BUF_SIZE;
        rx_descs[i].flags = VIRTQ_DESC_F_WRITE; /* device writes to this */
        rx_descs[i].next  = 0;
        rx_avail->ring[rx_avail->idx % QUEUE_SIZE] = i;
        rx_avail->idx++;
    }
}

/* ── Init ────────────────────────────────────────────── */
static int virtio_net_init(void) {
    /* Verify magic */
    if (vio_read32(VIRTIO_MMIO_MAGIC) != VIRTIO_MAGIC) {
        microkit_dbg_puts("NET: bad magic\n");
        return -1;
    }
    if (vio_read32(VIRTIO_MMIO_VERSION) != 1) {
        microkit_dbg_puts("NET: bad version\n");
        return -1;
    }
    if (vio_read32(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_NET_DEVICE_ID) {
        microkit_dbg_puts("NET: not a network device\n");
        return -1;
    }

    /* Reset */
    vio_write32(VIRTIO_MMIO_STATUS, 0);
    /* Acknowledge */
    vio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    vio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Read features, accept none (keep it simple) */
    (void)vio_read32(VIRTIO_MMIO_HOST_FEATURES);
    vio_write32(VIRTIO_MMIO_DRV_FEATURES, 0);

    /* Page size */
    vio_write32(VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);

    /* Zero DMA region */
    my_memset((void *)net_dma_base, 0, NET_DMA_SIZE);

    /* Setup RX queue (queue 0) */
    vio_write32(VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t rx_qmax = vio_read32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (rx_qmax < QUEUE_SIZE) {
        microkit_dbg_puts("NET: RX queue too small\n");
        return -1;
    }
    vio_write32(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);
    vio_write32(VIRTIO_MMIO_QUEUE_ALIGN, 4096);
    vio_write32(VIRTIO_MMIO_QUEUE_PFN, (NET_DMA_PADDR + RX_DESC_OFF) / 4096);

    rx_descs = (volatile struct virtq_desc *)(net_dma_base + RX_DESC_OFF);
    rx_avail = (volatile struct virtq_avail *)(net_dma_base + RX_AVAIL_OFF);
    rx_used  = (volatile struct virtq_used *)(net_dma_base + RX_USED_OFF);

    /* Setup TX queue (queue 1) */
    vio_write32(VIRTIO_MMIO_QUEUE_SEL, 1);
    uint32_t tx_qmax = vio_read32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (tx_qmax < QUEUE_SIZE) {
        microkit_dbg_puts("NET: TX queue too small\n");
        return -1;
    }
    vio_write32(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);
    vio_write32(VIRTIO_MMIO_QUEUE_ALIGN, 4096);
    vio_write32(VIRTIO_MMIO_QUEUE_PFN, (NET_DMA_PADDR + TX_DESC_OFF) / 4096);

    tx_descs = (volatile struct virtq_desc *)(net_dma_base + TX_DESC_OFF);
    tx_avail = (volatile struct virtq_avail *)(net_dma_base + TX_AVAIL_OFF);
    tx_used  = (volatile struct virtq_used *)(net_dma_base + TX_USED_OFF);

    /* Replenish RX buffers */
    rx_replenish();
    __asm__ volatile("dmb sy" ::: "memory");
    vio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);  /* kick RX queue */

    /* Read MAC address from config space */
    for (int i = 0; i < 6; i++) {
        mac_addr[i] = *(volatile uint8_t *)(virtio_net_base_vaddr + 0xc00 + VIRTIO_MMIO_CONFIG + i);
    }

    /* Driver OK */
    vio_write32(VIRTIO_MMIO_STATUS,
        VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    microkit_dbg_puts("NET: virtio-net ready, MAC=");
    for (int i = 0; i < 6; i++) {
        char hex[3];
        hex[0] = "0123456789abcdef"[(mac_addr[i] >> 4) & 0xf];
        hex[1] = "0123456789abcdef"[mac_addr[i] & 0xf];
        hex[2] = 0;
        microkit_dbg_puts(hex);
        if (i < 5) microkit_dbg_puts(":");
    }
    microkit_dbg_puts("\n");

    return 0;
}

/* ── Send a frame ────────────────────────────────────── */
static int net_send(const uint8_t *data, uint32_t len) {
    if (len > PKT_BUF_SIZE - VIRTIO_NET_HDR_SIZE) return -1;

    /* Use a TX buffer */
    uint16_t idx = tx_avail->idx % QUEUE_SIZE;
    uint8_t *buf = (uint8_t *)(net_dma_base + TX_BUF_OFF + idx * PKT_BUF_SIZE);
    uint64_t buf_pa = NET_DMA_PADDR + TX_BUF_OFF + idx * PKT_BUF_SIZE;

    /* Write virtio-net header (all zeros = no offload) */
    my_memset(buf, 0, VIRTIO_NET_HDR_SIZE);
    /* Copy frame data */
    my_memcpy(buf + VIRTIO_NET_HDR_SIZE, data, len);

    tx_descs[idx].addr  = buf_pa;
    tx_descs[idx].len   = VIRTIO_NET_HDR_SIZE + len;
    tx_descs[idx].flags = 0; /* device reads from this */
    tx_descs[idx].next  = 0;

    tx_avail->ring[idx] = idx;
    __sync_synchronize();
    tx_avail->idx++;
    __sync_synchronize();

    /* Notify device */
    vio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 1);

    return 0;
}

/* ── Receive a frame ─────────────────────────────────── */
/* ── Receive a frame ─────────────────────────────────── */
static int net_recv(uint8_t *out_buf, uint32_t *out_len) {
    if (rx_used->idx == rx_last_used) return -1; /* no packet */

    uint16_t used_idx = rx_last_used % QUEUE_SIZE;
    uint32_t desc_idx = rx_used->ring[used_idx].id;
    uint32_t total_len = rx_used->ring[used_idx].len;

    if (total_len <= VIRTIO_NET_HDR_SIZE) {
        *out_len = 0;
    } else {
        uint32_t frame_len = total_len - VIRTIO_NET_HDR_SIZE;
        uint8_t *buf = (uint8_t *)(net_dma_base + RX_BUF_OFF + desc_idx * PKT_BUF_SIZE);
        if (frame_len > NET_DATA_MAX) frame_len = NET_DATA_MAX;
        my_memcpy(out_buf, buf + VIRTIO_NET_HDR_SIZE, frame_len);
        *out_len = frame_len;
    }

    rx_last_used++;

    /* Re-queue the descriptor */
    rx_descs[desc_idx].addr  = NET_DMA_PADDR + RX_BUF_OFF + desc_idx * PKT_BUF_SIZE;
    rx_descs[desc_idx].len   = PKT_BUF_SIZE;
    rx_descs[desc_idx].flags = VIRTQ_DESC_F_WRITE;
    rx_avail->ring[rx_avail->idx % QUEUE_SIZE] = desc_idx;
    __asm__ volatile("dmb sy" ::: "memory");
    rx_avail->idx++;
    vio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    return (*out_len > 0) ? 0 : -1;
}

/* ── Microkit entry points ───────────────────────────── */

void init(void) {
    if (virtio_net_init() == 0) {
        microkit_dbg_puts("NET: driver ready\n");
    } else {
        microkit_dbg_puts("NET: init FAILED\n");
    }
}

void notified(microkit_channel ch) {
    if (ch == CH_NET_IRQ) {
        /* Acknowledge interrupt */
        uint32_t isr = vio_read32(VIRTIO_MMIO_INTERRUPT_STATUS);
        vio_write32(VIRTIO_MMIO_INTERRUPT_ACK, isr);
        microkit_irq_ack(ch);

        /* Check for received packets */
        while (rx_used->idx != rx_last_used) {
            /* Copy packet to net_data and notify net_server */
            uint32_t pkt_len = 0;
            volatile uint8_t *dst = (volatile uint8_t *)(net_data + NET_PKT_DATA);
            net_recv((uint8_t *)dst, &pkt_len);
            if (pkt_len > 0) {
                WR32(net_data, NET_PKT_LEN, pkt_len);
                WR32(net_data, NET_CMD, NET_CMD_RECV);
                microkit_notify(CH_NET);
            }
        }
    }

    if (ch == CH_NET) {
        /* net_server wants to send a packet */
        uint32_t cmd = RD32(net_data, NET_CMD);
        if (cmd == NET_CMD_SEND) {
            uint32_t len = RD32(net_data, NET_PKT_LEN);
            volatile uint8_t *src = (volatile uint8_t *)(net_data + NET_PKT_DATA);
            net_send((const uint8_t *)src, len);
            WR32(net_data, NET_STATUS, NET_ST_OK);
        } else if (cmd == NET_CMD_GET_MAC) {
            volatile uint8_t *dst = (volatile uint8_t *)(net_data + NET_MAC_OFF);
            for (int i = 0; i < 6; i++) dst[i] = mac_addr[i];
            WR32(net_data, NET_STATUS, NET_ST_OK);
        }
    }
}
