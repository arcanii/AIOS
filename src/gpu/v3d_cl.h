/*
 * v3d_cl.h -- V3D 4.2 control-list emitters (Phase 2: GPU clear to framebuffer).
 *
 * Pure byte construction: the emitters write CLE packet streams into a
 * caller-provided buffer and never touch MMIO, so they compile both in the
 * kernel and host-side (tools/v3d_host_clgen.c) for the byte-exact golden gate
 * (scripts/v3d_clcheck.py vs the Random06457-derived fixture). Packet encodings
 * are transcribed from that captured reference (see project_v3d_phase2); every
 * opcode/field/bias is documented at the emit site in v3d_cl.c.
 */
#ifndef AIOS_V3D_CL_H
#define AIOS_V3D_CL_H

#include <stdint.h>

/* A Phase 2 GPU clear-to-framebuffer job. All addresses are V3D GPU virtual
 * addresses (the bus addresses the CLE fetches control lists / stores pixels
 * at). Tile size (64x64) and supertile size (4x4 tiles) are fixed by design. */
struct v3d_clear_params {
    uint16_t width;         /* render-target width  in pixels (e.g. 1024)        */
    uint16_t height;        /* render-target height in pixels (e.g.  768)        */
    uint32_t clear_color;   /* low-32 clear value in FB byte order (BGR orange = 0xFFFF8000) */
    uint32_t fb_va;         /* GPU VA of the render target / framebuffer         */
    uint32_t fb_stride;     /* render-target row stride in bytes (width * 4)     */
    uint32_t tile_alloc_va; /* GPU VA of the tile-allocation (PTB) pool; 64B aligned */
    uint8_t  rb_swap;       /* STORE r_b_swap: 1 for an RGB FB (Random06457 golden),
                             * 0 for AIOS's BGR FB (pixel-ord 0). HW-confirmed: with
                             * rb_swap=1 the orange clear stores R/B-swapped (azure). */
};

/* Build the binning control list into buf (cap bytes). Address-independent --
 * depends only on width/height. Returns bytes written, or -1 if cap too small. */
int v3d_build_bin_cl(uint8_t *buf, int cap, const struct v3d_clear_params *p);

/* Build the render control list into buf (cap bytes). render_cl_va is the GPU VA
 * the buffer will live at -- needed because packet 20 self-references the inline
 * generic tile sub-list by absolute address. Returns bytes written, or -1. */
int v3d_build_render_cl(uint8_t *buf, int cap, uint32_t render_cl_va,
                        const struct v3d_clear_params *p);

#endif /* AIOS_V3D_CL_H */
