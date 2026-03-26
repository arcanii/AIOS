/*
 * AIOS Block Driver – virtio-blk (legacy v1)
 *
 * Provides sector-level read/write access to a virtio block device.
 * Serves both the orchestrator (CH_BLK) and fs_server (CH_FS_BLK).
 *
 * Copyright (c) 2025 AIOS Project
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <microkit.h>
#include "aios/channels.h"
#include "aios/ipc.h"
#include "virtio.h"

/* ── Memory regions (set by Microkit loader via setvar) ── */
uintptr_t virtio_blk_base_vaddr;
uintptr_t blk_data;
uintptr_t dma_base;

/* ── DMA physical address (must match hello.system) ────── */
#define DMA_PADDR       0xB0000000
#define DMA_SIZE        0x10000     /* 64 KiB */

/* ── DMA layout (legacy v1, VIRTQ_SIZE=16) ─────────────── *
 *   desc  : 0x0000  (16 × 16 = 256 bytes)
 *   avail : 0x0100  (immediately after desc)
 *   used  : 0x1000  (page-aligned)
 *   req   : 0x2000  (virtio_blk_req)
 * ──────────────────────────────────────────────────────── */
#define DMA_DESC_OFF    0x0000
#define DMA_AVAIL_OFF   0x0100
#define DMA_USED_OFF    0x1000
#define DMA_REQ_OFF     0x2000

/* ── Driver state ──────────────────────────────────────── */
static volatile struct virtq_desc      *descs    = 0;
static volatile struct virtq_avail     *avail    = 0;
static volatile struct virtq_used      *used     = 0;
static volatile struct virtio_blk_req  *blk_req  = 0;
static uint16_t last_used_idx = 0;
static uint64_t blk_capacity_sectors = 0;

/* ── MMIO helpers ──────────────────────────────────────── */
/* The virtio-blk device is at offset 0xe00 within the mapped page */
#define VIRTIO_BLK_OFFSET  0xe00

static inline uint32_t vio_read32(uint32_t off) {
    return *(volatile uint32_t *)(virtio_blk_base_vaddr + VIRTIO_BLK_OFFSET + off);
}

static inline void vio_write32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(virtio_blk_base_vaddr + VIRTIO_BLK_OFFSET + off) = val;
}

