/*
 * net_virtio.c -- virtio-net platform driver (QEMU virt)
 *
 * PAL implementation for PLAT_QEMU_VIRT networking.
 * Provides plat_net_init/tx/driver_fn/get_mac matching net_hal.h.
 *
 * v0.4.90: reads device slot from plat_virtio_probe instead of
 * extern bridge globals (net_vio, net_vio_slot removed).
 *
 * v0.4.236 (netd Stage 3): prov/dev split (DESIGN_NETD s7). The root-only
 * provisioning half (slot resolve, DMA alloc, IRQ bind) is gated #ifndef
 * NETD_BUILD; the device-programming half (identity, queues, MAC, drain, tx) is
 * gated #ifndef NETD_PROV. With NEITHER define (the flag-OFF monolithic build)
 * BOTH halves compile and plat_net_init() runs them in the original order, so
 * behavior is unchanged. Flag-ON: root compiles the prov half (NETD_PROV) and
 * calls plat_net_prov(); netd compiles the dev half (NETD_BUILD), attaches the
 * root-provisioned resources via plat_net_dev_attach(), and calls plat_net_init().
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
#include "plat_virtio_probe.h"

/* ---- Private state (was extern in root_shared.h) ---- */

static volatile uint32_t *net_vio_priv;
static uint8_t  *net_dma_priv;
static uint64_t  net_dma_pa_priv;
static int       net_vio_slot_priv;
static seL4_CPtr net_irq_handler_priv;

/* ============================================================
 * Provisioning half (root only) -- slot resolve, DMA alloc, IRQ bind.
 * These touch vka/simple, so netd (NETD_BUILD) never compiles them; netd
 * receives the results via plat_net_dev_attach(). DESIGN_NETD s7.
 * ============================================================ */
#ifndef NETD_BUILD
#include "aios/netd_handoff.h"

/* Retained DMA frame caps (32 x 4K) -- plat_net_prov hands these to spawn_netd
 * to copy + map into netd (DESIGN_NETD s3). */
static seL4_CPtr net_dma_caps_priv[NET_DMA_FRAMES];

/* Resolve the virtio-net slot from the shared probe and latch the MMIO base. */
static int net_slot_resolve(void) {
    const plat_virtio_info_t *vinfo = plat_virtio_get_info();
    if (!vinfo || vinfo->net_slot < 0) {
        AIOS_LOG_WARN("No virtio-net in probe results");
        net_available = 0;
        return -1;
    }
    net_vio_priv = plat_virtio_slot_base(vinfo->net_slot);
    net_vio_slot_priv = vinfo->net_slot;
    return 0;
}

/* Allocate 128KB DMA (size-17 untyped = 32 contiguous pages), map it
 * non-cacheable, and latch the vaddr/paddr. */
static int net_dma_alloc(void) {
    int error;
    vka_object_t dma_ut;
    vka_audit_untyped(VKA_SUB_NET, 17);
    error = vka_alloc_untyped(&vka, 17, &dma_ut);
    if (error) {
        AIOS_LOG_ERROR_V("DMA untyped alloc failed err=", error);
        net_available = 0;
        return -1;
    }

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
        net_dma_caps_priv[i] = slot;
    }

    void *dma_vaddr = vspace_map_pages(&vspace, net_dma_caps_priv, NULL,
        seL4_AllRights, NET_DMA_FRAMES, seL4_PageBits, 0);
    if (!dma_vaddr) {
        AIOS_LOG_ERROR("DMA map failed");
        net_available = 0;
        return -1;
    }

    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(net_dma_caps_priv[0]);
    if (ga.error) {
        AIOS_LOG_ERROR("DMA GetAddress failed");
        net_available = 0;
        return -1;
    }

    net_dma_priv = (uint8_t *)dma_vaddr;
    net_dma_pa_priv = ga.paddr;

    /* Zero DMA region */
    for (int i = 0; i < NET_DMA_SIZE; i++) net_dma_priv[i] = 0;
    return 0;
}

/* Allocate the RX IRQ notification, bind the virtio-net IRQ to a badge=1 minted
 * copy (v0.4.230: the merged net_server's bound notification wakes on it). */
