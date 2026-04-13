/*
 * display_ramfb.c -- QEMU ramfb framebuffer via fw_cfg (QEMU virt)
 *
 * PAL implementation for PLAT_QEMU_VIRT display.
 * Provides plat_display_init/get_fb matching display_hal.h.
 *
 * Extracted from boot_display_init.c during v0.4.89 PAL refactor.
 */
#include "aios/root_shared.h"
#include "aios/gpu.h"
#include "aios/vka_audit.h"
#include <sel4platsupport/device.h>
#include <stdio.h>
#include "arch.h"
#include "aios/hw_info.h"
#include "plat/display_hal.h"

#define FWCFG_PADDR     hw_info.fwcfg_paddr
#define FWCFG_FILE_DIR  0x0019
#define DRM_FORMAT_XRGB8888  0x34325258
#define FW_CFG_DMA_SELECT  0x08
#define FW_CFG_DMA_WRITE   0x10
#define FW_CFG_DMA_ERROR   0x01

static inline uint32_t bswap32(uint32_t x) {
    return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) |
           ((x << 8) & 0xFF0000) | ((x << 24) & 0xFF000000);
}
static inline uint64_t bswap64(uint64_t x) {
    uint32_t hi = (uint32_t)(x >> 32);
    uint32_t lo = (uint32_t)x;
    return ((uint64_t)bswap32(lo) << 32) | bswap32(hi);
}
static inline uint16_t bswap16(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}

struct ramfb_cfg {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

struct fw_cfg_dma_access {
    uint32_t control;
    uint32_t length;
    uint64_t address;
};

#define GPU_FB_MAX_PAGES 1024
static seL4_CPtr fb_caps[GPU_FB_MAX_PAGES];

/* ============================================================
 * plat_display_init -- configure ramfb via QEMU fw_cfg
 * ============================================================ */
int plat_display_init(uint32_t width, uint32_t height) {
    int error;

    if (!hw_info.has_fwcfg) {
        printf("[gpu] No fw_cfg (not QEMU?)\n");
        return -1;
    }

    /* Map fw_cfg MMIO */
    vka_object_t fw_frame;
    error = sel4platsupport_alloc_frame_at(&vka, FWCFG_PADDR,
                                            seL4_PageBits, &fw_frame);
    if (error) { printf("[gpu] fw_cfg alloc: %d\n", error); return -1; }
    void *fw_vaddr = vspace_map_pages(&vspace, &fw_frame.cptr, NULL,
        seL4_AllRights, 1, seL4_PageBits, 0);
    if (!fw_vaddr) { printf("[gpu] fw_cfg map fail\n"); return -1; }

    volatile uint8_t  *fw_data = (volatile uint8_t *)fw_vaddr;
    volatile uint16_t *fw_sel  = (volatile uint16_t *)((uintptr_t)fw_vaddr + 8);

    /* Enumerate fw_cfg files */
    *fw_sel = bswap16(FWCFG_FILE_DIR);
    arch_dsb();

    uint32_t fcount = 0;
    for (int i = 0; i < 4; i++)
        fcount = (fcount << 8) | fw_data[0];
    if (fcount > 256) fcount = 256;

    uint16_t ramfb_key = 0;
    int ramfb_found = 0;

    for (uint32_t e = 0; e < fcount; e++) {
        uint32_t fsize = 0;
        for (int i = 0; i < 4; i++) fsize = (fsize << 8) | fw_data[0];
        uint16_t fsel = 0;
        for (int i = 0; i < 2; i++) fsel = (fsel << 8) | fw_data[0];
        (void)fw_data[0]; (void)fw_data[0];
        char name[56];
        for (int i = 0; i < 56; i++) name[i] = (char)fw_data[0];

        if (name[0]=='e' && name[1]=='t' && name[2]=='c' &&
            name[3]=='/' && name[4]=='r' && name[5]=='a' &&
            name[6]=='m' && name[7]=='f' && name[8]=='b' &&
            name[9]=='\0') {
            ramfb_key = fsel;
            ramfb_found = 1;
        }
    }

    if (!ramfb_found) {
        printf("[gpu] etc/ramfb not found (add -device ramfb)\n");
        return -1;
    }

    /* Allocate DMA page for fw_cfg access */
    vka_object_t dma_frame_obj;
    vka_audit_untyped(VKA_SUB_GPU, 12);
    error = vka_alloc_frame(&vka, seL4_PageBits, &dma_frame_obj);
    if (error) { printf("[gpu] DMA alloc fail\n"); return -1; }
    void *dma_vaddr = vspace_map_pages(&vspace, &dma_frame_obj.cptr, NULL,
        seL4_AllRights, 1, seL4_PageBits, 0);
    if (!dma_vaddr) { printf("[gpu] DMA map fail\n"); return -1; }

    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_frame_obj.cptr);
    if (ga.error) { printf("[gpu] DMA addr fail\n"); return -1; }
    uint64_t dma_pa = ga.paddr;
    uint8_t *dma = (uint8_t *)dma_vaddr;
    for (int i = 0; i < 4096; i++) dma[i] = 0;

