/*
 * v3d_cl.c -- V3D 4.2 control-list emitters for the Phase 2 GPU clear.
 *
 * Reproduces, byte for byte, the Random06457-derived golden clear CLs (see
 * project_v3d_phase2 + scripts/v3d_clcheck.py). Encoding facts (clang-packed,
 * little-endian, LSB-first, opcode at byte 0; many fields stored biased) are
 * noted per packet. No MMIO / platform code -- pure buffer construction, so the
 * same object compiles in the kernel and host-side for the golden gate.
 */
#include "v3d_cl.h"

/* ---- little-endian cursor writers ---- */
static void w8(uint8_t **p, uint32_t v)  { *(*p)++ = (uint8_t)v; }
static void w16(uint8_t **p, uint32_t v) { w8(p, v); w8(p, v >> 8); }
static void w24(uint8_t **p, uint32_t v) { w8(p, v); w8(p, v >> 8); w8(p, v >> 16); }
static void w32(uint8_t **p, uint32_t v) { w16(p, v); w16(p, v >> 16); }
static void w64(uint8_t **p, uint64_t v) { w32(p, (uint32_t)v); w32(p, (uint32_t)(v >> 32)); }

/* tile geometry is fixed by design: 64x64-pixel tiles, 4x4-tile supertiles */
#define V3D_TILE_W       64u
#define V3D_TILE_H       64u
#define V3D_SUPERTILE_T  4u    /* tiles per supertile edge */

/* STORE_TILE_BUFFER_GENERAL (opcode 29, 13 B): store RT0 -> framebuffer.
 * b1   : buffer_to_store=0(RT0) | mem_format=0(RASTER) | flip_y=0
 * b2..3: output_image_format=27(RGBA8) at [4:6] | r_b_swap at [12]
 * b4..6: height_or_stride at [4:20]  = stride << 4  (24-bit)
 * b7..8: height = 0
 * b9..12: address = fb_va */
static void emit_store_rt0(uint8_t **q, uint32_t stride, uint32_t fb_va, int rb_swap) {
    w8(q, 29);
    w8(q, 0x00);
    w16(q, (27u << 4) | (rb_swap ? (1u << 12) : 0u));
    w24(q, stride << 4);
    w16(q, 0);
    w32(q, fb_va);
}

int v3d_build_bin_cl(uint8_t *buf, int cap, const struct v3d_clear_params *p) {
    if (cap < 17) return -1;
    uint8_t *q = buf;

    /* TILE_BINNING_MODE_CFG (120, 9 B): nRT=1, 32bpp, 64B blocks.
     * b1: init_block_size [2:2]=0(64B), block_size [4:2]=0(64B)
     * b2: number_of_render_targets [0:4] = nRT-1 = 0; max_bpp/msaa/dbuf = 0
     * b5..6/b7..8: width-1 / height-1 (BIASED) */
    w8(&q, 120); w8(&q, 0); w8(&q, 0); w8(&q, 0); w8(&q, 0);
    w16(&q, (uint32_t)p->width  - 1);
    w16(&q, (uint32_t)p->height - 1);

    w8(&q, 19);                 /* FLUSH_VCD_CACHE          */
    w8(&q, 92); w32(&q, 0);     /* OCCLUSION_QUERY_COUNTER, address = 0 */
    w8(&q, 6);                  /* START_TILE_BINNING       */
    w8(&q, 4);                  /* FLUSH                    */
    return (int)(q - buf);      /* 17 */
}

