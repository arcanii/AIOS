/*
 * v3d_cube.c -- spinning-cube geometry + per-frame vertex transform (Phase 4a).
 *
 * FP-free: integer Bhaskara-I sine (Q10) + fixed-point rotate/project, converting to
 * IEEE754 float bits at the end (v3d_i2f, exponent-shifted for the fractional divide).
 * No MMIO -- compiles host + kernel. See v3d_cube.h.
 */
#include "v3d_cube.h"
#include "v3d_cl.h"      /* v3d_i2f */

/* Q10 Bhaskara-I sine/cosine (same approximation as the CPU cube demo). */
static int cube_isin(int a) {
    a %= 360; if (a < 0) a += 360;
    int s = 1; if (a > 180) { a -= 180; s = -1; }
    int t = a * (180 - a);
    return s * (4 * t * 1024) / (40500 - t);
}
static int cube_icos(int a) { return cube_isin(a + 90); }

/* float(v / 2^frac) without an FPU: v3d_i2f gives float(v); subtracting frac from the
 * exponent field divides by 2^frac (sign bit and mantissa untouched). v small enough
 * that no exponent underflow occurs for our NDC range. */
static uint32_t fx2f(int32_t v, int frac) {
    uint32_t f = v3d_i2f(v);
    return f ? (f - ((uint32_t)frac << 23)) : 0u;
}

/* 8 corners (unit cube). */
static const int CUBE_CV[8][3] = {
    {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
    {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1},
};
/* 6 faces as quads of corner indices (consistent winding; cull direction tuned on HW). */
static const int CUBE_FACE[6][4] = {
    {4,5,6,7},  /* +Z front  */
    {1,0,3,2},  /* -Z back   */
    {5,1,2,6},  /* +X right  */
    {0,4,7,3},  /* -X left   */
    {3,2,6,7},  /* +Y top    */
    {0,1,5,4},  /* -Y bottom */
};
/* per-face RGB so the six faces are visually distinct. */
static const uint8_t CUBE_COL[6][3] = {
    {0xff,0x30,0x30},  /* red    */
    {0x30,0xff,0x30},  /* green  */
    {0x30,0x30,0xff},  /* blue   */
    {0xff,0xff,0x30},  /* yellow */
    {0x30,0xff,0xff},  /* cyan   */
    {0xff,0x30,0xff},  /* magenta*/
};

#define CUBE_FOCAL_Q16  60000   /* ~0.92; perspective focal -- tune for on-screen size */
#define CUBE_DIST_Q10   (3 * 1024)

void v3d_cube_verts(int ax, int ay, uint32_t *pos_out, uint8_t *col_out) {
    int sa = cube_isin(ay), ca = cube_icos(ay);   /* yaw   (around Y) */
    int sb = cube_isin(ax), cb = cube_icos(ax);   /* pitch (around X) */

    uint32_t ndc[8][2];   /* IEEE754 bits of NDC x,y per corner */
    for (int i = 0; i < 8; i++) {
        int X = CUBE_CV[i][0] * 1024, Y = CUBE_CV[i][1] * 1024, Z = CUBE_CV[i][2] * 1024;
        int Xr  = (X * ca + Z * sa) >> 10;
        int Zr  = (-X * sa + Z * ca) >> 10;
        int Yr  = (Y * cb - Zr * sb) >> 10;
        int Zr2 = (Y * sb + Zr * cb) >> 10;
        int zc  = Zr2 + CUBE_DIST_Q10; if (zc < 16) zc = 16;
        /* NDC = coord * focal / zc ; Xr/zc is unitless (both Q10), * focal_q16 -> Q16. */
        int ndcx = (int)(((long long)Xr * CUBE_FOCAL_Q16) / zc);
        int ndcy = (int)(((long long)Yr * CUBE_FOCAL_Q16) / zc);
        ndc[i][0] = fx2f(ndcx, 16);
        ndc[i][1] = fx2f(ndcy, 16);
    }

    int v = 0;
    for (int f = 0; f < 6; f++) {
        int q0 = CUBE_FACE[f][0], q1 = CUBE_FACE[f][1], q2 = CUBE_FACE[f][2], q3 = CUBE_FACE[f][3];
        int tri[6] = { q0, q1, q2,  q0, q2, q3 };   /* quad -> 2 triangles */
        for (int t = 0; t < 6; t++) {
            int ci = tri[t];
            pos_out[v * 3 + 0] = ndc[ci][0];
            pos_out[v * 3 + 1] = ndc[ci][1];
            pos_out[v * 3 + 2] = 0u;                /* z=0: cull uses xy winding */
            col_out[v * 4 + 0] = CUBE_COL[f][0];
            col_out[v * 4 + 1] = CUBE_COL[f][1];
            col_out[v * 4 + 2] = CUBE_COL[f][2];
            col_out[v * 4 + 3] = 0xff;
            v++;
        }
    }
}
