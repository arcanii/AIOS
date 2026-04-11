/*
 * boot_net_init.c -- virtio-net device initialization
 *
 * Called from main() after boot_fs_init() detects a net device.
 * Allocates 128KB DMA, initializes RX/TX virtqueues, reads MAC.
 * Allocates notification objects for driver/server communication.
 */
#include "aios/root_shared.h"
#include "aios/net.h"
#include "aios/vka_audit.h"
#include "virtio.h"
#define LOG_MODULE "net"
#define LOG_LEVEL LOG_LEVEL_DEBUG
#include "aios/aios_log.h"
#include <stdio.h>
#include "arch.h"

void boot_net_init(void) {
    if (!net_available) return;
    int error;

    /* Verify device identity */
    if (net_vio[VIRTIO_MMIO_MAGIC / 4] != VIRTIO_MAGIC ||
        net_vio[VIRTIO_MMIO_DEVICE_ID / 4] != VIRTIO_NET_DEVICE_ID) {
        printf("[net] Bad device at slot %d\n", net_vio_slot);
        net_available = 0;
        return;
    }

    /* Allocate 128KB DMA (size-17 untyped = 32 contiguous pages) */
    vka_object_t dma_ut;
    vka_audit_untyped(VKA_SUB_NET, 17);
    error = vka_alloc_untyped(&vka, 17, &dma_ut);
    if (error) {
        printf("[net] DMA untyped alloc failed: %d\n", error);
        net_available = 0;
        return;
    }

    seL4_CPtr dma_caps[NET_DMA_FRAMES];
    for (int i = 0; i < NET_DMA_FRAMES; i++) {
        seL4_CPtr slot;
        error = vka_cspace_alloc(&vka, &slot);
        if (error) {
            printf("[net] DMA cslot alloc failed at %d\n", i);
            net_available = 0;
            return;
        }
        error = seL4_Untyped_Retype(dma_ut.cptr,
            ARCH_PAGE_OBJECT, seL4_PageBits,
            seL4_CapInitThreadCNode, 0, 0, slot, 1);
        if (error) {
            printf("[net] DMA retype %d failed: %d\n", i, error);
            net_available = 0;
            return;
        }
        dma_caps[i] = slot;
    }

    void *dma_vaddr = vspace_map_pages(&vspace, dma_caps, NULL,
        seL4_AllRights, NET_DMA_FRAMES, seL4_PageBits, 0);
    if (!dma_vaddr) {
        printf("[net] DMA map failed\n");
        net_available = 0;
        return;
    }

    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_caps[0]);
    if (ga.error) {
        printf("[net] DMA GetAddress failed\n");
        net_available = 0;
        return;
    }

    net_dma = (uint8_t *)dma_vaddr;
    net_dma_pa = ga.paddr;

    /* Zero DMA region */
    for (int i = 0; i < NET_DMA_SIZE; i++) net_dma[i] = 0;

    /* Legacy virtio init sequence */
    net_vio[VIRTIO_MMIO_STATUS / 4] = 0;
    net_vio[VIRTIO_MMIO_STATUS / 4] = VIRTIO_STATUS_ACK;
    net_vio[VIRTIO_MMIO_STATUS / 4] = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER;

    /* Feature negotiation (MAC only for M1, no CSUM offload yet) */
    uint32_t host_feat = net_vio[VIRTIO_MMIO_HOST_FEATURES / 4];
    uint32_t drv_feat = 0;
    if (host_feat & VIRTIO_NET_F_MAC)
        drv_feat |= VIRTIO_NET_F_MAC;
    net_vio[VIRTIO_MMIO_DRV_FEATURES / 4] = drv_feat;
    net_vio[VIRTIO_MMIO_GUEST_PAGE_SIZE / 4] = 4096;

    /* Setup RX queue (queue 0) */
    net_vio[VIRTIO_MMIO_QUEUE_SEL / 4] = 0;
    uint32_t rx_qmax = net_vio[VIRTIO_MMIO_QUEUE_NUM_MAX / 4];
    if (rx_qmax < NET_QUEUE_SIZE) {
        printf("[net] RX queue too small: %u\n", rx_qmax);
        net_available = 0;
        return;
    }
    net_vio[VIRTIO_MMIO_QUEUE_NUM / 4] = NET_QUEUE_SIZE;
    net_vio[VIRTIO_MMIO_QUEUE_ALIGN / 4] = 4096;
    net_vio[VIRTIO_MMIO_QUEUE_PFN / 4] =
        (uint32_t)(net_dma_pa / 4096);

    /* Setup TX queue (queue 1) */
    net_vio[VIRTIO_MMIO_QUEUE_SEL / 4] = 1;
    uint32_t tx_qmax = net_vio[VIRTIO_MMIO_QUEUE_NUM_MAX / 4];
    if (tx_qmax < NET_QUEUE_SIZE) {
        printf("[net] TX queue too small: %u\n", tx_qmax);
        net_available = 0;
        return;
    }
    net_vio[VIRTIO_MMIO_QUEUE_NUM / 4] = NET_QUEUE_SIZE;
    net_vio[VIRTIO_MMIO_QUEUE_ALIGN / 4] = 4096;
    net_vio[VIRTIO_MMIO_QUEUE_PFN / 4] =
        (uint32_t)((net_dma_pa + NET_TX_DESC_OFF) / 4096);

    /* Replenish all 16 RX descriptors */
    struct virtq_desc *rx_desc =
        (struct virtq_desc *)(net_dma + NET_RX_DESC_OFF);
    struct virtq_avail *rx_avail =
        (struct virtq_avail *)(net_dma + NET_RX_AVAIL_OFF);

    for (int i = 0; i < NET_QUEUE_SIZE; i++) {
        rx_desc[i].addr =
            net_dma_pa + NET_RX_BUF_OFF + i * NET_PKT_BUF_SIZE;
        rx_desc[i].len   = NET_PKT_BUF_SIZE;
        rx_desc[i].flags = VIRTQ_DESC_F_WRITE;
        rx_desc[i].next  = 0;
        rx_avail->ring[i] = i;
    }
    rx_avail->idx = NET_QUEUE_SIZE;
    arch_dmb();
    net_vio[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;  /* kick RX queue */

    /* Read MAC from config space (6 bytes at offset 0x100) */
    volatile uint8_t *cfg = (volatile uint8_t *)
        ((uintptr_t)net_vio + VIRTIO_MMIO_CONFIG);
    for (int i = 0; i < 6; i++) net_mac[i] = cfg[i];

    /* Driver OK */
    net_vio[VIRTIO_MMIO_STATUS / 4] =
        VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK;

    /* Re-kick RX queue now that device is live */
    net_vio[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

    /* Allocate notification objects for driver/server IPC */
    vka_object_t drv_ntfn_obj, srv_ntfn_obj;
    error = vka_alloc_notification(&vka, &drv_ntfn_obj);
    if (error) {
        printf("[net] drv notification alloc failed\n");
        net_available = 0;
        return;
    }
    error = vka_alloc_notification(&vka, &srv_ntfn_obj);
    if (error) {
        printf("[net] srv notification alloc failed\n");
        net_available = 0;
        return;
    }
    net_drv_ntfn_cap = drv_ntfn_obj.cptr;
    net_srv_ntfn_cap = srv_ntfn_obj.cptr;

    printf("[boot] virtio-net ready, MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
           net_mac[0], net_mac[1], net_mac[2],
           net_mac[3], net_mac[4], net_mac[5]);
    LOG_INFO("virtio-net initialized");
}
