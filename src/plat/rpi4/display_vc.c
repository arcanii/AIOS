/*
 * display_vc.c -- VideoCore mailbox framebuffer driver (RPi4)
 *
 * PAL implementation for PLAT_RPI4 display.
 * Provides plat_display_init/get_fb matching display_hal.h.
 *
 * Uses the ARM-to-VideoCore mailbox property interface to allocate
 * a framebuffer, then maps the GPU-allocated pages into root VSpace.
 * Protocol proven in fb_test.c (bare-metal, v0.4.90).
 */
#include "aios/root_shared.h"
#include "aios/gpu.h"
#include "aios/vka_audit.h"
#include <sel4platsupport/device.h>
#include <stdio.h>
#include <string.h>
#include "arch.h"
#include "aios/hw_info.h"
#include "plat/display_hal.h"

/* Mailbox register offsets (from mailbox base, not page base) */
#define MBOX_READ_OFF    0x00
#define MBOX_STATUS_OFF  0x18
#define MBOX_WRITE_OFF   0x20
#define MBOX_FULL        0x80000000U
#define MBOX_EMPTY       0x40000000U
#define MBOX_CH_PROP     8

/* Mailbox property tags */
#define TAG_SET_PHYS_WH   0x00048003
#define TAG_SET_VIRT_WH   0x00048004
#define TAG_SET_VIRT_OFF  0x00048009
#define TAG_SET_DEPTH     0x00048005
#define TAG_SET_PIXEL_ORD 0x00048006
#define TAG_ALLOC_BUF     0x00040001
#define TAG_GET_PITCH     0x00040008
#define TAG_END           0x00000000

#define MBOX_REQ          0x00000000
#define MBOX_RESP_OK      0x80000000U

/* Maximum framebuffer pages (1024x768x4 = 768 pages, allow up to 1024) */
#define GPU_FB_MAX_PAGES  1024
static seL4_CPtr fb_caps[GPU_FB_MAX_PAGES];

/* Mapped mailbox registers */
static volatile uint32_t *mbox_regs;

/* ============================================================
 * mbox_call -- send property tag buffer via mailbox channel 8
 *
 * buf_pa: physical address of 16-byte aligned tag buffer
 * buf:    virtual address of same buffer (for reading response)
 * Returns: 0 on success, -1 on failure
 * ============================================================ */
static int mbox_call(uint64_t buf_pa, volatile uint32_t *buf) {
    uint32_t addr_ch = (uint32_t)(buf_pa & 0xFFFFFFF0U) | MBOX_CH_PROP;

    /* Wait for mailbox not full */
    for (int t = 0; t < 10000000; t++) {
        arch_dmb();
        if (!(mbox_regs[MBOX_STATUS_OFF / 4] & MBOX_FULL))
            break;
    }

    /* Write request */
    arch_dsb();
    mbox_regs[MBOX_WRITE_OFF / 4] = addr_ch;
    arch_dsb();

    /* Poll for response on our channel */
    for (int t = 0; t < 10000000; t++) {
        arch_dmb();
        if (mbox_regs[MBOX_STATUS_OFF / 4] & MBOX_EMPTY)
            continue;
        arch_dmb();
        uint32_t resp = mbox_regs[MBOX_READ_OFF / 4];
        if (resp == addr_ch) {
            arch_dmb();
            return (buf[1] == MBOX_RESP_OK) ? 0 : -1;
        }
    }

    printf("[gpu] VC mailbox timeout\n");
    return -1;
}

/* ============================================================
 * plat_display_init -- allocate framebuffer via VC mailbox
 * ============================================================ */
