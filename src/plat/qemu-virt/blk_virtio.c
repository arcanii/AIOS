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
#include "aios/mono_wait.h"
#include "arch.h"
#include "aios/hw_info.h"
#include "plat/blk_hal.h"
#include "plat_virtio_probe.h"
#define LOG_MODULE "blk"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "aios/aios_log.h"

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

/* v0.4.222 fatswap: partition layout of the system disk. Historically the
 * QEMU disk is a BARE ext2 image (part_offset 0, no boot partition); a
 * composite MBR image (mksdcard.py layout: FAT32 boot + ext2) now also
 * boots, so fatswap can be QEMU-tested before touching real cards. */
static uint64_t part_offset;
static uint64_t boot_part_start;
static uint64_t boot_part_sectors;

/* Probe-time single-sector read on a freshly initialized queue. Data lands
 * in req->data (dma + 0x2000 + 16). Captures used->idx BEFORE the notify
 * (v0.4.147 lesson: capturing after races a fast completion). */
static int probe_read_sector(volatile uint32_t *vio, uint8_t *dma,
                             uint64_t dma_pa, uint32_t qsz, uint64_t sector) {
    struct virtq_desc  *desc  = (struct virtq_desc *)(dma);
    struct virtq_avail *avail = (struct virtq_avail *)(dma + 0x100);
    struct virtq_used  *used  = (struct virtq_used  *)(dma + 0x1000);
    struct virtio_blk_req *req = (struct virtio_blk_req *)(dma + 0x2000);
    uint64_t req_pa = dma_pa + 0x2000;

    req->type = VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = sector;
    req->status = 0xFF;

    desc[0].addr  = req_pa;      desc[0].len = 16;
    desc[0].flags = VIRTQ_DESC_F_NEXT; desc[0].next = 1;
    desc[1].addr  = req_pa + 16; desc[1].len = 512;
    desc[1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT; desc[1].next = 2;
    desc[2].addr  = req_pa + 16 + 512; desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE; desc[2].next = 0;

    uint16_t last_used = used->idx;
    arch_dmb();
    avail->ring[avail->idx % qsz] = 0;
    arch_dmb();
    avail->idx += 1;
    arch_dmb();
    VIO_W(vio, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    int ok = 0;
    for (uint64_t dl = mono_deadline_ms(2000); mono_before(dl); ) {
        arch_dmb();
        if (used->idx != last_used) { ok = 1; break; }
    }
    arch_dmb();
    VIO_R(vio, VIRTIO_MMIO_INTERRUPT_STATUS);
    VIO_W(vio, VIRTIO_MMIO_INTERRUPT_ACK, 1);
    return (ok && req->status == 0) ? 0 : -1;
}

/* ============================================================
 * plat_blk_init -- probe drives, init system disk
 * ============================================================ */
int plat_blk_init(void) {
    int error;

    /* Shared virtio probe (maps MMIO, scans slots) */
    if (plat_virtio_probe() != 0) return -1;
    const plat_virtio_info_t *info = plat_virtio_get_info();
    if (!info || info->num_blk == 0) {
        AIOS_LOG_ERROR("No block device (add -drive to QEMU)");
        return -1;
    }
    AIOS_LOG_INFO_V("Found block device count=", (unsigned long)info->num_blk);
    printf("[fs] Found %d block device(s)\n", info->num_blk);

    /* Allocate 16K contiguous DMA */
    vka_object_t dma_ut;
    vka_audit_untyped(VKA_SUB_BOOT, 14);
    error = vka_alloc_untyped(&vka, 14, &dma_ut);
    if (error) {
        AIOS_LOG_ERROR_V("DMA untyped alloc failed err=", error);
        return -1;
    }
    seL4_CPtr dma_caps[4];
    for (int i = 0; i < 4; i++) {
        seL4_CPtr slot;
        error = vka_cspace_alloc(&vka, &slot);
        if (error) { AIOS_LOG_ERROR("DMA cslot alloc failed"); return -1; }
        error = seL4_Untyped_Retype(dma_ut.cptr,
            ARCH_PAGE_OBJECT, seL4_PageBits,
            seL4_CapInitThreadCNode, 0, 0, slot, 1);
        if (error) { AIOS_LOG_ERROR_V("DMA retype failed err=", error); return -1; }
        dma_caps[i] = slot;
    }
    void *dma_vaddr = vspace_map_pages(&vspace, dma_caps, NULL,
        seL4_AllRights, 4, seL4_PageBits, 0);
    if (!dma_vaddr) { AIOS_LOG_ERROR("DMA map failed"); return -1; }

    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_caps[0]);
    if (ga.error) { AIOS_LOG_ERROR("DMA GetAddress failed"); return -1; }
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

        struct virtio_blk_req *req = (struct virtio_blk_req *)(dma + 0x2000);

        /* v0.4.222 fatswap: composite (MBR-partitioned) images boot too.
         * Look for an MBR first: type 0x83 -> ext2 partition offset, type
         * 0x0b/0x0c -> FAT boot partition. A bare ext2 image has no 0x55AA
         * signature in its (zeroed) boot block, so the historic layout
         * falls through with offset 0. If the MBR pointed somewhere that
         * lacks an ext2 superblock, fall back to bare as well. */
        uint64_t dev_part = 0, dev_boot = 0, dev_boot_cnt = 0;
        if (probe_read_sector(vio, dma, dma_pa, qsz, 0) == 0 &&
            req->data[510] == 0x55 && req->data[511] == 0xAA) {
            for (int p = 0; p < 4; p++) {
                const uint8_t *e = &req->data[0x1BE + p * 16];
                uint8_t type = e[4];
                uint32_t lba = (uint32_t)e[8]  | ((uint32_t)e[9] << 8) |
                               ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
                uint32_t cnt = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                               ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
                if (type == 0x83 && dev_part == 0)
                    dev_part = lba;
                if ((type == 0x0B || type == 0x0C) && dev_boot == 0) {
                    dev_boot = lba;
                    dev_boot_cnt = cnt;
                }
            }
        }

        /* Read the ext2 superblock (sector 2 of the partition or disk) */
        int probe_ok = (probe_read_sector(vio, dma, dma_pa, qsz,
                                          dev_part + 2) == 0);
        uint16_t ext2_magic = probe_ok
            ? (uint16_t)(req->data[0x38] | (req->data[0x39] << 8)) : 0;
        if (probe_ok && ext2_magic != 0xEF53 && dev_part != 0) {
            /* MBR parse found a partition but no fs there -- try bare. */
            dev_part = 0; dev_boot = 0; dev_boot_cnt = 0;
            probe_ok = (probe_read_sector(vio, dma, dma_pa, qsz, 2) == 0);
            ext2_magic = probe_ok
                ? (uint16_t)(req->data[0x38] | (req->data[0x39] << 8)) : 0;
        }

        if (!probe_ok) {
            AIOS_LOG_ERROR_V("Slot read failed slot=", (unsigned long)info->blk_slots[d]);
            VIO_W(vio, VIRTIO_MMIO_STATUS, 0);
            continue;
        }
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
        part_offset = dev_part;
        boot_part_start = dev_boot;
        boot_part_sectors = dev_boot_cnt;
        if (part_offset)
            printf("[fs] Slot %d: system disk (MBR: ext2 at %u, boot at %u)\n",
                   blk_slot, (unsigned)part_offset, (unsigned)boot_part_start);
        else
            printf("[fs] Slot %d: system disk\n", blk_slot);
        break;
    }

    if (blk_slot < 0) {
        AIOS_LOG_ERROR("No system disk found");
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
        AIOS_LOG_ERROR_V("Log DMA alloc failed err=", error);
        return -1;
    }
    seL4_CPtr dma_caps[4];
    for (int i = 0; i < 4; i++) {
        seL4_CPtr slot;
        error = vka_cspace_alloc(&vka, &slot);
        if (error) { AIOS_LOG_ERROR("Log cslot failed"); return -1; }
        error = seL4_Untyped_Retype(dma_ut.cptr,
            ARCH_PAGE_OBJECT, seL4_PageBits,
            seL4_CapInitThreadCNode, 0, 0, slot, 1);
        if (error) { AIOS_LOG_ERROR_V("Log retype failed idx=", (unsigned long)i); return -1; }
        dma_caps[i] = slot;
    }
    void *dma_vaddr = vspace_map_pages(&vspace, dma_caps, NULL,
        seL4_AllRights, 4, seL4_PageBits, 0);
    if (!dma_vaddr) { AIOS_LOG_ERROR("Log DMA map failed"); return -1; }

    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_caps[0]);
    if (ga.error) { AIOS_LOG_ERROR("Log GetAddress failed"); return -1; }
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

