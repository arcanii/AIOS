/*
 * v3d_host_clgen.c -- host-side driver for the V3D CL emitters (Phase 2 clear +
 * Phase 3 triangle).
 *
 * Compiles src/gpu/v3d_cl.c on the host and emits our control lists + records using
 * the SAME synthetic addresses/scene as the captured golden fixtures, so
 * scripts/v3d_clcheck.py can byte-diff our output against them. Any divergence is a
 * packet-encoding bug in v3d_cl.c. The triangle addresses replicate
 * tools/v3d_ref/gen.cpp buildTriangle() exactly. See project_v3d_phase2/phase3.
 *
 *   usage: v3d_host_clgen <outdir>   (writes clear_*.bin + tri_*.bin)
 */
#include <stdio.h>
#include <stdint.h>
#include "v3d_cl.h"

/* must match tools/v3d_ref/gen.cpp (the golden generator) exactly */
#define GOLDEN_RENDER_CL_VA  0x00210000u

static int dump(const char *dir, const char *name, const uint8_t *buf, int n) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (n < 0) { fprintf(stderr, "emit failed for %s\n", name); return 1; }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    int wrote = (int)fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    if (wrote != n) { fprintf(stderr, "short write to %s\n", name); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <outdir>\n", argv[0]); return 2; }
    const char *d = argv[1];
    uint8_t buf[2048];
    int rc = 0, n;

    /* ---- Phase 2 clear (golden uses rb_swap=1, Random06457's RGB FB order) ---- */
    struct v3d_clear_params cp = {
        .width = 1024, .height = 768, .clear_color = 0xFFFF8000u,
        .fb_va = 0x10000000u, .fb_stride = 4096u, .tile_alloc_va = 0x00110000u,
        .rb_swap = 1,
    };
    n = v3d_build_bin_cl(buf, (int)sizeof buf, &cp);              rc |= dump(d, "clear_bin.bin", buf, n);
    n = v3d_build_render_cl(buf, (int)sizeof buf, GOLDEN_RENDER_CL_VA, &cp); rc |= dump(d, "clear_render.bin", buf, n);

    /* ---- Phase 3 triangle: synthetic addresses replicating gen.cpp buildTriangle ----
     * 500x500; bases IND=0x320000 ST=0x350000 A0=0x330000 A1=0x340000 TALLOC=0x400000
     * RT=0x10100000 ZB=0x10200000. The state buffer holds 256 B default attrs then the
     * three shaders (12/25/18 x u64); the indirect buffer holds the 5+3 uniform floats,
     * then (32-aligned) the GL Shader State Record, the 2 attribute records, then the
     * generic tile list. */
    const uint32_t IND = 0x320000u, ST = 0x350000u, A0 = 0x330000u, A1 = 0x340000u;
    uint32_t default_attr = ST;
    uint32_t frag_code  = ST + 256u;
    uint32_t vtx_code   = frag_code + 12u * 8u;   /* V3D_FRAG_SHADER_WORDS  */
    uint32_t coord_code = vtx_code  + 25u * 8u;   /* V3D_VTX_SHADER_WORDS   */
    uint32_t frag_unif = IND, vtx_unif = IND, coord_unif = IND + 20u;  /* 5 floats then 3 */
    uint32_t record_va = IND + 0x20u;             /* after uniforms, 32-aligned */
    uint32_t tile_list_va = record_va + 36u + 16u + 16u;  /* record + 2 attr records */

    struct v3d_tri_params tp = {
        .width = 500, .height = 500, .clear_color = 0xff101010u, .rb_swap = 1,
        .fb_va = 0x10100000u, .fb_stride = 2000u, .z_va = 0x10200000u,
        .tile_alloc_va = 0x400000u, .shader_record_va = record_va, .nattr = 2,
        .tile_list_va = tile_list_va, .vertex_count = 3,
    };
    n = v3d_build_triangle_bin_cl(buf, (int)sizeof buf, &tp);    rc |= dump(d, "tri_bin.bin", buf, n);
    n = v3d_build_triangle_render_cl(buf, (int)sizeof buf, &tp); rc |= dump(d, "tri_render.bin", buf, n);
    n = v3d_build_triangle_tile_list(buf, (int)sizeof buf, &tp); rc |= dump(d, "tri_tilelist.bin", buf, n);

    struct v3d_shader_record_params sr = {
        .default_attr_va = default_attr, .frag_code_va = frag_code, .frag_unif_va = frag_unif,
        .vtx_code_va = vtx_code, .vtx_unif_va = vtx_unif, .coord_code_va = coord_code,
        .coord_unif_va = coord_unif, .num_fs_varyings = 4,
    };
    n = v3d_build_shader_record(buf, (int)sizeof buf, &sr);      rc |= dump(d, "tri_record.bin", buf, n);

    struct v3d_attr_params pos = {
        .address = A0, .vec_size = 3, .type = 2, .normalized = 0,
        .num_read_vtx = 3, .num_read_coord = 3, .stride = 12, .max_index = 0xFFFFFFu,
    };
    n = v3d_build_attr_record(buf, (int)sizeof buf, &pos);       rc |= dump(d, "tri_attr0.bin", buf, n);
    struct v3d_attr_params col = {
        .address = A1, .vec_size = 0, .type = 4, .normalized = 1,
        .num_read_vtx = 4, .num_read_coord = 0, .stride = 4, .max_index = 0xFFFFFFu,
    };
    n = v3d_build_attr_record(buf, (int)sizeof buf, &col);       rc |= dump(d, "tri_attr1.bin", buf, n);

    return rc;
}
