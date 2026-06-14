/*
 * v3d_host_clgen.c -- host-side driver for the V3D Phase 2 CL emitters.
 *
 * Compiles src/gpu/v3d_cl.c on the host and emits our bin + render clear control
 * lists using the SAME synthetic addresses/scene as the captured golden fixture,
 * so scripts/v3d_clcheck.py can byte-diff our output against it. Any divergence
 * is a packet-encoding bug in v3d_cl.c. See project_v3d_phase2.
 *
 *   usage: v3d_host_clgen <out_bin> <out_render>
 */
#include <stdio.h>
#include <stdint.h>
#include "v3d_cl.h"

/* must match /tmp/v3d_ref/gen.cpp (the golden generator) exactly */
#define GOLDEN_RENDER_CL_VA  0x00210000u

static int dump(const char *path, const uint8_t *buf, int n) {
    if (n < 0) { fprintf(stderr, "emit failed for %s\n", path); return 1; }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    int wrote = (int)fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    if (wrote != n) { fprintf(stderr, "short write to %s\n", path); return 1; }
    printf("wrote %s (%d bytes)\n", path, n);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <out_bin> <out_render>\n", argv[0]);
        return 2;
    }
    struct v3d_clear_params p = {
        .width         = 1024,
        .height        = 768,
        .clear_color   = 0xFFFF8000u,   /* orange, BGR; R/B-asymmetric on purpose */
        .fb_va         = 0x10000000u,
        .fb_stride     = 4096u,
        .tile_alloc_va = 0x00110000u,
    };
    uint8_t bin[256], rend[1024];
    int bn = v3d_build_bin_cl(bin, (int)sizeof bin, &p);
    int rn = v3d_build_render_cl(rend, (int)sizeof rend, GOLDEN_RENDER_CL_VA, &p);
    return dump(argv[1], bin, bn) | dump(argv[2], rend, rn);
}