/* v0.4.147: hardened virtio-blk completion wait. The original single
 * 10M-iteration poll could expire before QEMUs virtio I/O thread ran under
 * heavy host contention, returning -1 and making an UNCACHED disk read fail
 * (exec ELF reads -> spurious EPERM, the COW Step 3 red herring). We now poll
 * in batches and RE-NOTIFY the device between batches. The request stays in the
 * avail ring -- we never re-issue it -- so there is no double-completion risk;
 * re-notify is idempotent (it just asks the device to re-check the ring,
 * covering a missed kick or a starved I/O thread). last is captured BEFORE the
 * notify (the original captured it after, a latent race if the device was
 * unusually fast). Returns 1 if the used ring advanced, 0 if it never did. */
#define BLK_POLL_ITERS   4000000
#define BLK_POLL_RETRIES 32
extern volatile uint32_t blk_poll_renotifies;   /* defined in blk_cache.c (shared) */
static int blk_complete(volatile uint32_t *vio, volatile struct virtq_used *used) {
    uint16_t last = used->idx;
    arch_dmb();
    vio[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;
    int done = 0;
    for (int r = 0; r < BLK_POLL_RETRIES; r++) {
        for (int t = 0; t < BLK_POLL_ITERS; t++) {
            arch_dmb();
            if (used->idx != last) { done = 1; break; }
        }
        if (done) break;
        blk_poll_renotifies++;
        arch_dmb();
        vio[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;   /* re-nudge; idempotent */
    }
    arch_dmb();
    vio[VIRTIO_MMIO_INTERRUPT_ACK / 4] = vio[VIRTIO_MMIO_INTERRUPT_STATUS / 4];
    return done;
}

/* One sector in or out at an ABSOLUTE disk LBA. The partition-relative
 * HAL entry points add part_offset; the _abs entry points (fatswap) do
 * not. Single shared body so the two spaces cannot drift. */
static int blk_xfer_abs(int is_write, uint64_t sector, void *buf) {
    if (!blk_vio) return -1;
    struct virtq_desc  *desc  = (struct virtq_desc *)(blk_dma);
    struct virtq_avail *avail = (struct virtq_avail *)(blk_dma + 0x100);
    struct virtq_used  *used  = (struct virtq_used  *)(blk_dma + 0x1000);
    struct virtio_blk_req *req = (struct virtio_blk_req *)(blk_dma + 0x2000);
    uint64_t req_pa = blk_dma_pa + 0x2000;

    req->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = sector;
    req->status = 0xFF;

    if (is_write) {
        const uint8_t *src = (const uint8_t *)buf;
        for (int i = 0; i < 512; i++) req->data[i] = src[i];
    }

    desc[0].addr = req_pa; desc[0].len = 16;
    desc[0].flags = VIRTQ_DESC_F_NEXT; desc[0].next = 1;
    desc[1].addr = req_pa + 16; desc[1].len = 512;
    desc[1].flags = (is_write ? 0 : VIRTQ_DESC_F_WRITE) | VIRTQ_DESC_F_NEXT;
    desc[1].next = 2;
    desc[2].addr = req_pa + 16 + 512; desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE; desc[2].next = 0;

    arch_dmb();
    avail->ring[avail->idx % 16] = 0;
    arch_dmb();
    avail->idx += 1;
    arch_dmb();
    if (!blk_complete(blk_vio, used) || req->status != 0) return -1;
    if (!is_write) {
        uint8_t *src = req->data;
        uint8_t *dst = (uint8_t *)buf;
        for (int i = 0; i < 512; i++) dst[i] = src[i];
    }
    return 0;
}

int plat_blk_read(uint64_t sector, void *buf) {
    return blk_xfer_abs(0, part_offset + sector, buf);
}

int plat_blk_write(uint64_t sector, const void *buf) {
    return blk_xfer_abs(1, part_offset + sector, (void *)buf);
}

/* v0.4.172: multi-sector write. QEMU virtio writes are already fast, so we
 * just loop single-block writes -- this still exercises the block cache's
 * write-back line-flush path (it calls the multi backend with count=8) so
 * the write-back logic is validated on QEMU before flashing. */
int plat_blk_write_multi(uint64_t sector, const void *buf, int count) {
    const uint8_t *p = (const uint8_t *)buf;
    for (int i = 0; i < count; i++) {
        if (plat_blk_write(sector + i, p + i * 512) != 0) return -1;
    }
    return 0;
}

/* Absolute-LBA HAL (v0.4.222 fatswap) -- see blk_hal.h. */
int plat_blk_read_abs(uint64_t sector, void *buf) {
    return blk_xfer_abs(0, sector, buf);
}

int plat_blk_write_abs(uint64_t sector, const void *buf) {
    return blk_xfer_abs(1, sector, (void *)buf);
}

int plat_blk_write_multi_abs(uint64_t sector, const void *buf, int count) {
    const uint8_t *p = (const uint8_t *)buf;
    for (int i = 0; i < count; i++) {
        if (blk_xfer_abs(1, sector + i, (void *)(p + i * 512)) != 0) return -1;
    }
    return 0;
}

uint64_t plat_blk_boot_part_start(void)   { return boot_part_start; }
uint64_t plat_blk_boot_part_sectors(void) { return boot_part_sectors; }

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
    if (!blk_complete(blk_vio_log, used) || req->status != 0) return -1;
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
    if (!blk_complete(blk_vio_log, used) || req->status != 0) return -1;
    return 0;
}
