/*
 * blk_virtio.c -- virtio-blk platform driver (QEMU virt)
 *
 * PAL implementation for PLAT_QEMU_VIRT block devices.
 * Provides plat_blk_init/read/write matching blk_hal.h.
 *
 * Device discovery delegated to plat_virtio_probe.c (Step 7).
 * This file handles DMA allocation, drive probing (volume labels),
 * and sector I/O only.
 */
#include "aios/root_shared.h"
#include "aios/vka_audit.h"
#include "virtio.h"
#include "aios/ext2.h"
#include <sel4platsupport/device.h>
#include <stdio.h>
#include "arch.h"
#include "aios/hw_info.h"
#include "plat/blk_hal.h"
#include "plat_virtio_probe.h"

#define VIO_R(base, off)       ((base)[(off)/4])
#define VIO_W(base, off, val)  ((base)[(off)/4] = (val))

/* ---- Private state ---- */

static volatile uint32_t *blk_vio;
static uint8_t  *blk_dma;
static uint64_t  blk_dma_pa;

static volatile uint32_t *blk_vio_log;
static uint8_t  *blk_dma_log;
static uint64_t  blk_dma_pa_log;

static int log_slot_found = -1;

/* ============================================================
 * plat_blk_init -- probe drives, init system disk
 * ============================================================ */