static inline void mb(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ── Block read/write ──────────────────────────────────── */
/*
 * blk_read_write – transfer one 512-byte sector via virtio-blk.
 *
 * Virtio descriptor flag semantics:
 *   VIRTQ_DESC_F_WRITE  = buffer is DEVICE-writable (device writes INTO it)
 *   (flag absent)       = buffer is DEVICE-readable  (device reads FROM it)
 *
 * For a READ  (device → host): data buffer needs VIRTQ_DESC_F_WRITE
 * For a WRITE (host → device): data buffer must NOT have VIRTQ_DESC_F_WRITE
 */
static int blk_read_write(uint64_t sector, int is_write, void *buf) {
    if (sector >= blk_capacity_sectors) {
        microkit_dbg_puts("BLK: sector out of range\n");
        return -1;
    }

    /* ── Build request header ──────────────────────────── */
    blk_req->type     = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    blk_req->reserved = 0;
    blk_req->sector   = sector;
    blk_req->status   = 0xFF;   /* poison — device will overwrite on success */

    /* For writes: copy caller's data into the DMA-visible buffer
       BEFORE we submit the descriptors to the device. */
    if (is_write) {
        uint8_t *src = (uint8_t *)buf;
        for (int i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++)
            blk_req->data[i] = src[i];
    }

    uint64_t req_pa = (uint64_t)DMA_PADDR + DMA_REQ_OFF;

    /* ── Descriptor 0: request header (16 bytes, device-readable) */
    descs[0].addr  = req_pa;
    descs[0].len   = 16;
    descs[0].flags = VIRTQ_DESC_F_NEXT;
    descs[0].next  = 1;

    /* ── Descriptor 1: 512-byte data buffer
     *    READ  → device writes into buffer  → VIRTQ_DESC_F_WRITE
     *    WRITE → device reads from buffer   → 0 (no WRITE flag)  */
    descs[1].addr  = req_pa + __builtin_offsetof(struct virtio_blk_req, data);
    descs[1].len   = VIRTIO_BLK_SECTOR_SIZE;
    descs[1].flags = VIRTQ_DESC_F_NEXT | (is_write ? 0 : VIRTQ_DESC_F_WRITE);
    descs[1].next  = 2;

    /* ── Descriptor 2: 1-byte status (always device-writable) */
    descs[2].addr  = req_pa + __builtin_offsetof(struct virtio_blk_req, status);
    descs[2].len   = 1;
    descs[2].flags = VIRTQ_DESC_F_WRITE;
    descs[2].next  = 0;

    /* ── Submit to available ring ──────────────────────── */
    avail->ring[avail->idx % VIRTQ_SIZE] = 0;  /* descriptor chain head = 0 */
    mb();
    avail->idx++;
    mb();

    /* Kick the device */
    vio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* ── Poll for completion ───────────────────────────── */
    int timeout = 0;
    while (used->idx == last_used_idx) {
        mb();
        if (++timeout > 10000000) {
            microkit_dbg_puts("BLK: TIMEOUT\n");
            return -1;
        }
    }
    last_used_idx = used->idx;

    /* ── Check device status ───────────────────────────── */
    if (blk_req->status != 0) {
        microkit_dbg_puts("BLK: device error, status=");
        microkit_dbg_put32(blk_req->status);
        microkit_dbg_puts("\n");
        return -1;
    }

    /* For reads: copy data from the DMA buffer to the caller. */
    if (!is_write) {
        uint8_t *dst = (uint8_t *)buf;
        for (int i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++)
            dst[i] = blk_req->data[i];
    }

    return 0;
}

/* ── Device initialization (legacy v1) ─────────────────── */
static int virtio_blk_init(void) {
    if (vio_read32(VIRTIO_MMIO_MAGIC) != VIRTIO_MAGIC) {
        microkit_dbg_puts("BLK: bad magic\n");
        return -1;
    }

    if (vio_read32(VIRTIO_MMIO_VERSION) != 1) {
        microkit_dbg_puts("BLK: bad version\n");
        return -1;
    }

    if (vio_read32(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_BLK_DEVICE_ID) {
        microkit_dbg_puts("BLK: not a block device\n");
        return -1;
    }

    /* Reset */
    vio_write32(VIRTIO_MMIO_STATUS, 0);
    mb();

    /* Acknowledge */
    vio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    mb();

    /* Driver */
    vio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    mb();

    /* Guest page size (required for legacy v1 before queue setup) */
    vio_write32(VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
    mb();

    /* Feature negotiation — accept no special features */
    vio_write32(VIRTIO_MMIO_DRV_FEATURES, 0);
    mb();

    /* Select queue 0 */
    vio_write32(VIRTIO_MMIO_QUEUE_SEL, 0);
    mb();

    uint32_t queue_max = vio_read32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max == 0) {
        microkit_dbg_puts("BLK: queue not available\n");
        return -1;
    }

    uint32_t qsz = VIRTQ_SIZE;
    if (qsz > queue_max) qsz = queue_max;
    vio_write32(VIRTIO_MMIO_QUEUE_NUM, qsz);
    mb();

    vio_write32(VIRTIO_MMIO_QUEUE_ALIGN, 4096);
    mb();

    /* Zero the entire DMA region */
    volatile uint8_t *p = (volatile uint8_t *)dma_base;
    for (uint32_t i = 0; i < DMA_SIZE; i++)
        p[i] = 0;
    mb();

    /* Set queue PFN (phys_addr / page_size) */
    vio_write32(VIRTIO_MMIO_QUEUE_PFN, DMA_PADDR / 4096);
    mb();

    /* Initialize virtqueue pointers */
    descs   = (volatile struct virtq_desc     *)(dma_base + DMA_DESC_OFF);
    avail   = (volatile struct virtq_avail    *)(dma_base + DMA_AVAIL_OFF);
    used    = (volatile struct virtq_used     *)(dma_base + DMA_USED_OFF);
    blk_req = (volatile struct virtio_blk_req *)(dma_base + DMA_REQ_OFF);
    last_used_idx = 0;

    /* Driver OK */
    vio_write32(VIRTIO_MMIO_STATUS,
                VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    mb();

    /* Read disk capacity (config space at offset 0x100) */
    uint32_t cap_lo = vio_read32(VIRTIO_MMIO_CONFIG + 0x00);
    uint32_t cap_hi = vio_read32(VIRTIO_MMIO_CONFIG + 0x04);
    blk_capacity_sectors = ((uint64_t)cap_hi << 32) | cap_lo;

    return 0;
}

/* ── Microkit entry points ─────────────────────────────── */
void init(void) {
    if (virtio_blk_init() == 0) {
        microkit_dbg_puts("BLK: virtio-blk ready\n");
    } else {
        microkit_dbg_puts("BLK: init FAILED\n");
    }
}


void notified(microkit_channel ch) {
    if (ch == CH_FS_BLK) {
        uint32_t cmd    = RD32(blk_data, BLK_CMD);
        uint32_t sector = RD32(blk_data, BLK_SECTOR);
        int is_write    = (cmd == BLK_CMD_WRITE) ? 1 : 0;

        int r = blk_read_write((uint64_t)sector, is_write,
                               (void *)(blk_data + BLK_DATA));
        WR32(blk_data, BLK_STATUS, (uint32_t)r);
        microkit_notify(CH_FS_BLK);
    }
}