int plat_display_init(uint32_t width, uint32_t height) {
    int error;

    if (!hw_info.has_vc_mbox) {
        printf("[gpu] No VC mailbox in DTB\n");
        return -1;
    }

    /* --- Step 1: Map mailbox MMIO page --- */
    vka_object_t mbox_frame;
    error = sel4platsupport_alloc_frame_at(&vka, hw_info.vc_mbox_paddr,
                                            seL4_PageBits, &mbox_frame);
    if (error) {
        printf("[gpu] VC mbox alloc: %d\n", error);
        return -1;
    }
    void *mbox_vaddr = vspace_map_pages(&vspace, &mbox_frame.cptr, NULL,
        seL4_AllRights, 1, seL4_PageBits, 0);
    if (!mbox_vaddr) {
        printf("[gpu] VC mbox map fail\n");
        return -1;
    }

    /* Mailbox registers at offset 0x880 within the page.
     * DTB reports the page base (e.g. 0xFE00B000),
     * actual mailbox starts at 0xFE00B880. */
    uint32_t page_offset = (uint32_t)(hw_info.vc_mbox_paddr & 0xFFF);
    mbox_regs = (volatile uint32_t *)((uintptr_t)mbox_vaddr + 0x880 - page_offset);

    printf("[gpu] VC mailbox mapped at %p (paddr 0x%lx)\n",
           (void *)mbox_regs, (unsigned long)hw_info.vc_mbox_paddr);

    /* --- Step 2: Allocate DMA page for tag buffer --- */
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

    /* Zero the DMA page */
    memset(dma_vaddr, 0, 4096);

    /* --- Step 3: Build mailbox property tags --- */
    volatile uint32_t *tags = (volatile uint32_t *)dma_vaddr;
    int i = 0;

    tags[i++] = 35 * 4;        /* buffer size (bytes) */
    tags[i++] = MBOX_REQ;      /* request code */

    /* Set physical display size */
    tags[i++] = TAG_SET_PHYS_WH;
    tags[i++] = 8;              /* value buffer size */
    tags[i++] = 0;              /* request/response indicator */
    tags[i++] = width;
    tags[i++] = height;

    /* Set virtual framebuffer size */
    tags[i++] = TAG_SET_VIRT_WH;
    tags[i++] = 8;
    tags[i++] = 0;
    tags[i++] = width;
    tags[i++] = height;

    /* Set virtual offset (0,0) */
    tags[i++] = TAG_SET_VIRT_OFF;
    tags[i++] = 8;
    tags[i++] = 0;
    tags[i++] = 0;
    tags[i++] = 0;

    /* Set depth (32 bpp) */
    tags[i++] = TAG_SET_DEPTH;
    tags[i++] = 4;
    tags[i++] = 0;
    tags[i++] = 32;

    /* Set pixel order (1 = RGB) */
    tags[i++] = TAG_SET_PIXEL_ORD;
    tags[i++] = 4;
    tags[i++] = 0;
    tags[i++] = 1;

    /* Allocate framebuffer */
    int alloc_idx = i;          /* remember position for reading response */
    tags[i++] = TAG_ALLOC_BUF;
    tags[i++] = 8;
    tags[i++] = 0;
    tags[i++] = 4096;           /* alignment */
    tags[i++] = 0;              /* size (response) */

    /* Get pitch */
    int pitch_idx = i;
    tags[i++] = TAG_GET_PITCH;
    tags[i++] = 4;
    tags[i++] = 0;
    tags[i++] = 0;

    /* End tag */
    tags[i++] = TAG_END;

    arch_dsb();

    /* --- Step 4: Execute mailbox call --- */
    if (mbox_call(dma_pa, tags) != 0) {
        printf("[gpu] VC mailbox call failed\n");
        return -1;
    }

    /* Read response: framebuffer address and pitch */
    uint32_t fb_bus_addr = tags[alloc_idx + 3];
    uint32_t fb_size_resp = tags[alloc_idx + 4];
    uint32_t fb_pitch = tags[pitch_idx + 3];

    if (fb_bus_addr == 0) {
        printf("[gpu] VC returned null framebuffer\n");
        return -1;
    }

    /* Convert VC bus address to ARM physical address */
    uint64_t fb_arm_phys = (uint64_t)(fb_bus_addr & 0x3FFFFFFFU);

    printf("[gpu] VC FB: bus=0x%x arm=0x%lx size=%u pitch=%u\n",
           fb_bus_addr, (unsigned long)fb_arm_phys, fb_size_resp, fb_pitch);

    /* --- Step 5: Map GPU framebuffer pages into root VSpace --- */
    uint32_t fb_bytes = width * height * 4;
    uint32_t fb_pages = (fb_bytes + 4095) / 4096;
    if (fb_pages > GPU_FB_MAX_PAGES) {
        printf("[gpu] FB too large: %u pages\n", fb_pages);
        return -1;
    }

    for (uint32_t p = 0; p < fb_pages; p++) {
        vka_object_t frame;
        error = sel4platsupport_alloc_frame_at(&vka,
            fb_arm_phys + p * 0x1000, seL4_PageBits, &frame);
        if (error) {
            printf("[gpu] FB page %u alloc fail (paddr 0x%lx): %d\n",
                   p, (unsigned long)(fb_arm_phys + p * 0x1000), error);
            printf("[gpu] GPU memory may be outside seL4 untypeds\n");
            return -1;
        }
        fb_caps[p] = frame.cptr;
    }

    void *fb_vaddr = vspace_map_pages(&vspace, fb_caps, NULL,
        seL4_AllRights, fb_pages, seL4_PageBits, 0);
    if (!fb_vaddr) {
        printf("[gpu] FB map fail (%u pages)\n", fb_pages);
        return -1;
    }

    /* --- Step 6: Set global display state --- */
    gpu_fb     = (uint32_t *)fb_vaddr;
    gpu_fb_pa  = fb_arm_phys;
    gpu_width  = width;
    gpu_height = height;
    gpu_available = 1;

    printf("[gpu] RPi4 HDMI: %ux%u @ %p (%u pages)\n",
           width, height, fb_vaddr, fb_pages);

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