int v3d_build_render_cl(uint8_t *buf, int cap, uint32_t render_cl_va,
                        const struct v3d_clear_params *p) {
    uint32_t tilesX = ((uint32_t)p->width  + V3D_TILE_W - 1) / V3D_TILE_W;
    uint32_t tilesY = ((uint32_t)p->height + V3D_TILE_H - 1) / V3D_TILE_H;
    uint32_t stX = (tilesX + V3D_SUPERTILE_T - 1) / V3D_SUPERTILE_T;
    uint32_t stY = (tilesY + V3D_SUPERTILE_T - 1) / V3D_SUPERTILE_T;
    /* fixed bytes (non-supertile-coord) = 129; each supertile coord = 3 B */
    if (cap < (int)(129u + 3u * stX * stY)) return -1;

    uint8_t *q = buf;

    /* TILE_RENDERING_MODE_CFG (121) -- four sub-packets in the design order
     * Common -> Color -> Clear Colors -> ZS Clear Values (LAST). */
    /* Common (sub_id=0, 9 B): nRT-1=0; RAW width/height (NOT biased here);
     * b6..7 u16 holds internal_depth_type=2(16-bit) at [7:4] -> 0x0100. */
    w8(&q, 121); w8(&q, 0);
    w16(&q, p->width); w16(&q, p->height);
    w16(&q, 2u << 7); w8(&q, 0);
    /* Color (sub_id=1, 9 B): u64 = sub_id | RT0.internal_type(2) at bit 6 -> 0x81 */
    w8(&q, 121); w64(&q, 1u | ((uint64_t)2u << 6));
    /* Clear Colors (sub_id=3, 9 B): u64 = 3 | RT<<4 | clear_color<<8 */
    w8(&q, 121); w64(&q, 3u | ((uint64_t)p->clear_color << 8));
    /* ZS Clear Values (sub_id=2, 9 B): b1=2, b2=stencil=0, b3..6=float z=1.0,
     * b7..8=0.  1.0f == 0x3F800000. */
    w8(&q, 121); w8(&q, 2); w8(&q, 0); w32(&q, 0x3f800000u); w16(&q, 0);

    /* TILE_LIST_INITIAL_BLOCK_SIZE (126, 2 B): first_block=0(64B), auto_chained=1 */
    w8(&q, 126); w8(&q, (0u) | (1u << 2));

    /* MULTICORE_RENDERING_TILE_LIST_SET_BASE (123, 5 B): set=0, addr (64B aligned) */
    w8(&q, 123); w32(&q, p->tile_alloc_va);

    /* MULTICORE_RENDERING_SUPERTILE_CFG (122, 9 B):
     * b1/b2 = supertile w/h in tiles - 1 (BIASED);  b3/b4 = frame w/h in supertiles (RAW)
     * b5..8 = frame_w_tiles [0:12] | frame_h_tiles [12:12] | n_bin_tile_lists-1 [29:3] */
    w8(&q, 122);
    w8(&q, V3D_SUPERTILE_T - 1); w8(&q, V3D_SUPERTILE_T - 1);
    w8(&q, stX); w8(&q, stY);
    w32(&q, tilesX | (tilesY << 12));

    /* dummy clear tile: TILE_COORDINATES(124,0,0) END_OF_LOADS(26)
     * STORE_TILE_BUFFER_GENERAL(29, NONE) CLEAR_TILE_BUFFERS(25, all) END_OF_TILE(27) */
    w8(&q, 124); w24(&q, 0);
    w8(&q, 26);
    w8(&q, 29); w8(&q, 8); for (int i = 0; i < 11; i++) w8(&q, 0);  /* buffer=8(NONE) */
    w8(&q, 25); w8(&q, 0x03);                                       /* clear all RT + ZS */
    w8(&q, 27);

    /* second dummy store tile (Random06457 race workaround -- kept verbatim) */
    w8(&q, 124); w24(&q, 0);
    w8(&q, 26);
    w8(&q, 29); w8(&q, 8); for (int i = 0; i < 11; i++) w8(&q, 0);
    w8(&q, 27);

    w8(&q, 19);                 /* FLUSH_VCD_CACHE */

    /* START_ADDRESS_OF_GENERIC_TILE_LIST (20, 9 B): start/end bracket the inline
     * sub-list below; absolute GPU VAs, so patched after emission. */
    w8(&q, 20);
    uint8_t *p20 = q; w32(&q, 0); w32(&q, 0);
    uint32_t sub_start = (uint32_t)(q - buf);
    /* --- generic tile sub-list --- */
    w8(&q, 125);                /* TILE_COORDINATES_IMPLICIT */
    w8(&q, 26);                 /* END_OF_LOADS */
    w8(&q, 56); w8(&q, 0x02);   /* PRIM_LIST_FORMAT: triangles */
    w8(&q, 54); w32(&q, 0);     /* SET_INSTANCEID 0 */
    w8(&q, 21); w8(&q, 0);      /* BRANCH_TO_IMPLICIT_TILE_LIST, set 0 */
    emit_store_rt0(&q, p->fb_stride, p->fb_va, p->rb_swap);
    w8(&q, 27);                 /* END_OF_TILE_MARKER */
    w8(&q, 18);                 /* RETURN_FROM_SUB_LIST */
    uint32_t sub_end = (uint32_t)(q - buf);
    w32(&p20, render_cl_va + sub_start);
    w32(&p20, render_cl_va + sub_end);

    /* SUPERTILE_COORDINATES (23, 3 B each): column-major (col fastest) */
    for (uint32_t row = 0; row < stY; row++)
        for (uint32_t col = 0; col < stX; col++) {
            w8(&q, 23); w8(&q, col); w8(&q, row);
        }

    w8(&q, 13);                 /* END_OF_RENDERING */
    return (int)(q - buf);
}
