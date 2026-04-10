#ifndef AIOS_GPU_H
#define AIOS_GPU_H
/*
 * AIOS gpu.h -- virtio-gpu protocol definitions
 *
 * Command/response structs for the virtio-gpu 2D device.
 * Based on virtio spec 1.1, device type 16.
 */
#include <stdint.h>

/* Device ID (also in virtio.h) */
#define VIRTIO_GPU_DEVICE_ID    16

/* -- 2D command types -- */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D        0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF            0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT               0x0103
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING   0x0104
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING   0x0105
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D       0x0106
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH            0x0107

/* -- Response types -- */
#define VIRTIO_GPU_RESP_OK_NODATA                0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO          0x1101
#define VIRTIO_GPU_RESP_ERR_UNSPEC               0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY        0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID   0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID  0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER    0x1205

/* -- Pixel formats -- */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM  1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM  2   /* 0x00RRGGBB on LE */
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM  3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM  4

/* Pixel macro: B8G8R8X8 on little-endian = 0x00RRGGBB */
#define GPU_PIXEL(r, g, b) \
    (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* -- Command/response header (24 bytes) -- */
struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};

/* -- Rectangle (16 bytes) -- */
struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

/* -- GET_DISPLAY_INFO response -- */
struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
};

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[16];
};

/* -- RESOURCE_CREATE_2D (40 bytes) -- */
struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

/* -- RESOURCE_ATTACH_BACKING (32 bytes + entries) -- */
struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
};

/* -- SET_SCANOUT (48 bytes) -- */
struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

/* -- TRANSFER_TO_HOST_2D (56 bytes) -- */
struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

/* -- RESOURCE_FLUSH (48 bytes) -- */
struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
};

/* -- DMA layout for GPU controlq (32KB) -- */
#define GPU_DMA_SIZE    0x8000
#define GPU_DMA_FRAMES  8
#define GPU_CMD_OFF     0x2000
#define GPU_RESP_OFF    0x3000


/* -- Raw image file format (for splash/display) -- */
#define AIOS_RAW_IMG_FORMAT  0   /* XRGB8888 */

struct aios_raw_img_hdr {
    uint32_t width;
    uint32_t height;
    uint32_t format;     /* 0 = XRGB8888 */
    uint32_t reserved;
};
/* Header is 16 bytes, followed by width*height*4 bytes of pixel data */

#endif /* AIOS_GPU_H */
