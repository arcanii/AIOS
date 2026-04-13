/*
 * plat_virtio_probe.c -- Shared virtio MMIO device probe (QEMU virt)
 *
 * Maps the virtio MMIO region (4 pages) and scans all 32 device
 * slots in a single pass. Results are cached in a file-static
 * struct and exposed via plat_virtio_get_info().
 *
 * Extracted from blk_virtio.c during v0.4.90 PAL Step 7 cleanup.
 */
#include "plat_virtio_probe.h"
#include "aios/root_shared.h"
#include "virtio.h"
#include <sel4platsupport/device.h>
#include <stdio.h>
#include "aios/hw_info.h"

static plat_virtio_info_t probe_info;
static int probe_done = 0;

int plat_virtio_probe(void) {
    if (probe_done) return 0;

    int error;
    vka_object_t vio_frames[4];
    seL4_CPtr vio_caps[4];
    int vio_ok = 1;
    for (int p = 0; p < 4; p++) {
        error = sel4platsupport_alloc_frame_at(&vka,
            hw_info.virtio_base + p * 0x1000, seL4_PageBits, &vio_frames[p]);
        if (error) { vio_ok = 0; break; }
        vio_caps[p] = vio_frames[p].cptr;
    }
    if (!vio_ok) {
        printf("[plat] Failed to alloc virtio MMIO frames\n");
        return -1;
    }

    void *vaddr = vspace_map_pages(&vspace, vio_caps, NULL,
        seL4_AllRights, 4, seL4_PageBits, 0);
    if (!vaddr) {
        printf("[plat] Failed to map virtio MMIO\n");
        return -1;
    }

    probe_info.vio_vaddr = vaddr;
    probe_info.num_blk   = 0;
    probe_info.net_slot  = -1;

    for (int i = 0; i < PLAT_VIRTIO_NUM_SLOTS; i++) {
        volatile uint32_t *sl = (volatile uint32_t *)
            ((uintptr_t)vaddr + i * PLAT_VIRTIO_SLOT_SIZE);
        if (sl[0] != VIRTIO_MAGIC) continue;
        uint32_t devid = sl[VIRTIO_MMIO_DEVICE_ID / 4];

        if (devid == VIRTIO_BLK_DEVICE_ID &&
            probe_info.num_blk < PLAT_MAX_BLK_DEVS) {
            probe_info.blk_slots[probe_info.num_blk++] = i;
        }
        if (devid == VIRTIO_NET_DEVICE_ID && probe_info.net_slot < 0) {
            probe_info.net_slot = i;
        }
    }

    /* Set global net_available for boot_services.c */
    net_available = (probe_info.net_slot >= 0) ? 1 : 0;
    /* GPU: display uses fw_cfg/ramfb, not virtio-gpu */
    gpu_available = 0;

    printf("[plat] Virtio probe: %d blk, net=%s\n",
           probe_info.num_blk,
           net_available ? "yes" : "no");

    probe_done = 1;
    return 0;
}

const plat_virtio_info_t *plat_virtio_get_info(void) {
    return probe_done ? &probe_info : NULL;
}

volatile uint32_t *plat_virtio_slot_base(int slot) {
    if (!probe_done || slot < 0 || slot >= PLAT_VIRTIO_NUM_SLOTS) return NULL;
    return (volatile uint32_t *)
        ((uintptr_t)probe_info.vio_vaddr + slot * PLAT_VIRTIO_SLOT_SIZE);
}
