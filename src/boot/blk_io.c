/*
 * blk_io.c -- virtio-blk sector read/write
 *
 * Extracted from aios_root.c (v0.4.53 modularization).
 * Used as callbacks by ext2_init() / ext2_init_write().
 */
#include "aios/root_shared.h"
#include "virtio.h"
#include <stdio.h>

int blk_read_sector(uint64_t sector, void *buf) {
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

    __asm__ volatile("dmb sy" ::: "memory");
    avail->ring[avail->idx % 16] = 0;
    __asm__ volatile("dmb sy" ::: "memory");
    avail->idx += 1;
    __asm__ volatile("dmb sy" ::: "memory");
    blk_vio[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

    uint16_t last = used->idx;
    for (int t = 0; t < 10000000; t++) {
        __asm__ volatile("dmb sy" ::: "memory");
        if (used->idx != last) break;
    }
    __asm__ volatile("dmb sy" ::: "memory");
    blk_vio[VIRTIO_MMIO_INTERRUPT_ACK / 4] = blk_vio[VIRTIO_MMIO_INTERRUPT_STATUS / 4];

    if (used->idx == last || req->status != 0) return -1;
    uint8_t *src = req->data;
    uint8_t *dst = (uint8_t *)buf;
    for (int i = 0; i < 512; i++) dst[i] = src[i];
    return 0;
}

int blk_write_sector(uint64_t sector, const void *buf) {
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

    __asm__ volatile("dmb sy" ::: "memory");
    avail->ring[avail->idx % 16] = 0;
    __asm__ volatile("dmb sy" ::: "memory");
    avail->idx += 1;
    __asm__ volatile("dmb sy" ::: "memory");
    blk_vio[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

    uint16_t last = used->idx;
    for (int t = 0; t < 10000000; t++) {
        __asm__ volatile("dmb sy" ::: "memory");
        if (used->idx != last) break;
    }
    __asm__ volatile("dmb sy" ::: "memory");
    blk_vio[VIRTIO_MMIO_INTERRUPT_ACK / 4] = blk_vio[VIRTIO_MMIO_INTERRUPT_STATUS / 4];

    if (used->idx == last || req->status != 0) return -1;
    return 0;
}

/* ---- Log drive I/O (second virtio-blk device) ---- */

int blk_read_sector_log(uint64_t sector, void *buf) {
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

    __asm__ volatile("dmb sy" ::: "memory");
    avail->ring[avail->idx % 16] = 0;
    __asm__ volatile("dmb sy" ::: "memory");
    avail->idx += 1;
    __asm__ volatile("dmb sy" ::: "memory");
    blk_vio_log[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

    uint16_t last = used->idx;
    for (int t = 0; t < 10000000; t++) {
        __asm__ volatile("dmb sy" ::: "memory");
        if (used->idx != last) break;
    }
    __asm__ volatile("dmb sy" ::: "memory");
    blk_vio_log[VIRTIO_MMIO_INTERRUPT_ACK / 4] =
        blk_vio_log[VIRTIO_MMIO_INTERRUPT_STATUS / 4];

    if (used->idx == last || req->status != 0) return -1;
    uint8_t *src = req->data;
    uint8_t *dst = (uint8_t *)buf;
    for (int i = 0; i < 512; i++) dst[i] = src[i];
    return 0;
}

int blk_write_sector_log(uint64_t sector, const void *buf) {
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

    __asm__ volatile("dmb sy" ::: "memory");
    avail->ring[avail->idx % 16] = 0;
    __asm__ volatile("dmb sy" ::: "memory");
    avail->idx += 1;
    __asm__ volatile("dmb sy" ::: "memory");
    blk_vio_log[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

    uint16_t last = used->idx;
    for (int t = 0; t < 10000000; t++) {
        __asm__ volatile("dmb sy" ::: "memory");
        if (used->idx != last) break;
    }
    __asm__ volatile("dmb sy" ::: "memory");
    blk_vio_log[VIRTIO_MMIO_INTERRUPT_ACK / 4] =
        blk_vio_log[VIRTIO_MMIO_INTERRUPT_STATUS / 4];

    if (used->idx == last || req->status != 0) return -1;
    return 0;
}
