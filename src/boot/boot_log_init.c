/*
 * boot_log_init.c -- Second virtio-blk device for log drive
 *
 * Called from boot_fs_init when a second block device is found.
 * Allocates DMA, initializes virtio, mounts ext2 at /log.
 */
#include "aios/root_shared.h"
#include "aios/vka_audit.h"
#include "virtio.h"
#include "aios/ext2.h"
#include "aios/vfs.h"
#define LOG_MODULE "boot"
#define LOG_LEVEL LOG_LEVEL_DEBUG
#include "aios/aios_log.h"
#include <sel4platsupport/device.h>
#include <stdio.h>

#define VIRTIO_SLOT_SIZE 0x200

void boot_log_drive_init(void *vio_vaddr, int log_slot) {
    int error;
    volatile uint32_t *vio = (volatile uint32_t *)
        ((uintptr_t)vio_vaddr + log_slot * VIRTIO_SLOT_SIZE);

    /* /log mount point: VFS prefix matching handles dispatch,
     * no directory needed on root ext2. Calling ext2_mkdir here
     * overflows the root task stack (ext2_alloc_block uses 3KB
     * of local buffers, boot call chain adds 2KB more). */

    /* Allocate 16K contiguous DMA via single untyped */
    vka_object_t dma_ut;
    vka_audit_untyped(VKA_SUB_BOOT, 14);
    error = vka_alloc_untyped(&vka, 14, &dma_ut);
    if (error) {
        printf("[boot] Log DMA alloc failed: %d\n", error);
        return;
    }

    /* Retype into 4 contiguous frames */
    seL4_CPtr dma_caps[4];
    for (int i = 0; i < 4; i++) {
        seL4_CPtr slot;
        error = vka_cspace_alloc(&vka, &slot);
        if (error) { printf("[boot] Log cslot failed\n"); return; }
        error = seL4_Untyped_Retype(dma_ut.cptr,
            seL4_ARM_SmallPageObject, seL4_PageBits,
            seL4_CapInitThreadCNode, 0, 0, slot, 1);
        if (error) { printf("[boot] Log retype %d failed\n", i); return; }
        dma_caps[i] = slot;
    }

    /* Map DMA pages */
    void *dma_vaddr = vspace_map_pages(&vspace, dma_caps, NULL,
        seL4_AllRights, 4, seL4_PageBits, 0);
    if (!dma_vaddr) { printf("[boot] Log DMA map failed\n"); return; }

    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_caps[0]);
    if (ga.error) { printf("[boot] Log GetAddress failed\n"); return; }
    uint64_t dma_pa = ga.paddr;

    /* Zero DMA region */
    uint8_t *dma = (uint8_t *)dma_vaddr;
    for (int i = 0; i < 16384; i++) dma[i] = 0;

    /* Legacy virtio init */
    vio[VIRTIO_MMIO_STATUS / 4] = 0;
    vio[VIRTIO_MMIO_STATUS / 4] = VIRTIO_STATUS_ACK;
    vio[VIRTIO_MMIO_STATUS / 4] = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER;
    vio[VIRTIO_MMIO_GUEST_PAGE_SIZE / 4] = 4096;
    vio[VIRTIO_MMIO_DRV_FEATURES / 4] = 0;
    vio[VIRTIO_MMIO_QUEUE_SEL / 4] = 0;
    uint32_t qmax = vio[VIRTIO_MMIO_QUEUE_NUM_MAX / 4];
    uint32_t qsz = qmax < 16 ? qmax : 16;
    vio[VIRTIO_MMIO_QUEUE_NUM / 4] = qsz;
    vio[VIRTIO_MMIO_QUEUE_PFN / 4] = (uint32_t)(dma_pa / 4096);
    vio[VIRTIO_MMIO_STATUS / 4] = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER
                                 | VIRTIO_STATUS_DRIVER_OK;

    /* Save log device state */
    blk_vio_log = vio;
    blk_dma_log = dma;
    blk_dma_pa_log = dma_pa;

    /* Init ext2 on log drive (dev_id=1 for separate cache namespace) */
    int err = ext2_init(&ext2_log, blk_read_sector_log, 1);
    if (err == 0) {
        ext2_init_write(&ext2_log, blk_write_sector_log);
        vfs_mount("/log", &ext2_fs_ops, &ext2_log);
        LOG_INFO("log drive mounted at /log");
        printf("[boot] Log drive mounted at /log\n");
    } else {
        printf("[boot] Log ext2 failed: %d (not formatted?)\n", err);
    }
}
