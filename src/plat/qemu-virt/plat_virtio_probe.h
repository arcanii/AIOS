/*
 * plat_virtio_probe.h -- Shared virtio MMIO device probe (QEMU virt)
 *
 * Maps the virtio MMIO region once, scans all 32 slots,
 * and caches which slots contain blk/net/gpu devices.
 * Called by plat_blk_init, results consumed by net_virtio.
 */
#ifndef PLAT_VIRTIO_PROBE_H
#define PLAT_VIRTIO_PROBE_H

#include <stdint.h>

#define PLAT_VIRTIO_SLOT_SIZE  0x200
#define PLAT_VIRTIO_NUM_SLOTS  32
#define PLAT_MAX_BLK_DEVS      4

typedef struct {
    void *vio_vaddr;
    int blk_slots[PLAT_MAX_BLK_DEVS];
    int num_blk;
    int net_slot;   /* -1 if not found */
} plat_virtio_info_t;

/* Probe virtio MMIO bus. Idempotent (second call is a no-op).
 * Sets net_available extern based on discovery.
 * Returns 0 on success, -1 on MMIO mapping failure. */
int plat_virtio_probe(void);

/* Get cached probe results. Returns NULL if probe has not run. */
const plat_virtio_info_t *plat_virtio_get_info(void);

/* Compute MMIO base pointer for a given slot index. */
volatile uint32_t *plat_virtio_slot_base(int slot);

#endif /* PLAT_VIRTIO_PROBE_H */
