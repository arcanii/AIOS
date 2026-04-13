/*
 * boot_fs_init.c -- Virtio-blk discovery + ext2 filesystem init
 *
 * Probes ALL virtio-mmio block devices. Reads each ext2 superblock
 * and checks the volume label (s_volume_name). Drives labeled
 * "aios-log" are handed to boot_log_drive_init. The first
 * unlabeled ext2 drive becomes the root filesystem.
 */
#include "aios/root_shared.h"
#include "aios/vka_audit.h"
#include "virtio.h"
#include "aios/ext2.h"
#include "aios/gpu.h"
#include "aios/vfs.h"
#include "aios/procfs.h"
#define LOG_MODULE "boot"
#define LOG_LEVEL LOG_LEVEL_DEBUG
#include "aios/aios_log.h"
#include <sel4platsupport/device.h>
#include <stdio.h>
#include "arch.h"
#include "aios/hw_info.h"
#include "plat/blk_hal.h"

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
            hw_info.virtio_base + p * 0x1000, seL4_PageBits, &vio_frames[p]);
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

    /* Discover all device slots (block + net) */
    int blk_slots[4];
    int num_blk_devs = 0;
    int net_slot_found = -1;
    int gpu_slot_found = -1;
    for (int i = 0; i < VIRTIO_NUM_SLOTS; i++) {
        volatile uint32_t *sl = (volatile uint32_t *)
            ((uintptr_t)vio_vaddr + i * VIRTIO_SLOT_SIZE);
        if (sl[0] != VIRTIO_MAGIC) continue;
        uint32_t devid = sl[VIRTIO_MMIO_DEVICE_ID/4];
        if (devid == VIRTIO_BLK_DEVICE_ID && num_blk_devs < 4) {
            blk_slots[num_blk_devs++] = i;
        }
        if (devid == VIRTIO_NET_DEVICE_ID && net_slot_found < 0) {
            net_slot_found = i;
        }
        if (devid == VIRTIO_GPU_DEVICE_ID && gpu_slot_found < 0) {
            gpu_slot_found = i;
        }
    }
    if (num_blk_devs == 0) {
        printf("[fs] No block device (add -drive to QEMU)\n");
        return;
    }
    printf("[fs] Found %d block device(s)\n", num_blk_devs);

    /* Allocate 16K contiguous DMA via single untyped */
    vka_object_t dma_ut;
    vka_audit_untyped(VKA_SUB_BOOT, 14);
    error = vka_alloc_untyped(&vka, 14, &dma_ut);
    if (error) {
        printf("[fs] DMA untyped alloc failed: %d\n", error);
        return;
    }
    seL4_CPtr dma_caps[4];
    for (int i = 0; i < 4; i++) {
        seL4_CPtr slot;
        error = vka_cspace_alloc(&vka, &slot);
        if (error) { printf("[fs] DMA cslot alloc failed\n"); return; }
        error = seL4_Untyped_Retype(dma_ut.cptr,
            ARCH_PAGE_OBJECT, seL4_PageBits,
            seL4_CapInitThreadCNode, 0, 0, slot, 1);
        if (error) { printf("[fs] DMA retype %d failed: %d\n", i, error); return; }
        dma_caps[i] = slot;
    }
    void *dma_vaddr = vspace_map_pages(&vspace, dma_caps, NULL,
        seL4_AllRights, 4, seL4_PageBits, 0);
    if (!dma_vaddr) { printf("[fs] DMA map failed\n"); return; }

    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_caps[0]);
    if (ga.error) { printf("[fs] DMA GetAddress failed\n"); return; }
    uint64_t dma_pa = ga.paddr;

    /* Probe each block device: read superblock, check s_volume_name.
     * Reuse the same DMA buffer for probing (zeroed between probes). */
    int blk_slot = -1;
    int log_slot_found = -1;
    volatile uint32_t *vio = NULL;

    for (int d = 0; d < num_blk_devs; d++) {
        vio = (volatile uint32_t *)
            ((uintptr_t)vio_vaddr + blk_slots[d] * VIRTIO_SLOT_SIZE);

        /* Zero DMA region (resets avail/used indices for clean probe) */
        uint8_t *dma = (uint8_t *)dma_vaddr;
        for (int z = 0; z < 16384; z++) dma[z] = 0;

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
        VIO_W(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER
              | VIRTIO_STATUS_DRIVER_OK);

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

        arch_dmb();
        avail->ring[avail->idx % qsz] = 0;
        arch_dmb();
        avail->idx += 1;
        arch_dmb();
        VIO_W(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

        uint16_t last_used = 0;
        int probe_done = 0;
        for (int t = 0; t < 10000000; t++) {
            arch_dmb();
            if (used->idx != last_used) { probe_done = 1; break; }
        }
        arch_dmb();
        VIO_R(VIRTIO_MMIO_INTERRUPT_STATUS);
        VIO_W(VIRTIO_MMIO_INTERRUPT_ACK, 1);

        if (!probe_done || req->status != 0) {
            printf("[fs] Slot %d: read failed\n", blk_slots[d]);
            VIO_W(VIRTIO_MMIO_STATUS, 0);
            continue;
        }

        /* Check ext2 magic */
        uint16_t ext2_magic = req->data[0x38] | (req->data[0x39] << 8);
        if (ext2_magic != 0xEF53) {
            printf("[fs] Slot %d: not ext2 (0x%04x)\n",
                   blk_slots[d], ext2_magic);
            VIO_W(VIRTIO_MMIO_STATUS, 0);
            continue;
        }

        /* Check s_volume_name at superblock offset 0x78 (16 bytes) */
        int is_log = (req->data[0x78] == 'a' && req->data[0x79] == 'i' &&
                      req->data[0x7a] == 'o' && req->data[0x7b] == 's' &&
                      req->data[0x7c] == '-' && req->data[0x7d] == 'l' &&
                      req->data[0x7e] == 'o' && req->data[0x7f] == 'g');
        if (is_log) {
            printf("[fs] Slot %d: log drive (aios-log)\n", blk_slots[d]);
            log_slot_found = blk_slots[d];
            VIO_W(VIRTIO_MMIO_STATUS, 0);
            continue;
        }

        /* This is the system disk */
        blk_slot = blk_slots[d];
        printf("[fs] Slot %d: system disk\n", blk_slot);
        break;
    }

    if (blk_slot < 0) {
        printf("[fs] No system disk found\n");
        return;
    }

    /* Save virtio state for fs thread */
    blk_vio = vio;
    blk_dma = (uint8_t *)dma_vaddr;
    blk_dma_pa = dma_pa;

    /* Init ext2 + VFS */
    vfs_init();
    proc_init();
    int fs_err = ext2_init(&ext2, plat_blk_read, 0);
    if (fs_err == 0) {
        ext2_init_write(&ext2, plat_blk_write);
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

    /* Mount log drive if found during probe */
    if (log_slot_found >= 0) {
        boot_log_drive_init(vio_vaddr, log_slot_found);
    } else {
        printf("[boot] No log drive (optional)\n");
    }

    /* Record network device for boot_net_init */
    if (net_slot_found >= 0) {
        net_vio_slot = net_slot_found;
        net_available = 1;
        net_vio = (volatile uint32_t *)
            ((uintptr_t)vio_vaddr + net_slot_found * VIRTIO_SLOT_SIZE);
        printf("[fs] Slot %d: virtio-net\n", net_slot_found);
    } else {
        net_available = 0;
    }

    /* Record GPU device for boot_display_init */
    if (gpu_slot_found >= 0) {
        gpu_vio_slot = gpu_slot_found;
        gpu_available = 1;
        gpu_vio = (volatile uint32_t *)
            ((uintptr_t)vio_vaddr + gpu_slot_found * VIRTIO_SLOT_SIZE);
        printf("[fs] Slot %d: virtio-gpu\n", gpu_slot_found);
    } else {
        gpu_available = 0;
    }
}