    /* Allocate framebuffer pages */
    gpu_width  = width;
    gpu_height = height;
    uint32_t fb_size  = width * height * 4;
    uint32_t fb_pages = (fb_size + 4095) / 4096;

    int fb_bits = 12;
    while ((1U << fb_bits) < (fb_pages * 4096)) fb_bits++;
    uint32_t fb_ut_pages = (1U << fb_bits) / 4096;
    if (fb_ut_pages > GPU_FB_MAX_PAGES) fb_ut_pages = GPU_FB_MAX_PAGES;

    vka_object_t fb_ut;
    vka_audit_untyped(VKA_SUB_GPU, fb_bits);
    error = vka_alloc_untyped(&vka, fb_bits, &fb_ut);
    if (error) { printf("[gpu] FB alloc fail: %d\n", error); return -1; }

    for (uint32_t i = 0; i < fb_ut_pages; i++) {
        seL4_CPtr slot;
        error = vka_cspace_alloc(&vka, &slot);
        if (error) { printf("[gpu] FB cslot %u\n", i); return -1; }
        error = seL4_Untyped_Retype(fb_ut.cptr,
            ARCH_PAGE_OBJECT, seL4_PageBits,
            seL4_CapInitThreadCNode, 0, 0, slot, 1);
        if (error) { printf("[gpu] FB retype %u: %d\n", i, error); return -1; }
        fb_caps[i] = slot;
    }

    void *fb_vaddr = vspace_map_pages(&vspace, fb_caps, NULL,
        seL4_AllRights, fb_ut_pages, seL4_PageBits, 0);
    if (!fb_vaddr) { printf("[gpu] FB map fail\n"); return -1; }

    ga = seL4_ARM_Page_GetAddress(fb_caps[0]);
    if (ga.error) { printf("[gpu] FB addr fail\n"); return -1; }

    gpu_fb    = (uint32_t *)fb_vaddr;
    gpu_fb_pa = ga.paddr;

    /* Configure ramfb via DMA */
    struct ramfb_cfg *cfg_ptr = (struct ramfb_cfg *)(dma + 128);
    cfg_ptr->addr   = bswap64(gpu_fb_pa);
    cfg_ptr->fourcc = bswap32(DRM_FORMAT_XRGB8888);
    cfg_ptr->flags  = 0;
    cfg_ptr->width  = bswap32(width);
    cfg_ptr->height = bswap32(height);
    cfg_ptr->stride = bswap32(width * 4);

    struct fw_cfg_dma_access *da = (struct fw_cfg_dma_access *)dma;
    da->control = bswap32(((uint32_t)ramfb_key << 16)
                          | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
    da->length  = bswap32(28);
    da->address = bswap64(dma_pa + 128);

    arch_dsb();

    volatile uint32_t *fw_dma_hi = (volatile uint32_t *)((uintptr_t)fw_vaddr + 0x10);
    volatile uint32_t *fw_dma_lo = (volatile uint32_t *)((uintptr_t)fw_vaddr + 0x14);

    *fw_dma_hi = bswap32((uint32_t)(dma_pa >> 32));
    arch_dsb();
    *fw_dma_lo = bswap32((uint32_t)(dma_pa & 0xFFFFFFFF));
    arch_dsb();

    for (int t = 0; t < 10000000; t++) {
        arch_dmb();
        if (da->control == 0) break;
    }

    if (da->control != 0) {
        printf("[gpu] DMA timeout\n");
        return -1;
    }

    gpu_available = 1;
    return 0;
}

/* ============================================================
 * plat_display_get_fb -- return framebuffer pointer
 * ============================================================ */
uint32_t *plat_display_get_fb(uint32_t *stride_out) {
    if (!gpu_available || !gpu_fb) return NULL;
    if (stride_out) *stride_out = gpu_width * 4;
    return gpu_fb;
}
