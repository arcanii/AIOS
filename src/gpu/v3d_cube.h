#ifndef AIOS_GPU_V3D_CUBE_H
#define AIOS_GPU_V3D_CUBE_H
/*
 * v3d_cube.h -- spinning-cube geometry + per-frame vertex transform (Phase 4a).
 *
 * A cube = 8 corners, 6 faces, 12 triangles, 36 vertices. The A72 transforms the
 * corners each frame (rotate + perspective project to NDC), reusing the borrowed
 * vertex/coord QPU shaders unchanged -- the GPU just applies the viewport. A convex
 * cube needs only backface CULLING (front faces tile the silhouette without overlap),
 * so there is NO depth buffer: positions carry z=0 and culling uses the screen-space
 * winding. Pure FP-free fixed-point math (the root task is FP-free), converting to
 * IEEE754 float bits only for the attribute buffer the GPU reads.
 */
#include <stdint.h>

#define V3D_CUBE_VERTICES  36
#define V3D_QUAD_VERTICES  12    /* 2 triangles x 2 opposite windings (cull diagnostic) */

/* Transform the cube for rotation (ax, ay) in degrees and fill the GPU attribute
 * buffers: pos_out = 36 x float3 (x,y NDC, z=0) as IEEE754 bit patterns (108 u32);
 * col_out = 36 x ubyte4 RGBA (144 bytes). Deterministic, FP-free. */
void v3d_cube_verts(int ax, int ay, uint32_t *pos_out, uint8_t *col_out);

/* Diagnostic geometry: a flat square spun around Y by ay, emitted twice with opposite
 * winding (RED + BLUE). 12 verts (float3 pos z=0, ubyte4 RGBA). With backface culling
 * only one colour shows at a time -- it flips RED<->BLUE each half turn. */
void v3d_quad_verts(int ay, uint32_t *pos_out, uint8_t *col_out);

#endif /* AIOS_GPU_V3D_CUBE_H */
