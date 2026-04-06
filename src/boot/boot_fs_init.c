/*
 * boot_fs_init.c -- Virtio-blk discovery + ext2 filesystem init
 *
 * Extracted from aios_root.c (v0.4.53 modularization).
 * Probes virtio MMIO for a block device, initializes the DMA ring,
 * reads the ext2 superblock, and mounts / and /proc.
 */
#include "aios/root_shared.h"
#include "virtio.h"
#include "aios/ext2.h"
#include "aios/vfs.h"
#include "aios/procfs.h"
#define LOG_MODULE "boot"
#define LOG_LEVEL LOG_LEVEL_DEBUG
#include "aios/aios_log.h"
#include <sel4platsupport/device.h>
#include <stdio.h>

#define VIRTIO_BASE_ADDR 0xa000000UL
#define VIRTIO_SLOT_SIZE 0x200
#define VIRTIO_NUM_SLOTS 32

#define VIO_R(off) vio[(off)/4]
#define VIO_W(off, val) vio[(off)/4] = (val)

void boot_fs_init(void) {
    int error;

    /* Map 4 pages of virtio MMIO */
    vka_object_t vio_frames[4];
    seL4_CPtr vio_caps[4];
    int vio_ok = 1;
    for (int p = 0; p < 4; p++) {
        error = sel4platsupport_alloc_frame_at(&vka,
            VIRTIO_BASE_ADDR + p * 0x1000, seL4_PageBits, &vio_frames[p]);
        if (error) { vio_ok = 0; break; }
        vio_caps[p] = vio_frames[p].cptr;
    }
    if (!vio_ok) {
        printf("[fs] Failed to alloc virtio frames\n");
        return;
    }
    void *vio_vaddr = vspace_map_pages(&vspace, vio_caps, NULL,
        seL4_AllRights, 4, seL4_PageBits, 0);
    if (!vio_vaddr) {
        printf("[fs] Failed to map virtio\n");
        return;
    }

    /* Find block device */
    int blk_slot = -1;
    for (int i = 0; i < VIRTIO_NUM_SLOTS; i++) {
        volatile uint32_t *slot = (volatile uint32_t *)((uintptr_t)vio_vaddr + i * VIRTIO_SLOT_SIZE);
        if (slot[0] == VIRTIO_MAGIC && slot[VIRTIO_MMIO_DEVICE_ID/4] == VIRTIO_BLK_DEVICE_ID) {
            blk_slot = i;
            break;
        }
    }
    if (blk_slot < 0) {
        printf("[fs] No block device (add -drive to QEMU)\n");
        return;
    }
    volatile uint32_t *vio = (volatile uint32_t *)((uintptr_t)vio_vaddr + blk_slot * VIRTIO_SLOT_SIZE);

    /* Allocate 16K contiguous DMA via single untyped */
    vka_object_t dma_ut;
    error = vka_alloc_untyped(&vka, 14, &dma_ut);
    if (error) {
        printf("[fs] DMA untyped alloc failed: %d\n", error);
        return;
    }

    /* Retype untyped into 4 contiguous frames */
    seL4_CPtr dma_caps[4];
    for (int i = 0; i < 4; i++) {
        seL4_CPtr slot;
        error = vka_cspace_alloc(&vka, &slot);
        if (error) { printf("[fs] DMA cslot alloc failed\n"); return; }
        error = seL4_Untyped_Retype(dma_ut.cptr,
            seL4_ARM_SmallPageObject, seL4_PageBits,
            seL4_CapInitThreadCNode, 0, 0, slot, 1);
        if (error) { printf("[fs] DMA retype %d failed: %d\n", i, error); return; }
        dma_caps[i] = slot;
    }

    /* Map DMA pages */
    void *dma_vaddr = vspace_map_pages(&vspace, dma_caps, NULL,
        seL4_AllRights, 4, seL4_PageBits, 0);
    if (!dma_vaddr) {
        printf("[fs] DMA map failed\n");
        return;
    }

    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_caps[0]);
    if (ga.error) { printf("[fs] DMA GetAddress failed\n"); return; }
    uint64_t dma_pa = ga.paddr;

    /* Zero DMA region */
    uint8_t *dma = (uint8_t *)dma_vaddr;
    for (int i = 0; i < 16384; i++) dma[i] = 0;

    /* Legacy virtio init */
    VIO_W(VIRTIO_MMIO_STATUS, 0);
    VIO_W(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    VIO_W(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    VIO_W(VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
    VIO_W(VIRTIO_MMIO_DRV_FEATURES, 0);
    VIO_W(VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = VIO_R(VIRTIO_MMIO_QUEUE_NUM_MAX);
    uint32_t qsz = qmax < 16 ? qmax : 16;
    VIO_W(VIRTIO_MMIO_QUEUE_NUM, qsz);
    VIO_W(VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(dma_pa / 4096));
    VIO_W(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    /* Read sector 2 (ext2 superblock) */
    struct virtq_desc  *desc  = (struct virtq_desc *)(dma);
    struct virtq_avail *avail = (struct virtq_avail *)(dma + 0x100);
    struct virtq_used  *used  = (struct virtq_used  *)(dma + 0x1000);
    struct virtio_blk_req *req = (struct virtio_blk_req *)(dma + 0x2000);
    uint64_t req_pa = dma_pa + 0x2000;

    req->type = VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = 2;
    req->status = 0xFF;

    desc[0].addr  = req_pa;      desc[0].len = 16;
    desc[0].flags = VIRTQ_DESC_F_NEXT; desc[0].next = 1;
    desc[1].addr  = req_pa + 16; desc[1].len = 512;
    desc[1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT; desc[1].next = 2;
    desc[2].addr  = req_pa + 16 + 512; desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE; desc[2].next = 0;

    __asm__ volatile("dmb sy" ::: "memory");
    avail->ring[avail->idx % qsz] = 0;
    __asm__ volatile("dmb sy" ::: "memory");
    avail->idx += 1;
    __asm__ volatile("dmb sy" ::: "memory");
    VIO_W(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    uint16_t last_used = 0;
    int done = 0;
    for (int t = 0; t < 10000000; t++) {
        __asm__ volatile("dmb sy" ::: "memory");
        if (used->idx != last_used) { done = 1; break; }
    }
    VIO_R(VIRTIO_MMIO_INTERRUPT_STATUS);
    VIO_W(VIRTIO_MMIO_INTERRUPT_ACK, 1);

    if (!done) { printf("[fs] Read timeout\n"); return; }
    if (req->status != 0) { printf("[fs] Read error status=%u\n", req->status); return; }

    /* Check ext2 magic */
    uint16_t ext2_magic = req->data[0x38] | (req->data[0x39] << 8);
    if (ext2_magic != 0xEF53) {
        printf("[fs] ext2 not found (got 0x%04x)\n", ext2_magic);
        return;
    }

    /* Save virtio state for fs thread */
    blk_vio = vio;
    blk_dma = dma;
    blk_dma_pa = dma_pa;

    /* Init ext2 + VFS */
    vfs_init();
    proc_init();
    int fs_err = ext2_init(&ext2, blk_read_sector);
    if (fs_err == 0) {
        ext2_init_write(&ext2, blk_write_sector);
        vfs_mount("/", &ext2_fs_ops, &ext2);
        vfs_mount("/proc", &procfs_ops, NULL);
        proc_add("fs_thread", 200);
        proc_add("exec_thread", 200);
        proc_add("thread_server", 200);
        LOG_INFO("ext2 + procfs mounted");
        printf("[boot] Filesystems mounted\n");
    } else {
        printf("[fs] ext2 init failed: %d\n", fs_err);
    }
}
