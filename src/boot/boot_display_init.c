/*
 * boot_display_init.c -- ramfb framebuffer + font + splash
 *
 * Uses QEMU fw_cfg interface to configure a simple RAM framebuffer.
 * QEMU command: -device ramfb (no virtio needed).
 *
 * Includes embedded 8x8 bitmap font for text rendering and
 * optional splash image loading from /images/splash.raw.
 */
#include "aios/root_shared.h"
#include "aios/gpu.h"
#include "aios/vka_audit.h"
#include "aios/vfs.h"
#include "aios/version.h"
#include <sel4platsupport/device.h>
#define LOG_MODULE "gpu"
#define LOG_LEVEL LOG_LEVEL_DEBUG
#include "aios/aios_log.h"
#include <stdio.h>

#define FWCFG_PADDR     0x09020000UL
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

#define DISPLAY_W  1024
#define DISPLAY_H  768

/* ================================================================
 * 8x8 bitmap font -- ASCII 32..126 (95 glyphs, 760 bytes)
 * Each glyph: 8 rows, MSB = leftmost pixel.
 * ================================================================ */

const uint8_t font8x8[95][8] = {
 {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
 {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, /* ! */
 {0x6C,0x6C,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
 {0x24,0x24,0x7E,0x24,0x7E,0x24,0x24,0x00}, /* # */
 {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, /* $ */
 {0x00,0x62,0x64,0x08,0x10,0x26,0x46,0x00}, /* % */
 {0x38,0x44,0x38,0x3A,0x44,0x44,0x3A,0x00}, /* & */
 {0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' (single-quote) */
 {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* ( */
 {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* ) */
 {0x00,0x24,0x18,0x7E,0x18,0x24,0x00,0x00}, /* * */
 {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* + */
 {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* , */
 {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* - */
 {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . */
 {0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x00}, /* / */
 {0x3C,0x42,0x46,0x5A,0x62,0x42,0x3C,0x00}, /* 0 */
 {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 1 */
 {0x3C,0x42,0x02,0x0C,0x30,0x40,0x7E,0x00}, /* 2 */
 {0x3C,0x42,0x02,0x1C,0x02,0x42,0x3C,0x00}, /* 3 */
 {0x04,0x0C,0x14,0x24,0x7E,0x04,0x04,0x00}, /* 4 */
 {0x7E,0x40,0x7C,0x02,0x02,0x42,0x3C,0x00}, /* 5 */
 {0x1C,0x20,0x40,0x7C,0x42,0x42,0x3C,0x00}, /* 6 */
 {0x7E,0x02,0x04,0x08,0x10,0x10,0x10,0x00}, /* 7 */
 {0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00}, /* 8 */
 {0x3C,0x42,0x42,0x3E,0x02,0x04,0x38,0x00}, /* 9 */
 {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, /* : */
 {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00}, /* ; */
 {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* < */
 {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* = */
 {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* > */
 {0x3C,0x42,0x02,0x0C,0x18,0x00,0x18,0x00}, /* ? */
 {0x3C,0x42,0x4E,0x52,0x4E,0x40,0x3C,0x00}, /* @ */
 {0x18,0x24,0x42,0x42,0x7E,0x42,0x42,0x00}, /* A */
 {0x7C,0x42,0x42,0x7C,0x42,0x42,0x7C,0x00}, /* B */
 {0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00}, /* C */
 {0x78,0x44,0x42,0x42,0x42,0x44,0x78,0x00}, /* D */
 {0x7E,0x40,0x40,0x7C,0x40,0x40,0x7E,0x00}, /* E */
 {0x7E,0x40,0x40,0x7C,0x40,0x40,0x40,0x00}, /* F */
 {0x3C,0x42,0x40,0x4E,0x42,0x42,0x3C,0x00}, /* G */
 {0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x00}, /* H */
 {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* I */
 {0x1E,0x04,0x04,0x04,0x04,0x44,0x38,0x00}, /* J */
 {0x42,0x44,0x48,0x70,0x48,0x44,0x42,0x00}, /* K */
 {0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00}, /* L */
 {0x42,0x66,0x5A,0x5A,0x42,0x42,0x42,0x00}, /* M */
 {0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x00}, /* N */
 {0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00}, /* O */
 {0x7C,0x42,0x42,0x7C,0x40,0x40,0x40,0x00}, /* P */
 {0x3C,0x42,0x42,0x42,0x4A,0x44,0x3A,0x00}, /* Q */
 {0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x00}, /* R */
 {0x3C,0x42,0x40,0x3C,0x02,0x42,0x3C,0x00}, /* S */
 {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* T */
 {0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00}, /* U */
 {0x42,0x42,0x42,0x42,0x24,0x24,0x18,0x00}, /* V */
 {0x42,0x42,0x42,0x5A,0x5A,0x66,0x42,0x00}, /* W */
 {0x42,0x42,0x24,0x18,0x24,0x42,0x42,0x00}, /* X */
 {0x42,0x42,0x24,0x18,0x18,0x18,0x18,0x00}, /* Y */
 {0x7E,0x04,0x08,0x10,0x20,0x40,0x7E,0x00}, /* Z */
 {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* [ */
 {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x00}, /* \ */
 {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* ] */
 {0x10,0x28,0x44,0x00,0x00,0x00,0x00,0x00}, /* ^ */
 {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00}, /* _ */
 {0x18,0x08,0x00,0x00,0x00,0x00,0x00,0x00}, /* ` */
 {0x00,0x00,0x3C,0x02,0x3E,0x42,0x3E,0x00}, /* a */
 {0x40,0x40,0x7C,0x42,0x42,0x42,0x7C,0x00}, /* b */
 {0x00,0x00,0x3C,0x42,0x40,0x42,0x3C,0x00}, /* c */
 {0x02,0x02,0x3E,0x42,0x42,0x42,0x3E,0x00}, /* d */
 {0x00,0x00,0x3C,0x42,0x7E,0x40,0x3C,0x00}, /* e */
 {0x0C,0x12,0x10,0x7C,0x10,0x10,0x10,0x00}, /* f */
 {0x00,0x00,0x3E,0x42,0x42,0x3E,0x02,0x3C}, /* g */
 {0x40,0x40,0x7C,0x42,0x42,0x42,0x42,0x00}, /* h */
 {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* i */
 {0x04,0x00,0x04,0x04,0x04,0x04,0x44,0x38}, /* j */
 {0x40,0x40,0x44,0x48,0x70,0x48,0x44,0x00}, /* k */
 {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* l */
 {0x00,0x00,0x76,0x49,0x49,0x49,0x49,0x00}, /* m */
 {0x00,0x00,0x7C,0x42,0x42,0x42,0x42,0x00}, /* n */
 {0x00,0x00,0x3C,0x42,0x42,0x42,0x3C,0x00}, /* o */
 {0x00,0x00,0x7C,0x42,0x42,0x7C,0x40,0x40}, /* p */
 {0x00,0x00,0x3E,0x42,0x42,0x3E,0x02,0x02}, /* q */
 {0x00,0x00,0x5C,0x62,0x40,0x40,0x40,0x00}, /* r */
 {0x00,0x00,0x3E,0x40,0x3C,0x02,0x7C,0x00}, /* s */
 {0x10,0x10,0x7C,0x10,0x10,0x12,0x0C,0x00}, /* t */
 {0x00,0x00,0x42,0x42,0x42,0x42,0x3E,0x00}, /* u */
 {0x00,0x00,0x42,0x42,0x42,0x24,0x18,0x00}, /* v */
 {0x00,0x00,0x42,0x42,0x5A,0x66,0x42,0x00}, /* w */
 {0x00,0x00,0x42,0x24,0x18,0x24,0x42,0x00}, /* x */
 {0x00,0x00,0x42,0x42,0x42,0x3E,0x02,0x3C}, /* y */
 {0x00,0x00,0x7E,0x04,0x18,0x20,0x7E,0x00}, /* z */
 {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, /* { */
 {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* | */
 {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, /* } */
 {0x00,0x00,0x32,0x4C,0x00,0x00,0x00,0x00}, /* ~ */
};

/* ---- Text rendering ---- */

void gpu_draw_char(int x, int y, char ch,
                          uint32_t fg, int scale) {
    if (ch < 32 || ch > 126) return;
    const uint8_t *g = font8x8[ch - 32];
    uint32_t w = gpu_width;
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;
                        if (px >= 0 && px < (int)w &&
                            py >= 0 && py < (int)gpu_height)
                            gpu_fb[py * w + px] = fg;
                    }
            }
        }
    }
}

void gpu_draw_text(int x, int y, const char *str,
                          uint32_t fg, int scale) {
    while (*str) {
        gpu_draw_char(x, y, *str, fg, scale);
        x += 8 * scale;
        str++;
    }
}

/* ---- Gradient background ---- */

static void gpu_draw_background(void) {
    uint32_t w = gpu_width, h = gpu_height;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t r = (x * 50) / w + 20;
            uint32_t g = (y * 40) / h + 25;
            uint32_t b = 60 + (x * 30) / w;
            gpu_fb[y * w + x] = GPU_PIXEL(r, g, b);
        }
}

/* ---- Splash image loader ---- */

static void gpu_load_splash(void) {
    /* Use elf_buf as temporary read buffer (8MB, available at boot) */
    int img_size = vfs_read("/images/splash.raw", elf_buf,
                            sizeof(elf_buf));
    if (img_size < 16) return;  /* no file or too small */

    uint32_t *hdr = (uint32_t *)elf_buf;
    uint32_t iw = hdr[0], ih = hdr[1], fmt = hdr[2];

    if (fmt != 0) {
        printf("[gpu] splash: unsupported format %u\n", fmt);
        return;
    }
    if (iw == 0 || ih == 0 || iw > 4096 || ih > 4096) {
        printf("[gpu] splash: bad dimensions %ux%u\n", iw, ih);
        return;
    }
    uint32_t expected = 16 + iw * ih * 4;
    if ((uint32_t)img_size < expected) {
        printf("[gpu] splash: truncated (%d < %u)\n", img_size, expected);
        return;
    }

    uint32_t *pixels = (uint32_t *)(elf_buf + 16);

    /* Center on screen (or top-left if larger than screen) */
    uint32_t ox = (iw < gpu_width)  ? (gpu_width  - iw) / 2 : 0;
    uint32_t oy = (ih < gpu_height) ? (gpu_height - ih) / 2 : 0;
    uint32_t cw = (iw < gpu_width)  ? iw : gpu_width;
    uint32_t ch = (ih < gpu_height) ? ih : gpu_height;

    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++)
            gpu_fb[(oy + y) * gpu_width + (ox + x)] = pixels[y * iw + x];

    printf("[gpu] Splash: %ux%u at (%u,%u)\n", iw, ih, ox, oy);
}

/* ================================================================
 * boot_display_init
 * ================================================================ */

void boot_display_init(void) {
    int error;

    /* Map fw_cfg MMIO */
    vka_object_t fw_frame;
    error = sel4platsupport_alloc_frame_at(&vka, FWCFG_PADDR,
                                            seL4_PageBits, &fw_frame);
    if (error) { printf("[gpu] fw_cfg alloc: %d\n", error); return; }
    void *fw_vaddr = vspace_map_pages(&vspace, &fw_frame.cptr, NULL,
        seL4_AllRights, 1, seL4_PageBits, 0);
    if (!fw_vaddr) { printf("[gpu] fw_cfg map fail\n"); return; }

    volatile uint8_t  *fw_data = (volatile uint8_t *)fw_vaddr;
    volatile uint16_t *fw_sel  = (volatile uint16_t *)((uintptr_t)fw_vaddr + 8);

    /* Enumerate fw_cfg files */
    *fw_sel = bswap16(FWCFG_FILE_DIR);
    __asm__ volatile("dsb sy" ::: "memory");

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
        return;
    }

    /* Allocate DMA page */
    vka_object_t dma_frame_obj;
    vka_audit_untyped(VKA_SUB_GPU, 12);
    error = vka_alloc_frame(&vka, seL4_PageBits, &dma_frame_obj);
    if (error) { printf("[gpu] DMA alloc fail\n"); return; }
    void *dma_vaddr = vspace_map_pages(&vspace, &dma_frame_obj.cptr, NULL,
        seL4_AllRights, 1, seL4_PageBits, 0);
    if (!dma_vaddr) { printf("[gpu] DMA map fail\n"); return; }

    seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_frame_obj.cptr);
    if (ga.error) { printf("[gpu] DMA addr fail\n"); return; }
    uint64_t dma_pa = ga.paddr;
    uint8_t *dma = (uint8_t *)dma_vaddr;
    for (int i = 0; i < 4096; i++) dma[i] = 0;

    /* Allocate framebuffer */
    gpu_width  = DISPLAY_W;
    gpu_height = DISPLAY_H;
    uint32_t fb_size  = gpu_width * gpu_height * 4;
    uint32_t fb_pages = (fb_size + 4095) / 4096;

    int fb_bits = 12;
    while ((1U << fb_bits) < (fb_pages * 4096)) fb_bits++;
    uint32_t fb_ut_pages = (1U << fb_bits) / 4096;
    if (fb_ut_pages > GPU_FB_MAX_PAGES) fb_ut_pages = GPU_FB_MAX_PAGES;

    vka_object_t fb_ut;
    vka_audit_untyped(VKA_SUB_GPU, fb_bits);
    error = vka_alloc_untyped(&vka, fb_bits, &fb_ut);
    if (error) { printf("[gpu] FB alloc fail: %d\n", error); return; }

    for (uint32_t i = 0; i < fb_ut_pages; i++) {
        seL4_CPtr slot;
        error = vka_cspace_alloc(&vka, &slot);
        if (error) { printf("[gpu] FB cslot %u\n", i); return; }
        error = seL4_Untyped_Retype(fb_ut.cptr,
            seL4_ARM_SmallPageObject, seL4_PageBits,
            seL4_CapInitThreadCNode, 0, 0, slot, 1);
        if (error) { printf("[gpu] FB retype %u: %d\n", i, error); return; }
        fb_caps[i] = slot;
    }

    void *fb_vaddr = vspace_map_pages(&vspace, fb_caps, NULL,
        seL4_AllRights, fb_ut_pages, seL4_PageBits, 0);
    if (!fb_vaddr) { printf("[gpu] FB map fail\n"); return; }

    ga = seL4_ARM_Page_GetAddress(fb_caps[0]);
    if (ga.error) { printf("[gpu] FB addr fail\n"); return; }

    gpu_fb    = (uint32_t *)fb_vaddr;
    gpu_fb_pa = ga.paddr;

    /* Draw gradient background */
    gpu_draw_background();

    /* Configure ramfb via DMA */
    struct ramfb_cfg *cfg = (struct ramfb_cfg *)(dma + 128);
    cfg->addr   = bswap64(gpu_fb_pa);
    cfg->fourcc = bswap32(DRM_FORMAT_XRGB8888);
    cfg->flags  = 0;
    cfg->width  = bswap32(gpu_width);
    cfg->height = bswap32(gpu_height);
    cfg->stride = bswap32(gpu_width * 4);

    struct fw_cfg_dma_access *da = (struct fw_cfg_dma_access *)dma;
    da->control = bswap32(((uint32_t)ramfb_key << 16)
                          | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
    da->length  = bswap32(28);
    da->address = bswap64(dma_pa + 128);

    __asm__ volatile("dsb sy" ::: "memory");

    volatile uint32_t *fw_dma_hi = (volatile uint32_t *)((uintptr_t)fw_vaddr + 0x10);
    volatile uint32_t *fw_dma_lo = (volatile uint32_t *)((uintptr_t)fw_vaddr + 0x14);

    *fw_dma_hi = bswap32((uint32_t)(dma_pa >> 32));
    __asm__ volatile("dsb sy" ::: "memory");
    *fw_dma_lo = bswap32((uint32_t)(dma_pa & 0xFFFFFFFF));
    __asm__ volatile("dsb sy" ::: "memory");

    for (int t = 0; t < 10000000; t++) {
        __asm__ volatile("dmb sy" ::: "memory");
        if (da->control == 0) break;
    }

    if (da->control != 0) {
        printf("[gpu] DMA timeout\n");
        return;
    }

    gpu_available = 1;

    /* Render boot text on the framebuffer */
    gpu_draw_text(40, 42, "AIOS " AIOS_VERSION_STR,
                  GPU_PIXEL(255, 255, 255), 3);

    char info[80];
    int ip = 0;
    const char *s1 = "seL4 microkernel | AArch64 | ";
    while (*s1) info[ip++] = *s1++;
    /* Append resolution digits */
    int dw = (int)gpu_width, dh = (int)gpu_height;
    if (dw >= 1000) info[ip++] = '0' + (dw / 1000);
    info[ip++] = '0' + ((dw / 100) % 10);
    info[ip++] = '0' + ((dw / 10) % 10);
    info[ip++] = '0' + (dw % 10);
    info[ip++] = 'x';
    info[ip++] = '0' + ((dh / 100) % 10);
    info[ip++] = '0' + ((dh / 10) % 10);
    info[ip++] = '0' + (dh % 10);
    const char *s2 = " ramfb";
    while (*s2) info[ip++] = *s2++;
    info[ip] = '\0';
    gpu_draw_text(40, 75, info, GPU_PIXEL(180, 200, 220), 1);

    char mem[32];
    int mp = 0;
    int mb = (int)(aios_total_mem / (1024 * 1024));
    if (mb >= 100) mem[mp++] = '0' + (mb / 100);
    if (mb >= 10)  mem[mp++] = '0' + ((mb / 10) % 10);
    mem[mp++] = '0' + (mb % 10);
    const char *s3 = " MB RAM | 4 cores SMP";
    while (*s3) mem[mp++] = *s3++;
    mem[mp] = '\0';
    gpu_draw_text(40, 88, mem, GPU_PIXEL(180, 200, 220), 1);

    /* Try to load splash image from disk */
    gpu_load_splash();

    printf("[boot] Display: %ux%u ramfb + text\n", gpu_width, gpu_height);
    LOG_INFO("ramfb display initialized");
}
