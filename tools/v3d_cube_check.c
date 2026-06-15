/*
 * v3d_cube_check.c -- host sanity for the Phase 4a cube transform (v3d_cube.c).
 *
 * There is no captured golden for the cube (Random06457's reference was a static
 * triangle), so this is a structural/range check rather than a byte-exact diff: it
 * exercises v3d_cube_verts across rotations and confirms the FP-free transform yields
 * on-screen NDC (|x|,|y| <= 1), z == 0 (cull-only, no depth), and alpha == 0xff for
 * all 36 vertices. Built + run by scripts/v3d_clcheck.py. Exit 0 = OK.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "v3d_cube.h"

int main(void) {
    int bad = 0;
    float gmin = 1e9f, gmax = -1e9f;
    for (int ax = 0; ax < 360; ax += 17) {
        for (int ay = 0; ay < 360; ay += 23) {
            uint32_t pos[V3D_CUBE_VERTICES * 3];
            uint8_t  col[V3D_CUBE_VERTICES * 4];
            memset(pos, 0xAA, sizeof pos);
            memset(col, 0, sizeof col);
            v3d_cube_verts(ax, ay, pos, col);
            for (int v = 0; v < V3D_CUBE_VERTICES; v++) {
                for (int k = 0; k < 2; k++) {           /* x,y NDC */
                    float f; uint32_t u = pos[v * 3 + k];
                    memcpy(&f, &u, 4);
                    if (f < gmin) gmin = f;
                    if (f > gmax) gmax = f;
                    if (f < -1.0f || f > 1.0f) bad++;   /* must stay on screen */
                }
                float z; uint32_t uz = pos[v * 3 + 2];
                memcpy(&z, &uz, 4);
                if (z != 0.0f) bad++;                   /* cull-only: z must be 0 */
                if (col[v * 4 + 3] != 0xff) bad++;      /* opaque */
            }
        }
    }
    printf("cube NDC range [%.3f, %.3f] bad=%d\n", gmin, gmax, bad);
    return bad ? 1 : 0;
}
