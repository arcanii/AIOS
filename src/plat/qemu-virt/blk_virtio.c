/*
 * blk_virtio.c -- virtio-blk sector I/O (QEMU virt platform)
 *
 * Platform HAL implementation for PLAT_QEMU_VIRT.
 * Provides plat_blk_read/write matching blk_hal.h interface.
 *
 * Extracted from blk_io.c (v0.4.88 PAL refactor).
 * The underlying virtio register protocol is unchanged.
 */
#include "aios/root_shared.h"
#include "virtio.h"
#include <stdio.h>
#include "arch.h"
#include "plat/blk_hal.h"

/* ---- Primary device (system disk) ---- */

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

/* ---- Log device (second virtio-blk) ---- */

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
