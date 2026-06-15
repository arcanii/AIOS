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

/* ============================================================ *
 *  Phase 3 -- rainbow triangle (GL Shader State path) emitters  *
 * ============================================================ */

/* IEEE754 single-precision bit pattern of a signed integer, |v| < 2^24 (exact). No
 * FPU: the root task is FP-free, and host+kernel must agree bit-for-bit. Exposed
 * (v3d_i2f) so v3d.c can compute the shader viewport uniforms the same way. */
uint32_t v3d_i2f(int32_t v) {
    if (v == 0) return 0;
    uint32_t sign = 0, a;
    if (v < 0) { sign = 0x80000000u; a = (uint32_t)(-(int64_t)v); } else a = (uint32_t)v;
    int e = 0;
    while (a < (1u << 23)) { a <<= 1; e--; }    /* put the MSB at bit 23 */
    while (a >= (1u << 24)) { a >>= 1; e++; }   /* (only if |v| >= 2^24; lossy, avoided) */
    uint32_t exp = (uint32_t)(127 + 23 + e);
    return sign | (exp << 23) | (a & 0x7fffffu);
}

/* STORE_TILE_BUFFER_GENERAL (opcode 29, 13 B), general form: buffer/format/addr. */
static void emit_store_tile(uint8_t **q, uint32_t buffer, uint32_t memfmt,
                            uint32_t outfmt, int rb_swap, uint32_t stride, uint32_t addr) {
    w8(q, 29);
    w8(q, (buffer & 0xf) | ((memfmt & 7) << 4));               /* buffer | mem_format | flip_y=0 */
    w16(q, ((outfmt & 0x3f) << 4) | (rb_swap ? (1u << 12) : 0u));
    w24(q, stride << 4);                                       /* height_in_ub_or_stride [4:24] */
    w16(q, 0);                                                 /* height */
    w32(q, addr);
}

/* supertile sizing (Random06457 getSuperTiles): grow sw/sh until the frame is < 256
 * supertiles. Fills *sw,*sh (tiles per supertile) and *fw,*fh (frame in supertiles). */
static void v3d_super_tiles(uint32_t tx, uint32_t ty, uint32_t *sw, uint32_t *sh,
                            uint32_t *fw, uint32_t *fh) {
    *sw = 1; *sh = 1;
    for (;;) {
        *fw = (tx + *sw - 1) / *sw; *fh = (ty + *sh - 1) / *sh;
        if (*fw * *fh < 256) break;
        if (*sw < *sh) (*sw)++; else (*sh)++;
    }
}

int v3d_build_shader_record(uint8_t *buf, int cap, const struct v3d_shader_record_params *p) {
    if (cap < 36) return -1;
    uint8_t *q = buf;
    w8(&q, 0x02);                 /* enable_clipping (bit 1)                          */
    w8(&q, 0x10);                 /* fs_uses_real_pixel_centre_w (bit 4)              */
    w8(&q, 0x04);                 /* disable_implicit_point_line_varyings (bit 2)     */
    w8(&q, p->num_fs_varyings);   /* number_of_varyings_in_fragment_shader            */
    w8(&q, 0x01);                 /* coord output VPM segment size = 1                */
    w8(&q, 0x01);                 /* coord input  VPM segment size = 1                */
    w8(&q, 0x01);                 /* vertex output VPM segment size = 1               */
    w8(&q, 0x01);                 /* vertex input  VPM segment size = 1               */
    w32(&q, p->default_attr_va);
    w32(&q, (p->frag_code_va  & ~7u) | 0x5);   /* code>>3<<3 | 4way(1) final(0) nans(1) */
    w32(&q, p->frag_unif_va);
    w32(&q, (p->vtx_code_va   & ~7u) | 0x7);   /* 4way + final + nans                  */
    w32(&q, p->vtx_unif_va);
    w32(&q, (p->coord_code_va & ~7u) | 0x7);
    w32(&q, p->coord_unif_va);
    return 36;
}

int v3d_build_attr_record(uint8_t *buf, int cap, const struct v3d_attr_params *a) {
    if (cap < 16) return -1;
    uint8_t *q = buf;
    w32(&q, a->address);
    w8(&q, (a->vec_size & 3) | ((a->type & 7) << 2) | ((a->normalized ? 1u : 0u) << 6));
    w8(&q, (a->num_read_coord & 0xf) | ((a->num_read_vtx & 0xf) << 4));
    w16(&q, 0);                   /* instance_divisor */
    w32(&q, a->stride);
    w32(&q, a->max_index);
    return 16;
}

