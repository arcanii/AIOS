/*
 * display_vc.c -- VideoCore framebuffer driver (RPi4)
 *
 * PAL implementation for PLAT_RPI4 display.
 * Provides plat_display_init/get_fb matching display_hal.h.
 *
 * Two-phase initialization:
 *   Phase A: Map diagnostic stub framebuffer (fb_info at 0x3A000000).
 *            The DTS reserves 0x3A000000-0x40000000 as no-map, so seL4
 *            creates device untypeds for this range. Device untypeds
 *            are NOT zeroed on retype, preserving the fb_info struct
 *            written by the diagnostic stub before seL4 boot.
 *   Phase B: Fallback to full VC mailbox protocol (re-allocate FB).
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

/* ---------------------------------------------------------
 * fb_info struct -- must match diag_stub/diag_main.c layout
 * Written by diagnostic stub at phys FB_INFO_PADDR before
 * seL4 elfloader runs.
 * --------------------------------------------------------- */
struct fb_info {
    uint64_t ptr;       /* framebuffer ARM physical address */
    uint32_t pitch;     /* bytes per scanline */
    uint32_t width;
    uint32_t height;
    uint32_t line;      /* current text line (diag progress) */
};

#define FB_INFO_PADDR  0x3A000000UL

/* Maximum framebuffer pages (1024x768x4 = 768 pages) */
#define GPU_FB_MAX_PAGES  1024
static seL4_CPtr fb_caps[GPU_FB_MAX_PAGES];

/* ---------------------------------------------------------
 * Mailbox constants (used by Phase B fallback)
 * --------------------------------------------------------- */
#define MBOX_READ_OFF    0x00
#define MBOX_STATUS_OFF  0x18
#define MBOX_WRITE_OFF   0x20
#define MBOX_FULL        0x80000000U
#define MBOX_EMPTY       0x40000000U
#define MBOX_CH_PROP     8

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

static volatile uint32_t *mbox_regs;

/* Forward declaration */
static int plat_display_init_mailbox(uint32_t w, uint32_t h);

/* ============================================================
 * map_fb_pages -- map contiguous physical pages into vspace
 * Returns virtual address on success, NULL on failure.
 * ============================================================ */
static void *map_fb_pages(uint64_t phys_base, uint32_t n_pages) {
    if (n_pages == 0 || n_pages > GPU_FB_MAX_PAGES) {
        printf("[gpu] page count out of range: %u\n", n_pages);
        return NULL;
    }

    for (uint32_t p = 0; p < n_pages; p++) {
        vka_object_t frame;
        int err = sel4platsupport_alloc_frame_at(&vka,
            phys_base + (uint64_t)p * 0x1000,
            seL4_PageBits, &frame);
        if (err) {
            printf("[gpu] FB page %u at 0x%lx: err %d\n",
                   p, (unsigned long)(phys_base + p * 0x1000), err);
            return NULL;
        }
        fb_caps[p] = frame.cptr;
    }

    void *vaddr = vspace_map_pages(&vspace, fb_caps, NULL,
        seL4_AllRights, n_pages, seL4_PageBits, 0);
    if (!vaddr) {
        printf("[gpu] vspace_map_pages failed (%u pages)\n", n_pages);
    }
    return vaddr;
}

/* ============================================================
 * mbox_call -- send property tag buffer via mailbox channel 8
 * ============================================================ */
