/*
 * net_virtio.c -- virtio-net platform driver (QEMU virt)
 *
 * PAL implementation for PLAT_QEMU_VIRT networking.
 * Provides plat_net_init/tx/driver_fn/get_mac matching net_hal.h.
 *
 * Extracted from boot_net_init.c + net_driver.c + net_stack.c
 * during v0.4.89 PAL refactor.
 */
#include "aios/root_shared.h"
#include <simple/simple.h>
#include "aios/net.h"
#include "aios/vka_audit.h"
#include "virtio.h"
#define LOG_MODULE "net"
#define LOG_LEVEL LOG_LEVEL_DEBUG
#include "aios/aios_log.h"
#include <stdio.h>
#include "arch.h"
#include "plat/net_hal.h"

/* ---- Private state (was extern in root_shared.h) ---- */

static volatile uint32_t *net_vio_priv;
static uint8_t  *net_dma_priv;
static uint64_t  net_dma_pa_priv;
static int       net_vio_slot_priv;
static seL4_CPtr net_irq_handler_priv;

/* ============================================================
 * plat_net_init -- init virtio-net device, DMA, IRQ
 * ============================================================ */
int plat_net_init(void) {
    if (!net_available) return -1;
    int error;

    /* Use net_vio from blk_virtio.c probe (still extern) */
    net_vio_priv = net_vio;
    net_vio_slot_priv = net_vio_slot;

    /* Verify device identity */
    if (net_vio_priv[VIRTIO_MMIO_MAGIC / 4] != VIRTIO_MAGIC ||
        net_vio_priv[VIRTIO_MMIO_DEVICE_ID / 4] != VIRTIO_NET_DEVICE_ID) {
        printf("[net] Bad device at slot %d\n", net_vio_slot_priv);
        net_available = 0;
        return -1;
    }

    /* Allocate 128KB DMA (size-17 untyped = 32 contiguous pages) */
    vka_object_t dma_ut;
    vka_audit_untyped(VKA_SUB_NET, 17);
    error = vka_alloc_untyped(&vka, 17, &dma_ut);
    if (error) {
        printf("[net] DMA untyped alloc failed: %d\n", error);
        net_available = 0;
        return -1;
    }

    seL4_CPtr dma_caps[NET_DMA_FRAMES];
    for (int i = 0; i < NET_DMA_FRAMES; i++) {
        seL4_CPtr slot;
        error = vka_cspace_alloc(&vka, &slot);
        if (error) {
            printf("[net] DMA cslot alloc failed at %d\n", i);
            net_available = 0;
            return -1;
        }
        error = seL4_Untyped_Retype(dma_ut.cptr,
            ARCH_PAGE_OBJECT, seL4_PageBits,
            seL4_CapInitThreadCNode, 0, 0, slot, 1);
        if (error) {
            printf("[net] DMA retype %d failed: %d\n", i, error);
            net_available = 0;
            return -1;
        }
        dma_caps[i] = slot;
    }

    void *dma_vaddr = vspace_map_pages(&vspace, dma_caps, NULL,
        seL4_AllRights, NET_DMA_FRAMES, seL4_PageBits, 0);
    if (!dma_vaddr) {
        printf("[net] DMA map failed\n");
        net_available = 0;
        return -1;
    }

    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_caps[0]);
    if (ga.error) {
        printf("[net] DMA GetAddress failed\n");
        net_available = 0;
        return -1;
    }

    net_dma_priv = (uint8_t *)dma_vaddr;
    net_dma_pa_priv = ga.paddr;

    /* Zero DMA region */
    for (int i = 0; i < NET_DMA_SIZE; i++) net_dma_priv[i] = 0;

    /* Legacy virtio init sequence */
    net_vio_priv[VIRTIO_MMIO_STATUS / 4] = 0;
    net_vio_priv[VIRTIO_MMIO_STATUS / 4] = VIRTIO_STATUS_ACK;
    net_vio_priv[VIRTIO_MMIO_STATUS / 4] = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER;

    uint32_t host_feat = net_vio_priv[VIRTIO_MMIO_HOST_FEATURES / 4];
    uint32_t drv_feat = 0;
    if (host_feat & VIRTIO_NET_F_MAC)
        drv_feat |= VIRTIO_NET_F_MAC;
    net_vio_priv[VIRTIO_MMIO_DRV_FEATURES / 4] = drv_feat;
    net_vio_priv[VIRTIO_MMIO_GUEST_PAGE_SIZE / 4] = 4096;

    /* Setup RX queue (queue 0) */
    net_vio_priv[VIRTIO_MMIO_QUEUE_SEL / 4] = 0;
    uint32_t rx_qmax = net_vio_priv[VIRTIO_MMIO_QUEUE_NUM_MAX / 4];
    if (rx_qmax < NET_QUEUE_SIZE) {
        printf("[net] RX queue too small: %u\n", rx_qmax);
        net_available = 0;
        return -1;
    }
    net_vio_priv[VIRTIO_MMIO_QUEUE_NUM / 4] = NET_QUEUE_SIZE;
    net_vio_priv[VIRTIO_MMIO_QUEUE_ALIGN / 4] = 4096;
    net_vio_priv[VIRTIO_MMIO_QUEUE_PFN / 4] =
        (uint32_t)(net_dma_pa_priv / 4096);

    /* Setup TX queue (queue 1) */
    net_vio_priv[VIRTIO_MMIO_QUEUE_SEL / 4] = 1;
    uint32_t tx_qmax = net_vio_priv[VIRTIO_MMIO_QUEUE_NUM_MAX / 4];
    if (tx_qmax < NET_QUEUE_SIZE) {
        printf("[net] TX queue too small: %u\n", tx_qmax);
        net_available = 0;
        return -1;
    }
    net_vio_priv[VIRTIO_MMIO_QUEUE_NUM / 4] = NET_QUEUE_SIZE;
    net_vio_priv[VIRTIO_MMIO_QUEUE_ALIGN / 4] = 4096;
    net_vio_priv[VIRTIO_MMIO_QUEUE_PFN / 4] =
        (uint32_t)((net_dma_pa_priv + NET_TX_DESC_OFF) / 4096);

    /* Replenish all RX descriptors */
    struct virtq_desc *rx_desc =
        (struct virtq_desc *)(net_dma_priv + NET_RX_DESC_OFF);
    struct virtq_avail *rx_avail =
        (struct virtq_avail *)(net_dma_priv + NET_RX_AVAIL_OFF);

    for (int i = 0; i < NET_QUEUE_SIZE; i++) {
        rx_desc[i].addr =
            net_dma_pa_priv + NET_RX_BUF_OFF + i * NET_PKT_BUF_SIZE;
        rx_desc[i].len   = NET_PKT_BUF_SIZE;
        rx_desc[i].flags = VIRTQ_DESC_F_WRITE;
        rx_desc[i].next  = 0;
        rx_avail->ring[i] = i;
    }
    rx_avail->idx = NET_QUEUE_SIZE;
    arch_dmb();
    net_vio_priv[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

    /* Read MAC from config space */
    volatile uint8_t *cfg = (volatile uint8_t *)
        ((uintptr_t)net_vio_priv + VIRTIO_MMIO_CONFIG);
    for (int i = 0; i < 6; i++) net_mac[i] = cfg[i];

    /* Driver OK */
    net_vio_priv[VIRTIO_MMIO_STATUS / 4] =
        VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK;
    net_vio_priv[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

    /* Allocate notification objects for driver/server IPC */
    vka_object_t drv_ntfn_obj, srv_ntfn_obj;
    error = vka_alloc_notification(&vka, &drv_ntfn_obj);
    if (error) {
        printf("[net] drv notification alloc failed\n");
        net_available = 0;
        return -1;
    }
    error = vka_alloc_notification(&vka, &srv_ntfn_obj);
    if (error) {
        printf("[net] srv notification alloc failed\n");
        net_available = 0;
        return -1;
    }
    net_drv_ntfn_cap = drv_ntfn_obj.cptr;
    net_srv_ntfn_cap = srv_ntfn_obj.cptr;

    /* Bind virtio-net IRQ to driver notification */
    {
        uint32_t net_irq = 48 + (uint32_t)net_vio_slot_priv;
        cspacepath_t nirq_path;
        int irq_err = vka_cspace_alloc_path(&vka, &nirq_path);
        if (!irq_err) {
            irq_err = simple_get_IRQ_handler(&simple, net_irq, nirq_path);
            if (!irq_err) {
                net_irq_handler_priv = nirq_path.capPtr;
                irq_err = seL4_IRQHandler_SetNotification(
                    net_irq_handler_priv, net_drv_ntfn_cap);
                if (!irq_err) {
                    seL4_IRQHandler_Ack(net_irq_handler_priv);
                    printf("[boot] virtio-net IRQ %u bound to driver\n", net_irq);
                } else {
                    printf("[boot] net IRQ bind failed: %d\n", irq_err);
                }
            } else {
                printf("[boot] net IRQ handler failed: %d (irq=%u)\n", irq_err, net_irq);
            }
        }
    }

    printf("[boot] virtio-net ready, MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
           net_mac[0], net_mac[1], net_mac[2],
           net_mac[3], net_mac[4], net_mac[5]);
    LOG_INFO("virtio-net initialized");
    return 0;
}

/* ============================================================
 * plat_net_tx -- transmit Ethernet frame via virtio-net TX queue
 * ============================================================ */
int plat_net_tx(const uint8_t *frame, uint32_t len) {
    if (len + VIRTIO_NET_HDR_SIZE > NET_PKT_BUF_SIZE) return -1;

    struct virtq_desc  *tx_desc  =
        (struct virtq_desc  *)(net_dma_priv + NET_TX_DESC_OFF);
    struct virtq_avail *tx_avail =
        (struct virtq_avail *)(net_dma_priv + NET_TX_AVAIL_OFF);

    uint16_t idx = tx_avail->idx % NET_QUEUE_SIZE;
    uint8_t *buf = net_dma_priv + NET_TX_BUF_OFF + idx * NET_PKT_BUF_SIZE;
    uint64_t buf_pa = net_dma_pa_priv + NET_TX_BUF_OFF
        + (uint64_t)idx * NET_PKT_BUF_SIZE;

    for (int i = 0; i < VIRTIO_NET_HDR_SIZE; i++) buf[i] = 0;
    for (uint32_t i = 0; i < len; i++)
        buf[VIRTIO_NET_HDR_SIZE + i] = frame[i];

    tx_desc[idx].addr  = buf_pa;
    tx_desc[idx].len   = VIRTIO_NET_HDR_SIZE + len;
    tx_desc[idx].flags = 0;
    tx_desc[idx].next  = 0;

    tx_avail->ring[idx] = idx;
    arch_dmb();
    tx_avail->idx++;
    arch_dmb();

    net_vio_priv[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 1;
    return 0;
}

/* ============================================================
 * plat_net_driver_fn -- RX driver thread (virtio-net)
 * ============================================================ */
void plat_net_driver_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr drv_ntfn = (seL4_CPtr)(uintptr_t)arg0;
    (void)arg1; (void)ipc_buf;

    struct virtq_desc  *rx_desc  =
        (struct virtq_desc  *)(net_dma_priv + NET_RX_DESC_OFF);
    struct virtq_avail *rx_avail =
        (struct virtq_avail *)(net_dma_priv + NET_RX_AVAIL_OFF);
    struct virtq_used  *rx_used  =
        (struct virtq_used  *)(net_dma_priv + NET_RX_USED_OFF);

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

                uint32_t h = net_rx_ring.head;
                uint32_t t = net_rx_ring.tail;
                if ((h - t) < NET_RX_RING_SIZE) {
                    struct rx_pkt_entry *entry =
                        &net_rx_ring.pkts[h % NET_RX_RING_SIZE];
                    uint8_t *src = net_dma_priv + NET_RX_BUF_OFF
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

            rx_desc[desc_idx].addr = net_dma_pa_priv + NET_RX_BUF_OFF
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
            net_vio_priv[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;
            seL4_Signal(net_srv_ntfn_cap);
        }

        uint32_t isr = net_vio_priv[VIRTIO_MMIO_INTERRUPT_STATUS / 4];
        if (isr)
            net_vio_priv[VIRTIO_MMIO_INTERRUPT_ACK / 4] = isr;
        if (net_irq_handler_priv)
            seL4_IRQHandler_Ack(net_irq_handler_priv);
    }
}

/* ============================================================
 * plat_net_get_mac -- return MAC address
 * ============================================================ */
void plat_net_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = net_mac[i];
}