int v3d_build_triangle_bin_cl(uint8_t *buf, int cap, const struct v3d_tri_params *p) {
    if (cap < 122) return -1;
    uint8_t *q = buf;
    uint32_t w = p->width, h = p->height;

    /* TILE_BINNING_MODE_CFG (120): nRT=1, 32bpp, 64B blocks; width-1/height-1 biased */
    w8(&q, 120); w8(&q, 0); w8(&q, 0); w8(&q, 0); w8(&q, 0);
    w16(&q, w - 1); w16(&q, h - 1);
    w8(&q, 19);                            /* FLUSH_VCD_CACHE */
    w8(&q, 92); w32(&q, 0);                /* OCCLUSION_QUERY_COUNTER addr=0 */
    w8(&q, 6);                             /* START_TILE_BINNING */
    /* CLIP_WINDOW (107): left, bottom, width, height (u16) */
    w8(&q, 107); w16(&q, 0); w16(&q, 0); w16(&q, w); w16(&q, h);
    /* CFG_BITS (96): byte0 bits = fwd(0) | rev(1) | cw(2). cull=0 (triangle) => 0x07
     * (fwd+rev+cw: both faces drawn, winding irrelevant). cull=1 (cube) => 0x01 (fwd
     * only = back-face cull, cw=0 => COUNTER-clockwise is front). The Y viewport is
     * flipped, so screen winding is inverted vs object space: cw=1 (0x05) culls the
     * NEAR faces and renders inside-out -- cw=0 culls the far faces (correct winding).
     * byte2/3 = depth_test ALWAYS(7) + early-z; a convex cube needs only culling. */
    w8(&q, 96); w8(&q, p->cull ? 0x01 : 0x07); w8(&q, 0x70); w8(&q, 0x02);
    w8(&q, 104); w32(&q, 0x3f800000u);     /* POINT_SIZE 1.0 */
    w8(&q, 105); w32(&q, 0x3f800000u);     /* LINE_WIDTH 1.0 */
    /* CLIPPER_XY_SCALING (110): (w/2)*256, (h/2)*-256 (f32) */
    w8(&q, 110); w32(&q, v3d_i2f((int32_t)((w / 2) * 256))); w32(&q, v3d_i2f(-(int32_t)((h / 2) * 256)));
    /* CLIPPER_Z_SCALE_AND_OFFSET (111): 0.5, 0.5 */
    w8(&q, 111); w32(&q, 0x3f000000u); w32(&q, 0x3f000000u);
    /* CLIPPER_Z_MIN_MAX (109): 0.0, 1.0 */
    w8(&q, 109); w32(&q, 0x00000000u); w32(&q, 0x3f800000u);
    /* VIEWPORT_OFFSET (108): centre x,y = (w/2)*256, (h/2)*256 (u14.8 in 22 bits); coarse=0 */
    w8(&q, 108);
    w32(&q, ((uint32_t)((w / 2) * 256)) & 0x3fffffu);
    w32(&q, ((uint32_t)((h / 2) * 256)) & 0x3fffffu);
    w8(&q, 87); w32(&q, 0);                /* COLOR_WRITE_MASKS 0 (write all) */
    w8(&q, 86); w32(&q, 0); w32(&q, 0);    /* BLEND_CONSTANT_COLOR 0 */
    w8(&q, 97); w8(&q, 99); w8(&q, 88);    /* ZERO_ALL flat / non-persp / centroid */
    w8(&q, 74); w8(&q, 0);                 /* TRANSFORM_FEEDBACK_SPECS 0 */
    w8(&q, 92); w32(&q, 0);                /* OCCLUSION_QUERY_COUNTER 0 (again) */
    w8(&q, 91); w8(&q, 0x0f); w8(&q, 0x00); w16(&q, 0x3f80); /* SAMPLE_STATE mask=0xf cov=1.0>>16 */
    w8(&q, 71); w8(&q, 0x44);              /* VCM_CACHE_SIZE bin=4 render=4 */
    /* GL_SHADER_STATE (64): record VA (16B-aligned) | nattr in low 4 bits */
    w8(&q, 64); w32(&q, (p->shader_record_va & ~0xfu) | (p->nattr & 0xf));
    /* VERTEX_ARRAY_PRIMS (36): mode=4(triangles), count, first=0 */
    w8(&q, 36); w8(&q, 4); w32(&q, p->vertex_count); w32(&q, 0);
    w8(&q, 4);                             /* FLUSH */
    return (int)(q - buf);                 /* 122 */
}

/* Generic tile list (separate buffer; referenced by render-CL packet 20): the per-tile
 * draw sub-list. 125,26,56,21, STORE RT0, STORE Z, 25, 27, 18. */