static int mbox_call(uint64_t buf_pa, volatile uint32_t *buf) {
    uint32_t addr_ch = (uint32_t)(buf_pa & 0xFFFFFFF0U) | MBOX_CH_PROP;

    /* Wait for mailbox not full */
    for (int t = 0; t < 10000000; t++) {
        arch_dmb();
        if (!(mbox_regs[MBOX_STATUS_OFF / 4] & MBOX_FULL))
            break;
    }

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
 * plat_display_init -- initialize RPi4 HDMI display
 *
 * Phase A: Use framebuffer pre-allocated by diagnostic stub.
 *          fb_info at phys 0x3A000000 (device untyped, preserved).
 * Phase B: Fallback to full VC mailbox protocol.
 * ============================================================ */
int plat_display_init(uint32_t width, uint32_t height) {
    /* Display disabled until serial boot is stable */
    printf("[gpu] Display disabled (serial mode)\n");
    return -1;

    /* === Phase A: Map diagnostic stub framebuffer === */

    printf("[gpu] Phase A: reading fb_info at 0x%lx\n",
           (unsigned long)FB_INFO_PADDR);

    /* Step 1: Map fb_info page (device untyped -- not zeroed) */
    vka_object_t info_frame;
    int err = sel4platsupport_alloc_frame_at(&vka, FB_INFO_PADDR,
                                              seL4_PageBits, &info_frame);
    if (err) {
        printf("[gpu] fb_info alloc at 0x%lx: err %d\n",
               (unsigned long)FB_INFO_PADDR, err);
        goto phase_b;
    }

    void *info_vaddr = vspace_map_pages(&vspace, &info_frame.cptr, NULL,
        seL4_AllRights, 1, seL4_PageBits, 0);
    if (!info_vaddr) {
        printf("[gpu] fb_info map failed\n");
        goto phase_b;
    }

    /* Step 2: Read and validate fb_info struct */
    volatile struct fb_info *info = (volatile struct fb_info *)info_vaddr;
    arch_dmb();

    uint64_t fb_pa    = info->ptr;
    uint32_t fb_pitch = info->pitch;
    uint32_t fb_w     = info->width;
    uint32_t fb_h     = info->height;

    printf("[gpu] fb_info: ptr=0x%lx pitch=%u %ux%u\n",
           (unsigned long)fb_pa, fb_pitch, fb_w, fb_h);

    /* Sanity checks */
    if (fb_pa == 0 || fb_w == 0 || fb_h == 0 ||
        fb_w > 4096 || fb_h > 4096 || fb_pitch == 0 ||
        fb_pitch < fb_w * 4) {
        printf("[gpu] fb_info invalid (zeroed or corrupt)\n");
        goto phase_b;
    }

    /* Step 3: Map framebuffer pages (also device untypeds) */
    uint32_t fb_bytes = fb_pitch * fb_h;
    uint32_t fb_pages = (fb_bytes + 4095) / 4096;

    printf("[gpu] Mapping %u FB pages at phys 0x%lx (%u bytes)\n",
           fb_pages, (unsigned long)fb_pa, fb_bytes);

    void *fb_vaddr = map_fb_pages(fb_pa, fb_pages);
    if (!fb_vaddr) {
        printf("[gpu] FB page mapping failed\n");
        printf("[gpu] GPU memory may be outside device untypeds\n");
        goto phase_b;
    }

    /* Phase A success -- use stub framebuffer */
    gpu_fb        = (uint32_t *)fb_vaddr;
    gpu_fb_pa     = fb_pa;
    gpu_width     = fb_w;
    gpu_height    = fb_h;
    gpu_available = 1;

    printf("[gpu] RPi4 HDMI (stub FB): %ux%u @ %p (%u pages)\n",
           fb_w, fb_h, fb_vaddr, fb_pages);
    return 0;

phase_b:
    printf("[gpu] Phase A failed, trying Phase B (VC mailbox)\n");
    return plat_display_init_mailbox(width, height);
}

/* ============================================================
 * plat_display_init_mailbox -- Phase B: full VC mailbox protocol
 *
 * Allocates a new framebuffer via VideoCore property mailbox.
 * This is the fallback if the diagnostic stub FB is unavailable
 * (e.g. QEMU RPi4 emulation, or no diagnostic stub).
 * ============================================================ */
static int plat_display_init_mailbox(uint32_t width, uint32_t height) {
    int error;

    if (!hw_info.has_vc_mbox) {
        printf("[gpu] No VC mailbox in DTB\n");
        return -1;
    }

    /* Map mailbox MMIO page */
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

    uint32_t page_offset = (uint32_t)(hw_info.vc_mbox_paddr & 0xFFF);
    mbox_regs = (volatile uint32_t *)((uintptr_t)mbox_vaddr +
                                       0x880 - page_offset);

    printf("[gpu] VC mailbox mapped at %p (paddr 0x%lx)\n",
           (void *)mbox_regs, (unsigned long)hw_info.vc_mbox_paddr);

    /* Allocate DMA page for tag buffer */
    vka_object_t dma_frame_obj;
    vka_audit_untyped(VKA_SUB_GPU, 12);
    error = vka_alloc_frame(&vka, seL4_PageBits, &dma_frame_obj);
    if (error) {
        printf("[gpu] DMA alloc fail\n");
        return -1;
    }

    void *dma_vaddr = vspace_map_pages(&vspace, &dma_frame_obj.cptr, NULL,
        seL4_AllRights, 1, seL4_PageBits, 0);
    if (!dma_vaddr) {
        printf("[gpu] DMA map fail\n");
        return -1;
    }

    seL4_ARM_Page_GetAddress_t ga =
        seL4_ARM_Page_GetAddress(dma_frame_obj.cptr);
    if (ga.error) {
        printf("[gpu] DMA addr fail\n");
        return -1;
    }
    uint64_t dma_pa = ga.paddr;

    memset(dma_vaddr, 0, 4096);

    /* Build mailbox property tags */
    volatile uint32_t *tags = (volatile uint32_t *)dma_vaddr;
    int i = 0;

    tags[i++] = 35 * 4;        /* buffer size */
    tags[i++] = MBOX_REQ;

    tags[i++] = TAG_SET_PHYS_WH;
    tags[i++] = 8; tags[i++] = 0;
    tags[i++] = width; tags[i++] = height;

    tags[i++] = TAG_SET_VIRT_WH;
    tags[i++] = 8; tags[i++] = 0;
    tags[i++] = width; tags[i++] = height;

    tags[i++] = TAG_SET_VIRT_OFF;
    tags[i++] = 8; tags[i++] = 0;
    tags[i++] = 0; tags[i++] = 0;

    tags[i++] = TAG_SET_DEPTH;
    tags[i++] = 4; tags[i++] = 0; tags[i++] = 32;

    tags[i++] = TAG_SET_PIXEL_ORD;
    tags[i++] = 4; tags[i++] = 0; tags[i++] = 1;

    int alloc_idx = i;
    tags[i++] = TAG_ALLOC_BUF;
    tags[i++] = 8; tags[i++] = 0;
    tags[i++] = 4096; tags[i++] = 0;

    int pitch_idx = i;
    tags[i++] = TAG_GET_PITCH;
    tags[i++] = 4; tags[i++] = 0; tags[i++] = 0;

    tags[i++] = TAG_END;

    arch_dsb();

    /* Execute mailbox call */
    if (mbox_call(dma_pa, tags) != 0) {
        printf("[gpu] VC mailbox call failed\n");
        return -1;
    }

    uint32_t fb_bus_addr  = tags[alloc_idx + 3];
    uint32_t fb_size_resp = tags[alloc_idx + 4];
    uint32_t fb_pitch     = tags[pitch_idx + 3];

    if (fb_bus_addr == 0) {
        printf("[gpu] VC returned null framebuffer\n");
        return -1;
    }

    /* Convert VC bus address to ARM physical */
    uint64_t fb_arm_phys = (uint64_t)(fb_bus_addr & 0x3FFFFFFFU);

    printf("[gpu] VC FB: bus=0x%x arm=0x%lx size=%u pitch=%u\n",
           fb_bus_addr, (unsigned long)fb_arm_phys,
           fb_size_resp, fb_pitch);

    /* Map framebuffer pages */
    uint32_t fb_bytes = width * height * 4;
    uint32_t fb_pages = (fb_bytes + 4095) / 4096;

    void *fb_vaddr = map_fb_pages(fb_arm_phys, fb_pages);
    if (!fb_vaddr) {
        printf("[gpu] FB map fail -- GPU mem outside device untypeds\n");
        printf("[gpu] Ensure DTS reserved-memory covers 0x%lx\n",
               (unsigned long)fb_arm_phys);
        return -1;
    }

    gpu_fb        = (uint32_t *)fb_vaddr;
    gpu_fb_pa     = fb_arm_phys;
    gpu_width     = width;
    gpu_height    = height;
    gpu_available = 1;

    printf("[gpu] RPi4 HDMI (mailbox): %ux%u @ %p (%u pages)\n",
           width, height, fb_vaddr, fb_pages);
    return 0;
}

/* ============================================================
 * plat_display_get_fb -- return framebuffer pointer and stride
 * ============================================================ */
uint32_t *plat_display_get_fb(uint32_t *stride_out) {
    if (!gpu_available || !gpu_fb) return NULL;
    if (stride_out) *stride_out = gpu_width * 4;
    return gpu_fb;
}