static int net_irq_bind(void) {
    int error;
    vka_object_t drv_ntfn_obj;
    error = vka_alloc_notification(&vka, &drv_ntfn_obj);
    if (error) {
        AIOS_LOG_ERROR("drv notification alloc failed");
        net_available = 0;
        return -1;
    }
    net_drv_ntfn_cap = drv_ntfn_obj.cptr;

    /* Bind virtio-net IRQ to driver notification */
    {
        uint32_t net_irq = 48 + (uint32_t)net_vio_slot_priv;
        cspacepath_t nirq_path;
        int irq_err = vka_cspace_alloc_path(&vka, &nirq_path);
        if (!irq_err) {
            irq_err = simple_get_IRQ_handler(&simple, net_irq, nirq_path);
            if (!irq_err) {
                net_irq_handler_priv = nirq_path.capPtr;
                /* v0.4.230 (Stage 1): the IRQ signals a badge=1 minted copy so
                 * the merged net_server's seL4_Recv wakes with a non-zero badge
                 * (its wake test). The unbadged net_drv_ntfn_cap stays bound to
                 * the server TCB; this badged copy is what the IRQ delivers. */
                seL4_CPtr irq_ntfn = net_drv_ntfn_cap;
                cspacepath_t b1src, b1;
                vka_cspace_make_path(&vka, net_drv_ntfn_cap, &b1src);
                if (!vka_cspace_alloc_path(&vka, &b1) &&
                    !seL4_CNode_Mint(b1.root, b1.capPtr, b1.capDepth,
                                     b1src.root, b1src.capPtr, b1src.capDepth,
                                     seL4_AllRights, (seL4_Word)1))
                    irq_ntfn = b1.capPtr;
                irq_err = seL4_IRQHandler_SetNotification(
                    net_irq_handler_priv, irq_ntfn);
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
    return 0;
}

/* plat_net_prov -- root-side provisioning for the flag-ON netd path
 * (DESIGN_NETD s7). Resolves the slot, allocates DMA, binds the IRQ. netd then
 * programs the device itself in plat_net_init() after plat_net_dev_attach(). */
int plat_net_prov(driver_handoff_t *ho) {
    if (!net_available) return -1;
    if (net_slot_resolve() != 0) return -1;
    if (net_dma_alloc() != 0) return -1;
    if (net_irq_bind() != 0) return -1;

    /* Fill the handoff for spawn_netd (DESIGN_NETD s3). The MMIO frames are the
     * 4 virtio-window pages the shared probe mapped; netd re-maps them at the
     * same vaddr and uses slot to find the per-slot register base. QEMU reads its
     * MAC from config space in dev-init, so ho->mac stays 0. */
    const plat_virtio_info_t *vinfo = plat_virtio_get_info();
    for (int i = 0; i < PLAT_VIRTIO_PAGES; i++)
        ho->mmio_frames[i] = vinfo ? vinfo->vio_frame_caps[i] : 0;
    ho->mmio_nframes = PLAT_VIRTIO_PAGES;
    ho->mmio_vaddr   = (uintptr_t)(vinfo ? vinfo->vio_vaddr : 0);
    ho->slot         = net_vio_slot_priv;
    for (int i = 0; i < NET_DMA_FRAMES; i++)
        ho->dma_frames[i] = net_dma_caps_priv[i];
    ho->dma_nframes  = NET_DMA_FRAMES;
    ho->dma_vaddr    = (uintptr_t)net_dma_priv;
    ho->dma_paddr    = net_dma_pa_priv;
    ho->irq_handler  = net_irq_handler_priv;
    ho->irq_ntfn     = net_drv_ntfn_cap;
    for (int i = 0; i < 6; i++) ho->mac[i] = net_mac[i];
    return 0;
}

#endif /* !NETD_BUILD */

/* ============================================================
 * netd attach -- netd latches the root-provisioned MMIO/DMA/IRQ (passed via
 * argv) before plat_net_init() programs the device. DESIGN_NETD s3.
 * ============================================================ */
#ifdef NETD_BUILD
void plat_net_dev_attach(uintptr_t mmio_vaddr, int slot, uintptr_t dma_vaddr,
                         uint64_t dma_paddr, seL4_CPtr irq_handler,
                         const uint8_t mac[6]) {
    /* mmio_vaddr is the BASE of the virtio MMIO window root mapped into netd
     * (PLAT_VIRTIO_PAGES pages); the per-device register block is at
     * base + slot*0x200 (8 x 0x200 slots per page). This matches the monolithic
     * plat_virtio_slot_base(slot) = probe_vaddr + slot*0x200. DESIGN_NETD s3. */
    net_vio_priv = (volatile uint32_t *)(mmio_vaddr + (uintptr_t)slot * PLAT_VIRTIO_SLOT_SIZE);
    net_vio_slot_priv = slot;
    net_dma_priv = (uint8_t *)dma_vaddr;
    net_dma_pa_priv = dma_paddr;
    net_irq_handler_priv = irq_handler;
    (void)mac;   /* QEMU reads the MAC from config space in plat_net_init() */
}
#endif

/* ============================================================
 * Device-programming half (netd + monolithic) -- identity, queues, MAC,
 * DRIVER_OK, TX, RX drain. Gated #ifndef NETD_PROV so root's prov-only build
 * does not define these (link-time check: root no longer references them).
 * ============================================================ */
#ifndef NETD_PROV

/* ============================================================
 * plat_net_init -- program the virtio-net device.
 *
 * Monolithic (flag-OFF): provisions inline via the #ifndef NETD_BUILD helpers,
 * in the original order, then programs the device -- behavior unchanged.
 * netd (NETD_BUILD): the helpers are gated out; netd has already attached the
 * root-provisioned resources via plat_net_dev_attach().
 * ============================================================ */
int plat_net_init(void) {
    if (!net_available) return -1;

#ifndef NETD_BUILD
    /* Monolithic provisioning (flag-OFF). Flag-ON root does this via
     * plat_net_prov() instead, before netd starts. */
    if (net_slot_resolve() != 0) return -1;
#endif

    /* Verify device identity */
    if (net_vio_priv[VIRTIO_MMIO_MAGIC / 4] != VIRTIO_MAGIC ||
        net_vio_priv[VIRTIO_MMIO_DEVICE_ID / 4] != VIRTIO_NET_DEVICE_ID) {
        printf("[net] Bad device at slot %d\n", net_vio_slot_priv);
        net_available = 0;
        return -1;
    }

#ifndef NETD_BUILD
    if (net_dma_alloc() != 0) return -1;
#endif

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

#ifndef NETD_BUILD
    /* RX IRQ notification (v0.4.230: bound to the net_server TCB in
     * boot_services; the old separate net_srv_ntfn / driver thread are gone).
     * Flag-ON root binds this in plat_net_prov(); netd inherits the handler via
     * plat_net_dev_attach() and self-binds the notification to its own TCB. */
    if (net_irq_bind() != 0) return -1;
#endif

    printf("[boot] virtio-net ready, MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
           net_mac[0], net_mac[1], net_mac[2],
           net_mac[3], net_mac[4], net_mac[5]);
#ifndef NETD_BUILD
    LOG_INFO("virtio-net initialized");   /* root log ring; netd has no log ring (logs via printf) */
#endif
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
 * plat_net_drain -- drain the virtio-net RX used-ring into net_rx_ring.
 *
 * v0.4.230 (netd Stage 1): this is the old driver-thread body, minus the
 * seL4_Wait (the merged net_server's bound notification wakes its Recv) and the
 * cross-thread Signal (it processes net_rx_ring itself right after this call).
 * The do/while re-check after the IRQ ack consumes a frame that completed
 * during the ack window instead of waiting for the next IRQ (DESIGN_NETD s2).
 * ============================================================ */
void plat_net_drain(void) {
    static uint16_t rx_last_used = 0;   /* persists across calls (was thread-local) */

    struct virtq_desc  *rx_desc  =
        (struct virtq_desc  *)(net_dma_priv + NET_RX_DESC_OFF);
    struct virtq_avail *rx_avail =
        (struct virtq_avail *)(net_dma_priv + NET_RX_AVAIL_OFF);
    struct virtq_used  *rx_used  =
        (struct virtq_used  *)(net_dma_priv + NET_RX_USED_OFF);

    do {
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
                    net_rx_stats.ring_overflow_drops++;
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

        if (drained > 0)
            net_vio_priv[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

        uint32_t isr = net_vio_priv[VIRTIO_MMIO_INTERRUPT_STATUS / 4];
        if (isr)
            net_vio_priv[VIRTIO_MMIO_INTERRUPT_ACK / 4] = isr;
        if (net_irq_handler_priv)
            seL4_IRQHandler_Ack(net_irq_handler_priv);
    } while (rx_used->idx != rx_last_used);
}

/* ============================================================
 * plat_net_get_mac -- return MAC address
 * ============================================================ */
void plat_net_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = net_mac[i];
}

#endif /* !NETD_PROV */