int v3d_build_triangle_tile_list(uint8_t *buf, int cap, const struct v3d_tri_params *p) {
    if (cap < 36) return -1;
    uint8_t *q = buf;
    w8(&q, 125);                 /* TILE_COORDINATES_IMPLICIT */
    w8(&q, 26);                  /* END_OF_LOADS */
    w8(&q, 56); w8(&q, 0x02);    /* PRIM_LIST_FORMAT: triangles */
    w8(&q, 21); w8(&q, 0);       /* BRANCH_TO_IMPLICIT_TILE_LIST, set 0 */
    emit_store_tile(&q, 0, 0, 27, p->rb_swap, p->fb_stride, p->fb_va);          /* RT0 RGBA8 RASTER */
    if (!p->skip_z)
        emit_store_tile(&q, 9, 5, 42, 0, (p->height + 15u) / 8u, p->z_va);      /* Z D16 UIF_XOR (cube: skipped) */
    w8(&q, 25); w8(&q, 0x03);    /* CLEAR_TILE_BUFFERS all RT + ZS */
    w8(&q, 27);                  /* END_OF_TILE_MARKER */
    w8(&q, 18);                  /* RETURN_FROM_SUB_LIST */
    return (int)(q - buf);       /* 36 */
}

int v3d_build_triangle_render_cl(uint8_t *buf, int cap, const struct v3d_tri_params *p) {
    uint32_t w = p->width, h = p->height;
    uint32_t tilesX = (w + 63) / 64, tilesY = (h + 63) / 64;
    uint32_t sw, sh, fw, fh;
    v3d_super_tiles(tilesX, tilesY, &sw, &sh, &fw, &fh);
    if (cap < (int)(103u + 3u * fw * fh)) return -1;
    uint8_t *q = buf;

    /* TRMC sub-packets, triangle order: Common -> Clear Colors -> Color -> ZS (LAST). */
    /* Common (sub 0): RAW width/height; internal_depth_type=2 (16-bit) at bit 7. */
    w8(&q, 121); w8(&q, 0); w16(&q, w); w16(&q, h); w16(&q, 2u << 7); w8(&q, 0);
    /* Clear Colors (sub 3): u64 = 3 | RT<<4 | clear_color<<8 */
    w8(&q, 121); w64(&q, 3u | ((uint64_t)p->clear_color << 8));
    /* Color (sub 1): u64 = 1 | RT0.internal_type(2) at bit 6 */
    w8(&q, 121); w64(&q, 1u | ((uint64_t)2u << 6));
    /* ZS Clear Values (sub 2): stencil=0, z=1.0 */
    w8(&q, 121); w8(&q, 2); w8(&q, 0); w32(&q, 0x3f800000u); w16(&q, 0);

    /* TILE_LIST_INITIAL_BLOCK_SIZE (126): first=0(64B), auto_chained=1 */
    w8(&q, 126); w8(&q, (1u << 2));
    /* MULTICORE_RENDERING_TILE_LIST_SET_BASE (123): set=0, addr (64B aligned) */
    w8(&q, 123); w32(&q, p->tile_alloc_va);
    /* MULTICORE_RENDERING_SUPERTILE_CFG (122) */
    w8(&q, 122);
    w8(&q, sw - 1); w8(&q, sh - 1); w8(&q, fw); w8(&q, fh);
    w32(&q, tilesX | (tilesY << 12));

    /* dummy clear tile + second dummy store tile (Random06457 race workaround) */
    w8(&q, 124); w24(&q, 0);
    w8(&q, 26);
    w8(&q, 29); w8(&q, 8); for (int i = 0; i < 11; i++) w8(&q, 0);
    w8(&q, 25); w8(&q, 0x03);
    w8(&q, 27);
    w8(&q, 124); w24(&q, 0);
    w8(&q, 26);
    w8(&q, 29); w8(&q, 8); for (int i = 0; i < 11; i++) w8(&q, 0);
    w8(&q, 27);

    w8(&q, 19);                  /* FLUSH_VCD_CACHE */

    /* START_ADDRESS_OF_GENERIC_TILE_LIST (20): the SEPARATE tile-list buffer. Its
     * length is 36 B with the Z store, 23 B without (cube). Must match the tile-list
     * emitter exactly so packet 20's end address brackets the sub-list. */
    uint32_t tlist_len = p->skip_z ? 23u : 36u;
    w8(&q, 20); w32(&q, p->tile_list_va); w32(&q, p->tile_list_va + tlist_len);

    /* SUPERTILE_COORDINATES (23) per supertile -- row-major (col fastest) */
    for (uint32_t row = 0; row < fh; row++)
        for (uint32_t col = 0; col < fw; col++) { w8(&q, 23); w8(&q, col); w8(&q, row); }

    w8(&q, 13);                  /* END_OF_RENDERING */
    return (int)(q - buf);
}
