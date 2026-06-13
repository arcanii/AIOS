/*
 * netd_handoff.h -- root-side provisioning result for the netd leaf driver.
 *
 * netd de-monolithization (DESIGN_NETD s3). plat_net_prov() fills this in the
 * root task: the device-MMIO + DMA frame caps (so spawn_netd can copy + map them
 * into netd), their vaddr/paddr, the IRQ handler + the unbadged notification, the
 * slot, and the MAC read at prov time. spawn_netd copies the caps into netd and
 * passes the scalars via argv (DESIGN_NETD s3 table).
 *
 * Cap-pointer-only (no vka_object_t): v1 hands over cptrs for copy+map. Freeing
 * the underlying objects on respawn (v2) is a follow-up; root retains the source
 * vka objects separately for that.
 */
#ifndef AIOS_NETD_HANDOFF_H
#define AIOS_NETD_HANDOFF_H

#include <stdint.h>
#include <sel4/sel4.h>
#include "aios/net.h"          /* NET_DMA_FRAMES */

/* Max device-MMIO frames across platforms: RPi4 GENET = 16, QEMU virtio = 4. */
#define NETD_MAX_MMIO_FRAMES  16

typedef struct driver_handoff {
    /* Device MMIO: frame caps + the root vaddr they are mapped at. netd maps the
     * same frames at the same vaddr. The per-device register base is
     * mmio_vaddr + slot*0x200 on QEMU (virtio slot window) or mmio_vaddr itself
     * on RPi4 (single GENET block, slot unused). */
    seL4_CPtr mmio_frames[NETD_MAX_MMIO_FRAMES];
    int       mmio_nframes;
    uintptr_t mmio_vaddr;
    int       slot;

    /* 128KB DMA: 32 retyped 4K frame caps, the root map vaddr, the bus paddr.
     * Non-cacheable both ends (attribute-identical, the A72 rule). */
    seL4_CPtr dma_frames[NET_DMA_FRAMES];
    int       dma_nframes;
    uintptr_t dma_vaddr;
    uint64_t  dma_paddr;

    /* IRQ: the handler (netd Acks per drain) and the UNBADGED notification (netd
     * copies it and self-binds it to its own TCB). Root keeps the badge-1 mint
     * bound to the IRQ via IRQHandler_SetNotification and the badge-2 kick mint. */
    seL4_CPtr irq_handler;
    seL4_CPtr irq_ntfn;

    uint8_t   mac[6];          /* MAC read at prov time (RPi4 mailbox; QEMU 0) */
} driver_handoff_t;

#endif /* AIOS_NETD_HANDOFF_H */
