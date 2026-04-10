/*
 * net_driver.c -- virtio-net driver thread (RX hardware interface)
 *
 * Drains the RX used ring, copies Ethernet frames (minus the
 * 10-byte virtio-net header) into the lock-free rx_ring shared
 * with net_server, replenishes RX descriptors, and signals the
 * server notification.
 */
#include "aios/root_shared.h"
#include "aios/net.h"
#include "virtio.h"
#include <stdio.h>

void net_driver_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr drv_ntfn = (seL4_CPtr)(uintptr_t)arg0;
    (void)arg1; (void)ipc_buf;

    struct virtq_desc  *rx_desc  =
        (struct virtq_desc  *)(net_dma + NET_RX_DESC_OFF);
    struct virtq_avail *rx_avail =
        (struct virtq_avail *)(net_dma + NET_RX_AVAIL_OFF);
    struct virtq_used  *rx_used  =
        (struct virtq_used  *)(net_dma + NET_RX_USED_OFF);

    uint16_t rx_last_used = 0;
    uint32_t rx_dropped = 0;

    printf("[net-drv] Driver thread ready\n");

    while (1) {
        seL4_Word badge;
        seL4_Wait(drv_ntfn, &badge);

        int drained = 0;
        while (rx_used->idx != rx_last_used) {
            uint16_t used_slot = rx_last_used % NET_QUEUE_SIZE;
            uint32_t desc_idx  = rx_used->ring[used_slot].id;
            uint32_t total_len = rx_used->ring[used_slot].len;

            if (total_len > VIRTIO_NET_HDR_SIZE) {
                uint32_t frame_len = total_len - VIRTIO_NET_HDR_SIZE;
                if (frame_len > NET_RX_PKT_MAX)
                    frame_len = NET_RX_PKT_MAX;

                /* Check rx_ring capacity (SPSC: driver writes head) */
                uint32_t h = net_rx_ring.head;
                uint32_t t = net_rx_ring.tail;
                if ((h - t) < NET_RX_RING_SIZE) {
                    struct rx_pkt_entry *entry =
                        &net_rx_ring.pkts[h % NET_RX_RING_SIZE];
                    uint8_t *src = net_dma + NET_RX_BUF_OFF
                        + desc_idx * NET_PKT_BUF_SIZE
                        + VIRTIO_NET_HDR_SIZE;
                    for (uint32_t i = 0; i < frame_len; i++)
                        entry->data[i] = src[i];
                    entry->len = (uint16_t)frame_len;
                    __asm__ volatile("dmb sy" ::: "memory");
                    net_rx_ring.head = h + 1;
                    drained++;
                } else {
                    rx_dropped++;
                }
            }

            /* Replenish this RX descriptor */
            rx_desc[desc_idx].addr = net_dma_pa + NET_RX_BUF_OFF
                + desc_idx * NET_PKT_BUF_SIZE;
            rx_desc[desc_idx].len   = NET_PKT_BUF_SIZE;
            rx_desc[desc_idx].flags = VIRTQ_DESC_F_WRITE;
            rx_desc[desc_idx].next  = 0;
            rx_avail->ring[rx_avail->idx % NET_QUEUE_SIZE] = desc_idx;
            __asm__ volatile("dmb sy" ::: "memory");
            rx_avail->idx++;

            rx_last_used++;
        }

        if (drained > 0) {
            /* Kick RX queue for replenished descriptors */
            net_vio[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;
            /* Wake server to process packets */
            seL4_Signal(net_srv_ntfn_cap);
        }
    }
}
