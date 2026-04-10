/*
 * AIOS – virtio MMIO definitions (legacy v1)
 *
 * Register offsets, status bits, and virtqueue structures
 * for virtio-mmio devices as used by QEMU virt platform.
 *
 * Copyright (c) 2025 AIOS Project
 * SPDX-License-Identifier: MIT
 */
#ifndef AIOS_VIRTIO_H
#define AIOS_VIRTIO_H

#include <stdint.h>

/* ── MMIO register offsets ─────────────────────────────── */
#define VIRTIO_MMIO_MAGIC            0x000
#define VIRTIO_MMIO_VERSION          0x004
#define VIRTIO_MMIO_DEVICE_ID        0x008
#define VIRTIO_MMIO_VENDOR_ID        0x00C
#define VIRTIO_MMIO_HOST_FEATURES    0x010
#define VIRTIO_MMIO_DRV_FEATURES     0x020
#define VIRTIO_MMIO_GUEST_PAGE_SIZE  0x028
#define VIRTIO_MMIO_QUEUE_SEL        0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX    0x034
#define VIRTIO_MMIO_QUEUE_NUM        0x038
#define VIRTIO_MMIO_QUEUE_ALIGN      0x03C
#define VIRTIO_MMIO_QUEUE_PFN        0x040
#define VIRTIO_MMIO_QUEUE_NOTIFY     0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK    0x064
#define VIRTIO_MMIO_STATUS           0x070
#define VIRTIO_MMIO_CONFIG           0x100

/* ── Device status bits ────────────────────────────────── */
#define VIRTIO_STATUS_ACK            1
#define VIRTIO_STATUS_DRIVER         2
#define VIRTIO_STATUS_DRIVER_OK      4
#define VIRTIO_STATUS_FEATURES_OK    8

/* ── virtio magic value ────────────────────────────────── */
#define VIRTIO_MAGIC                 0x74726976

/* ── Block device request types ────────────────────────── */
#define VIRTIO_BLK_T_IN             0   /* read  */
#define VIRTIO_BLK_T_OUT            1   /* write */
#define VIRTIO_BLK_DEVICE_ID        2   /* device ID for block */
#define VIRTIO_NET_DEVICE_ID        1   /* device ID for network */
#define VIRTIO_SLOT_SIZE            0x200 /* MMIO slot spacing */

/* ── Virtqueue ─────────────────────────────────────────── */
#define VIRTQ_SIZE                  16
#define VIRTQ_DESC_F_NEXT           1
#define VIRTQ_DESC_F_WRITE          2

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_SIZE];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VIRTQ_SIZE];
};

/* ── Block device request ──────────────────────────────── */
#define VIRTIO_BLK_SECTOR_SIZE      512

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
    uint8_t  data[VIRTIO_BLK_SECTOR_SIZE];
    uint8_t  status;
} __attribute__((packed));

#endif /* AIOS_VIRTIO_H */
