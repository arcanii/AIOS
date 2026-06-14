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

/* ============================================================ *
 *  Phase 3 -- the rainbow triangle (GL Shader State path)       *
 * ============================================================ */

/* A Phase 3 GL-draw of one triangle. All addresses are V3D GPU virtual addresses.
 * The caller lays out the BOs and resolves every address; these emitters just pack
 * bytes (explicit writes, no bitfields, so host clgen + kernel agree byte-for-byte).
 * Differs structurally from the clear: TRMC order is Common->ClearColors->Color->ZS,
 * the generic tile list lives in a SEPARATE buffer (referenced by packet 20), and the
 * render adds a D16 Z store. */
struct v3d_tri_params {
    uint16_t width;             /* render-target width  in pixels                   */
    uint16_t height;            /* render-target height in pixels                   */
    uint32_t clear_color;       /* FB-order clear value (golden: 0xff101010)        */
    uint8_t  rb_swap;           /* color STORE r_b_swap: 0 AIOS BGR FB / 1 golden   */
    uint32_t fb_va;             /* RT0 (framebuffer) GPU VA                          */
    uint32_t fb_stride;         /* RT0 row stride in bytes (width * 4)              */
    uint32_t z_va;              /* depth (D16) buffer GPU VA                         */
    uint32_t tile_alloc_va;     /* PTB tile-allocation pool GPU VA (64B aligned)     */
    uint32_t shader_record_va;  /* GL Shader State Record GPU VA (16-byte aligned)   */
    uint8_t  nattr;             /* number of attribute arrays (2: pos + color)      */
    uint32_t tile_list_va;      /* generic tile-list buffer GPU VA (start)           */
    uint32_t vertex_count;      /* number of vertices (3)                            */
};

/* Resolved shader/uniform addresses for the GL Shader State Record (36 B). */
struct v3d_shader_record_params {
    uint32_t default_attr_va;   /* default-attribute-values block                   */
    uint32_t frag_code_va;      /* fragment shader code (8-byte aligned)            */
    uint32_t frag_unif_va;      /* fragment shader uniforms                         */
    uint32_t vtx_code_va;       /* vertex shader code (8-byte aligned)              */
    uint32_t vtx_unif_va;       /* vertex shader uniforms                           */
    uint32_t coord_code_va;     /* coordinate shader code (8-byte aligned)          */
    uint32_t coord_unif_va;     /* coordinate shader uniforms                       */
    uint8_t  num_fs_varyings;   /* number_of_varyings_in_fragment_shader (4)        */
};

/* Attribute record (16 B) parameters. */
struct v3d_attr_params {
    uint32_t address;           /* attribute data GPU VA                            */
    uint8_t  vec_size;          /* components - ... raw (pos=3, color=0)            */
    uint8_t  type;              /* 2 = float, 4 = ubyte                             */
    uint8_t  normalized;        /* normalized_int_type                              */
    uint8_t  num_read_vtx;      /* number_of_values_read_by_vertex_shader           */
    uint8_t  num_read_coord;    /* number_of_values_read_by_coordinate_shader       */
    uint32_t stride;            /* byte stride                                      */
    uint32_t max_index;         /* maximum_index (0xFFFFFF)                         */
};

/* Build the triangle binning CL. Returns bytes written (clear-scene fixed-field +
 * scene/address-derived packets), or -1 if cap too small. */
int v3d_build_triangle_bin_cl(uint8_t *buf, int cap, const struct v3d_tri_params *p);

/* Build the triangle render CL. Returns bytes written, or -1. */
int v3d_build_triangle_render_cl(uint8_t *buf, int cap, const struct v3d_tri_params *p);

/* Build the generic tile list (the per-tile draw sub-list referenced by packet 20:
 * implicit coords, prim format, branch, RT0 store, Z store, clear, end, return).
 * Lives in its own buffer. Returns bytes written, or -1. */
int v3d_build_triangle_tile_list(uint8_t *buf, int cap, const struct v3d_tri_params *p);

/* Build the GL Shader State Record (36 B) into buf. Returns 36, or -1. */
int v3d_build_shader_record(uint8_t *buf, int cap, const struct v3d_shader_record_params *p);

/* Build one attribute record (16 B) into buf. Returns 16, or -1. */
int v3d_build_attr_record(uint8_t *buf, int cap, const struct v3d_attr_params *a);

#endif /* AIOS_V3D_CL_H */