int plat_blk_init(void) {
    int error;

    /* Shared virtio probe (maps MMIO, scans slots) */
    if (plat_virtio_probe() != 0) return -1;
    const plat_virtio_info_t *info = plat_virtio_get_info();
    if (!info || info->num_blk == 0) {
        printf("[fs] No block device (add -drive to QEMU)\n");
        return -1;
    }
    printf("[fs] Found %d block device(s)\n", info->num_blk);

    /* Allocate 16K contiguous DMA */
    vka_object_t dma_ut;
    vka_audit_untyped(VKA_SUB_BOOT, 14);
    error = vka_alloc_untyped(&vka, 14, &dma_ut);
    if (error) {
        printf("[fs] DMA untyped alloc failed: %d\n", error);
        return -1;
    }
    seL4_CPtr dma_caps[4];
    for (int i = 0; i < 4; i++) {
        seL4_CPtr slot;
        error = vka_cspace_alloc(&vka, &slot);
        if (error) { printf("[fs] DMA cslot alloc failed\n"); return -1; }
        error = seL4_Untyped_Retype(dma_ut.cptr,
            ARCH_PAGE_OBJECT, seL4_PageBits,
            seL4_CapInitThreadCNode, 0, 0, slot, 1);
        if (error) { printf("[fs] DMA retype %d failed: %d\n", i, error); return -1; }
        dma_caps[i] = slot;
    }
    void *dma_vaddr = vspace_map_pages(&vspace, dma_caps, NULL,
        seL4_AllRights, 4, seL4_PageBits, 0);
    if (!dma_vaddr) { printf("[fs] DMA map failed\n"); return -1; }

    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_caps[0]);
    if (ga.error) { printf("[fs] DMA GetAddress failed\n"); return -1; }
    uint64_t dma_pa = ga.paddr;

    /* Probe each block device: read superblock, check volume label */
    int blk_slot = -1;
    log_slot_found = -1;
    volatile uint32_t *vio = NULL;

    for (int d = 0; d < info->num_blk; d++) {
        vio = plat_virtio_slot_base(info->blk_slots[d]);

        uint8_t *dma = (uint8_t *)dma_vaddr;
        for (int z = 0; z < 16384; z++) dma[z] = 0;

        /* Legacy virtio init */
        VIO_W(vio, VIRTIO_MMIO_STATUS, 0);
        VIO_W(vio, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
        VIO_W(vio, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
        VIO_W(vio, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
        VIO_W(vio, VIRTIO_MMIO_DRV_FEATURES, 0);
        VIO_W(vio, VIRTIO_MMIO_QUEUE_SEL, 0);
        uint32_t qmax = VIO_R(vio, VIRTIO_MMIO_QUEUE_NUM_MAX);
        uint32_t qsz = qmax < 16 ? qmax : 16;
        VIO_W(vio, VIRTIO_MMIO_QUEUE_NUM, qsz);
        VIO_W(vio, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(dma_pa / 4096));
        VIO_W(vio, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER
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
        VIO_W(vio, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

        uint16_t last_used = 0;
        int probe_ok = 0;
        for (int t = 0; t < 10000000; t++) {
            arch_dmb();
            if (used->idx != last_used) { probe_ok = 1; break; }
        }
        arch_dmb();
        VIO_R(vio, VIRTIO_MMIO_INTERRUPT_STATUS);
        VIO_W(vio, VIRTIO_MMIO_INTERRUPT_ACK, 1);

        if (!probe_ok || req->status != 0) {
            printf("[fs] Slot %d: read failed\n", info->blk_slots[d]);
            VIO_W(vio, VIRTIO_MMIO_STATUS, 0);
            continue;
        }

        /* Check ext2 magic */
        uint16_t ext2_magic = req->data[0x38] | (req->data[0x39] << 8);
        if (ext2_magic != 0xEF53) {
            printf("[fs] Slot %d: not ext2 (0x%04x)\n",
                   info->blk_slots[d], ext2_magic);
            VIO_W(vio, VIRTIO_MMIO_STATUS, 0);
            continue;
        }

        /* Check volume label at superblock offset 0x78 */
        int is_log = (req->data[0x78] == 'a' && req->data[0x79] == 'i' &&
                      req->data[0x7a] == 'o' && req->data[0x7b] == 's' &&
                      req->data[0x7c] == '-' && req->data[0x7d] == 'l' &&
                      req->data[0x7e] == 'o' && req->data[0x7f] == 'g');
        if (is_log) {
            printf("[fs] Slot %d: log drive (aios-log)\n", info->blk_slots[d]);
            log_slot_found = info->blk_slots[d];
            VIO_W(vio, VIRTIO_MMIO_STATUS, 0);
            continue;
        }

        blk_slot = info->blk_slots[d];
        printf("[fs] Slot %d: system disk\n", blk_slot);
        break;
    }

    if (blk_slot < 0) {
        printf("[fs] No system disk found\n");
        return -1;
    }

    /* Save block device state */
    blk_vio = vio;
    blk_dma = (uint8_t *)dma_vaddr;
    blk_dma_pa = dma_pa;

    if (info->net_slot >= 0) {
        printf("[fs] Slot %d: virtio-net\n", info->net_slot);
    }

    return 0;
}

/* ============================================================
 * plat_blk_init_log -- init second virtio-blk for log drive
 * ============================================================ */
int plat_blk_init_log(void) {
    if (log_slot_found < 0) return -1;
    int error;

    volatile uint32_t *vio = plat_virtio_slot_base(log_slot_found);
    if (!vio) return -1;

    /* Allocate 16K contiguous DMA */
    vka_object_t dma_ut;
    vka_audit_untyped(VKA_SUB_BOOT, 14);
    error = vka_alloc_untyped(&vka, 14, &dma_ut);
    if (error) {
        printf("[boot] Log DMA alloc failed: %d\n", error);
        return -1;
    }
    seL4_CPtr dma_caps[4];
    for (int i = 0; i < 4; i++) {
        seL4_CPtr slot;
        error = vka_cspace_alloc(&vka, &slot);
        if (error) { printf("[boot] Log cslot failed\n"); return -1; }
        error = seL4_Untyped_Retype(dma_ut.cptr,
            ARCH_PAGE_OBJECT, seL4_PageBits,
            seL4_CapInitThreadCNode, 0, 0, slot, 1);
        if (error) { printf("[boot] Log retype %d failed\n", i); return -1; }
        dma_caps[i] = slot;
    }
    void *dma_vaddr = vspace_map_pages(&vspace, dma_caps, NULL,
        seL4_AllRights, 4, seL4_PageBits, 0);
    if (!dma_vaddr) { printf("[boot] Log DMA map failed\n"); return -1; }

    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_caps[0]);
    if (ga.error) { printf("[boot] Log GetAddress failed\n"); return -1; }
    uint64_t dma_pa = ga.paddr;

    uint8_t *dma = (uint8_t *)dma_vaddr;
    for (int i = 0; i < 16384; i++) dma[i] = 0;

    /* Legacy virtio init */
    VIO_W(vio, VIRTIO_MMIO_STATUS, 0);
    VIO_W(vio, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    VIO_W(vio, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    VIO_W(vio, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
    VIO_W(vio, VIRTIO_MMIO_DRV_FEATURES, 0);
    VIO_W(vio, VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = VIO_R(vio, VIRTIO_MMIO_QUEUE_NUM_MAX);
    uint32_t qsz = qmax < 16 ? qmax : 16;
    VIO_W(vio, VIRTIO_MMIO_QUEUE_NUM, qsz);
    VIO_W(vio, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(dma_pa / 4096));
    VIO_W(vio, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER
                                 | VIRTIO_STATUS_DRIVER_OK);

    blk_vio_log = vio;
    blk_dma_log = dma;
    blk_dma_pa_log = dma_pa;

    return 0;
}

/* ============================================================
 * Sector I/O -- primary device
 * ============================================================ */
int plat_blk_read(uint64_t sector, void *buf) {
    struct virtq_desc  *desc  = (struct virtq_desc *)(blk_dma);
    struct virtq_avail *avail = (struct virtq_avail *)(blk_dma + 0x100);
    struct virtq_used  *used  = (struct virtq_used  *)(blk_dma + 0x1000);
    struct virtio_blk_req *req = (struct virtio_blk_req *)(blk_dma + 0x2000);
    uint64_t req_pa = blk_dma_pa + 0x2000;

    req->type = VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = sector;
    req->status = 0xFF;

    desc[0].addr = req_pa; desc[0].len = 16;
    desc[0].flags = VIRTQ_DESC_F_NEXT; desc[0].next = 1;
    desc[1].addr = req_pa + 16; desc[1].len = 512;
    desc[1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT; desc[1].next = 2;
    desc[2].addr = req_pa + 16 + 512; desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE; desc[2].next = 0;

    arch_dmb();
    avail->ring[avail->idx % 16] = 0;
    arch_dmb();
    avail->idx += 1;
    arch_dmb();
    blk_vio[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

    uint16_t last = used->idx;
    for (int t = 0; t < 10000000; t++) {
        arch_dmb();
        if (used->idx != last) break;
    }
    arch_dmb();
    blk_vio[VIRTIO_MMIO_INTERRUPT_ACK / 4] = blk_vio[VIRTIO_MMIO_INTERRUPT_STATUS / 4];

    if (used->idx == last || req->status != 0) return -1;
    uint8_t *src = req->data;
    uint8_t *dst = (uint8_t *)buf;
    for (int i = 0; i < 512; i++) dst[i] = src[i];
    return 0;
}

int plat_blk_write(uint64_t sector, const void *buf) {
    struct virtq_desc  *desc  = (struct virtq_desc *)(blk_dma);
    struct virtq_avail *avail = (struct virtq_avail *)(blk_dma + 0x100);
    struct virtq_used  *used  = (struct virtq_used  *)(blk_dma + 0x1000);
    struct virtio_blk_req *req = (struct virtio_blk_req *)(blk_dma + 0x2000);
    uint64_t req_pa = blk_dma_pa + 0x2000;

    req->type = VIRTIO_BLK_T_OUT;
    req->reserved = 0;
    req->sector = sector;
    req->status = 0xFF;

    const uint8_t *src = (const uint8_t *)buf;
    for (int i = 0; i < 512; i++) req->data[i] = src[i];

    desc[0].addr = req_pa; desc[0].len = 16;
    desc[0].flags = VIRTQ_DESC_F_NEXT; desc[0].next = 1;
    desc[1].addr = req_pa + 16; desc[1].len = 512;
    desc[1].flags = VIRTQ_DESC_F_NEXT; desc[1].next = 2;
    desc[2].addr = req_pa + 16 + 512; desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE; desc[2].next = 0;

    arch_dmb();
    avail->ring[avail->idx % 16] = 0;
    arch_dmb();
    avail->idx += 1;
    arch_dmb();
    blk_vio[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

    uint16_t last = used->idx;
    for (int t = 0; t < 10000000; t++) {
        arch_dmb();
        if (used->idx != last) break;
    }
    arch_dmb();
    blk_vio[VIRTIO_MMIO_INTERRUPT_ACK / 4] = blk_vio[VIRTIO_MMIO_INTERRUPT_STATUS / 4];

    if (used->idx == last || req->status != 0) return -1;
    return 0;
}

/* ============================================================
 * Sector I/O -- log device
 * ============================================================ */
int plat_blk_read_log(uint64_t sector, void *buf) {
    struct virtq_desc  *desc  = (struct virtq_desc *)(blk_dma_log);
    struct virtq_avail *avail = (struct virtq_avail *)(blk_dma_log + 0x100);
    struct virtq_used  *used  = (struct virtq_used  *)(blk_dma_log + 0x1000);
    struct virtio_blk_req *req = (struct virtio_blk_req *)(blk_dma_log + 0x2000);
    uint64_t req_pa = blk_dma_pa_log + 0x2000;

    req->type = VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = sector;
    req->status = 0xFF;

    desc[0].addr = req_pa; desc[0].len = 16;
    desc[0].flags = VIRTQ_DESC_F_NEXT; desc[0].next = 1;
    desc[1].addr = req_pa + 16; desc[1].len = 512;
    desc[1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT; desc[1].next = 2;
    desc[2].addr = req_pa + 16 + 512; desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE; desc[2].next = 0;

    arch_dmb();
    avail->ring[avail->idx % 16] = 0;
    arch_dmb();
    avail->idx += 1;
    arch_dmb();
    blk_vio_log[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

    uint16_t last = used->idx;
    for (int t = 0; t < 10000000; t++) {
        arch_dmb();
        if (used->idx != last) break;
    }
    arch_dmb();
    blk_vio_log[VIRTIO_MMIO_INTERRUPT_ACK / 4] =
        blk_vio_log[VIRTIO_MMIO_INTERRUPT_STATUS / 4];

    if (used->idx == last || req->status != 0) return -1;
    uint8_t *src = req->data;
    uint8_t *dst = (uint8_t *)buf;
    for (int i = 0; i < 512; i++) dst[i] = src[i];
    return 0;
}

int plat_blk_write_log(uint64_t sector, const void *buf) {
    struct virtq_desc  *desc  = (struct virtq_desc *)(blk_dma_log);
    struct virtq_avail *avail = (struct virtq_avail *)(blk_dma_log + 0x100);
    struct virtq_used  *used  = (struct virtq_used  *)(blk_dma_log + 0x1000);
    struct virtio_blk_req *req = (struct virtio_blk_req *)(blk_dma_log + 0x2000);
    uint64_t req_pa = blk_dma_pa_log + 0x2000;

    req->type = VIRTIO_BLK_T_OUT;
    req->reserved = 0;
    req->sector = sector;
    req->status = 0xFF;

    const uint8_t *src = (const uint8_t *)buf;
    for (int i = 0; i < 512; i++) req->data[i] = src[i];

    desc[0].addr = req_pa; desc[0].len = 16;
    desc[0].flags = VIRTQ_DESC_F_NEXT; desc[0].next = 1;
    desc[1].addr = req_pa + 16; desc[1].len = 512;
    desc[1].flags = VIRTQ_DESC_F_NEXT; desc[1].next = 2;
    desc[2].addr = req_pa + 16 + 512; desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE; desc[2].next = 0;

    arch_dmb();
    avail->ring[avail->idx % 16] = 0;
    arch_dmb();
    avail->idx += 1;
    arch_dmb();
    blk_vio_log[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

    uint16_t last = used->idx;
    for (int t = 0; t < 10000000; t++) {
        arch_dmb();
        if (used->idx != last) break;
    }
    arch_dmb();
    blk_vio_log[VIRTIO_MMIO_INTERRUPT_ACK / 4] =
        blk_vio_log[VIRTIO_MMIO_INTERRUPT_STATUS / 4];

    if (used->idx == last || req->status != 0) return -1;
    return 0;
}
