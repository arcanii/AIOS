#ifndef AIOS_GPU_V3D_SHADERS_H
#define AIOS_GPU_V3D_SHADERS_H
/*
 * v3d_shaders.h -- the three V3D 4.2 QPU shader blobs + triangle geometry for the
 * Phase 3 rainbow triangle (GL Shader State path; design sec 3 / sec 8 Phase 3).
 *
 * The blobs are borrowed VERBATIM from Random06457/rpi4-gpu-bare-metal-examples
 * gl_rainbow_triangle.cpp (MIT -- see tools/v3d_ref/ref_src/LICENSE). v3d 4.2 has no
 * NV-shader shortcut, so a draw needs all three QPU programs: a coordinate shader
 * (positions for the binner), a vertex shader (positions + varyings for the RCL), and
 * a fragment shader (per-pixel color). They are loaded into a GPU BO and referenced by
 * code address (>>3) from the GL Shader State Record. We do NOT assemble QPU code --
 * these are the proven blobs, treated as opaque (the host golden-CL gate proves the CL
 * bytes; the blobs are validated only by the triangle actually rendering on HW).
 *
 * Geometry: one screen-space triangle, per-vertex RGB (red/green/blue corners) so the
 * rainbow interpolation is visually obvious and the pixel probe can tell center (mixed)
 * from corners (clear color).
 */
#include <stdint.h>

/* Fragment shader: 12 x 64-bit instructions. */
static const uint64_t v3d_frag_shader[] = {
    0x3D103186BB800000ull, 0x54003046BBC00000ull, 0x3D10A18605829000ull,
    0x540030C6BBC80000ull, 0x3D1121850582B000ull, 0x54003006BBD00000ull,
    0x3D30618405828000ull, 0x54203086BBC40000ull, 0x3C0021830582A000ull,
    0x3C2031873583E185ull, 0x3C0031873583E103ull, 0x3C003186BB800000ull,
};

/* Vertex shader: 25 x 64-bit instructions (positions + varyings for the RCL). */
static const uint64_t v3d_vtx_shader[] = {
    0x3DE02187BC807000ull, 0x3DE02189BC807001ull, 0x3DE0218ABC807002ull,
    0x3DE02183BC807003ull, 0x3D823186BB800000ull, 0x3DE02184BC807004ull,
    0x3C403186BB800000ull, 0x55E02005BCB871C5ull, 0x3DE02186BC807006ull,
    0x3C403181F5818000ull, 0x3DE02180F88370C4ull, 0x54403086BBB80240ull,
    0x3DE02180F8837105ull, 0x3DE02180F8837146ull, 0x54403003F5B9A280ull,
    0x3DE02180F8837187ull, 0x3DE02180F880F000ull, 0x3C00318405828000ull,
    0x3DE02180F881F001ull, 0x3DE02180F8827002ull, 0x3DE02180F8837203ull,
    0x3C003186BB816000ull, 0x3C203186BB800000ull, 0x3C003186BB800000ull,
    0x3C003186BB800000ull,
};

/* Coordinate shader: 18 x 64-bit instructions (positions for the binner). */
static const uint64_t v3d_coord_shader[] = {
    0x3DE02184BC807000ull, 0x3DE02185BC807001ull, 0x3DE02183BC807002ull,
    0x3DE02180F8837100ull, 0x3DE02180F8837141ull, 0x3C403186BB800000ull,
    0x3DE02180F88370C2ull, 0x3DE02180F882F003ull, 0x3C403186BB800000ull,
    0x54403006BBB80100ull, 0x54003081F5B98140ull, 0x3DE02180F880F004ull,
    0x3C003183F581A000ull, 0x3DE02180F881F005ull, 0x3C003186BB816000ull,
    0x3C203186BB800000ull, 0x3C003186BB800000ull, 0x3C003186BB800000ull,
};

/* Position attribute: 3 vertices x float3 (clip-space xy, z=0). */
static const float v3d_tri_pos[] = {
    -1.f, -1.f, 0.f,
     1.f, -1.f, 0.f,
     0.f,  1.f, 0.f,
};

/* Color attribute: 3 vertices x normalized ubyte4 RGBA (red, green, blue). */
static const uint8_t v3d_tri_color[] = {
    0xFF, 0x00, 0x00, 0xFF,
    0x00, 0xFF, 0x00, 0xFF,
    0x00, 0x00, 0xFF, 0xFF,
};

#define V3D_FRAG_SHADER_WORDS   (sizeof(v3d_frag_shader)  / sizeof(uint64_t))
#define V3D_VTX_SHADER_WORDS    (sizeof(v3d_vtx_shader)   / sizeof(uint64_t))
#define V3D_COORD_SHADER_WORDS  (sizeof(v3d_coord_shader) / sizeof(uint64_t))
#define V3D_TRI_VERTICES        3

#endif /* AIOS_GPU_V3D_SHADERS_H */
