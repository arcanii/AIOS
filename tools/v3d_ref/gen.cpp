// Host harness: byte-exact golden V3D 4.2 CLEAR control lists (+ triangle cross-check).
// Compiles Random06457's UNMODIFIED encoding headers (v3d_cl.hpp / cl_emitter.hpp)
// host-side on macOS clang. ARM_TO_BUS and all MMIO/hardware bits are stubbed so the
// emitted BYTES are identical to what the kernel would place in memory; only the
// absolute buffer addresses are synthetic (chosen by us, documented below).
//
// Build: see build.sh
//
// IMPORTANT: we do NOT modify any header under /tmp/random06457. The CLEmitter's
// paddr() calls ARM_TO_BUS(m_buff + off). Random06457's ARM_TO_BUS truncates the
// heap pointer to u32, which on a 64-bit host yields a non-deterministic low-32 of a
// real heap address. To make the SELF/CL addresses deterministic we override
// ARM_TO_BUS at compile time via a macro that maps a known set of CLEmitter buffers
// onto synthetic bases. We do this WITHOUT editing their files: we predefine the
// ARM_TO_BUS macro before including their header so their template definition is
// skipped (their header guards the symbol name, not the macro). See stub below.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cassert>   // their cl_emitter.hpp / v3d_cl.hpp use assert() (kernel: panic.h)
#include <vector>
#include <string>
#include <map>

// ---- types the headers expect (mirror types.hpp so we don't need it first) ----
// We include their real types.hpp; nothing to stub there.

// ---------------------------------------------------------------------------
// STUB 1: ARM_TO_BUS / bus addressing.
// We must intercept ARM_TO_BUS *before* v3d_memory.hpp defines it. v3d_memory.hpp
// defines a templated `static constexpr u32 ARM_TO_BUS(T*)`. We cannot macro-rename
// a templated function cleanly, so instead we let their definition compile but make
// it deterministic by REGISTERING every CLEmitter/V3DBuffer base pointer with a
// synthetic GPU/bus address and having a global translation map. We achieve this by
// providing our OWN v3d_memory.hpp shim on the include path that the kernel header
// would have provided. Their cl_emitter.hpp does `#include "device/v3d/v3d_memory.hpp"`,
// so by putting OUR include dir first we substitute it. (We do not touch their tree.)
// ---------------------------------------------------------------------------

// Pull in their headers. Our -I order ensures our shim v3d_memory.hpp wins.
#include "types.hpp"
#include "bit_utils.hpp"
#include "device/v3d/v3d_memory.hpp"   // <-- OUR shim (see shim/)
#include "kernel/cl_emitter.hpp"
#include "device/v3d/v3d_cl.hpp"

#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Synthetic address allocator: g_busmap is defined in our v3d_memory.hpp shim.
// We register each buffer's base with a fixed synthetic bus address here so the
// emitted CLs are deterministic. The shim's ARM_TO_BUS routes through it.
// ---------------------------------------------------------------------------

static void hexdump_file(const char* path, const u8* p, size_t n)
{
    FILE* f = fopen(path, "wb");
    fwrite(p, 1, n, f);
    fclose(f);
}

static void dumphex(const char* label, const u8* p, size_t n)
{
    printf("=== %s (%zu bytes) ===\n", label, n);
    for (size_t i = 0; i < n; i += 16) {
        printf("%04zx:", i);
        for (size_t j = 0; j < 16 && i + j < n; j++)
            printf(" %02x", p[i + j]);
        printf("\n");
    }
    printf("\n");
}

static constexpr u32 divRoundUp(u32 a, u32 b) { return (a + b - 1) / b; }

// =========================================================================
//                              THE CLEAR
// =========================================================================
static void buildClear()
{
    // ---- scene params (from the design, exact) ----
    const u32 width  = 1024;
    const u32 height = 768;
    const u32 tile_w = 64, tile_h = 64;
    const u32 tilesX = divRoundUp(width, tile_w);   // 16
    const u32 tilesY = divRoundUp(height, tile_h);  // 12
    // supertiles 4x4 tiles -> 4x3 grid
    const u32 supertile_w = 4, supertile_h = 4;
    const u32 frame_w_supertile = divRoundUp(tilesX, supertile_w); // 4
    const u32 frame_h_supertile = divRoundUp(tilesY, supertile_h); // 3
    const u32 clear_color = 0xFFFF8000u; // ORANGE (A=FF R=FF G=80 B=00 in ARGB sense)

    // ---- synthetic GPU/bus addresses (documented) ----
    const u32 TILE_STATE_ADDR = 0x00100000; // tile state data array
    const u32 TILE_ALLOC_ADDR = 0x00110000; // tile allocation (PTB) base, 64-aligned
    const u32 FB_RT_ADDR      = 0x10000000; // render target / framebuffer
    const u32 BCL_ADDR        = 0x00200000; // bin CL buffer base
    const u32 RCL_ADDR        = 0x00210000; // render CL buffer base
    (void)TILE_STATE_ADDR;

    CLEmitter bcl(0x1000, 4096);
    CLEmitter rcl(0x1000, 4096);

    // register synthetic bases (CLEmitter exposes the raw base via startPtr<u8>()).
    // Use the TRUE capacity as the span so distinct heap allocations never alias.
    g_busmap.reg(bcl.startPtr<u8>(), BCL_ADDR, bcl.capacity());
    g_busmap.reg(rcl.startPtr<u8>(), RCL_ADDR, rcl.capacity());

    // =================== BIN CL ===================
    // TILE_BINNING_MODE_CFG(120) -> FLUSH_VCD_CACHE(19) ->
    // OCCLUSION_QUERY_COUNTER(92,0) -> START_TILE_BINNING(6) -> FLUSH(4)
    {
        // initAllocBlockSize=0 (64B), allocBlockSize=0 (64B), targetCount=1,
        // maxBpp=0 (32bpp), msaa=false, doubleBuffer=false, w, h.
        bcl << TileBinningModeCfg(0, 0, 1, 0, false, false, width, height);
        bcl << OP_FLUSH_VCD_CACHE;
        bcl << OcclusionQueryCounter(0);
        bcl << OP_START_TILE_BINNING;
        bcl << OP_FLUSH;
    }

    // =================== RENDER CL ===================
    {
        // ---- TILE_RENDERING_MODE_CFG(121): Common -> Color -> ClearColors -> ZS last
        // Common: targetCount=1, w, h, maxBpp=0, msaa=false, doubleBuffer=false,
        //   zTestDir=0, earlyZDisable=false, internalDepthType=2 (16), earlyStencilClear=false
        rcl << TileRenderingModeCfgCommon(1, width, height, 0, false, false, 0,
                                          false, 2, false);
        // Color: RT0 bpp=0 (32bpp), type=2, clamp=0; RT1..3 unused (0)
        rcl << TileRenderingModeCfgColor(0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        // Clear Colors part1: RT0, low32 = clear_color, next24 = 0
        rcl << TileRenderingModeCfgClearColorsPart1(0, clear_color, 0x000000);
        // ZS Clear Values LAST: stencil=0, z=1.0
        rcl << TileRenderingModeCfgZSClearValues(0, 1.0f);

        // ---- 126: TILE_LIST_INITIAL_BLOCK_SIZE. blockSize enum MUST match 120.
        // 120 used tile_allocation_initial_block_size=0 (64B) -> here size=0 (64B).
        rcl << TileListInitialBlockSize(0, true);

        // ---- 123: MULTICORE_RENDERING_TILE_LIST_SET_BASE (tile-alloc base)
        rcl << MulticoreRenderingTileListSetBase(TILE_ALLOC_ADDR);

        // ---- 122: MULTICORE_RENDERING_SUPERTILE_CFG (4x4 tiles -> 4x3 grid)
        // args: superTileW, superTileH, frameWSupertile, frameHSupertile,
        //       frameWTile, frameHTile, binTileListCount, rasterOrder, multicoreEnabled
        rcl << MulticoreRenderingSupertileCfg(
            supertile_w, supertile_h, frame_w_supertile, frame_h_supertile,
            tilesX, tilesY, 1, false, false);

        // ---- dummy clear tile {124, 26, 29(STORE NONE), 25, 27}
        rcl << TileCoordinates(0, 0);            // 124
        rcl << OP_END_OF_LOADS;                  // 26
        rcl << StoreTileBufferGeneral();         // 29, STORE NONE
        rcl << ClearTileBuffers(true, true);     // 25
        rcl << OP_END_OF_TILE_MARKER;            // 27

        // ---- SECOND dummy store tile (Random06457 race workaround) {124,26,29,27}
        rcl << TileCoordinates(0, 0);            // 124
        rcl << OP_END_OF_LOADS;                  // 26
        rcl << StoreTileBufferGeneral();         // 29, STORE NONE
        rcl << OP_END_OF_TILE_MARKER;            // 27

        rcl << OP_FLUSH_VCD_CACHE;               // 19 (mirrors triangle rcl)

        // ---- generic tile list registered via 20 (StartAddressOfGenericTileList)
        // We emit the generic list INLINE into the rcl buffer (a sub-list), bracketed
        // by 20 (start/end). Random06457 puts it in a separate "indirect" buffer; for
        // a self-contained golden we inline it and point 20 at its [start,end).
        // Sequence: {125, 26, PRIM_LIST_FORMAT(56,triangles), SET_INSTANCEID(54,0),
        //            21, 29(store color RT), 27, 18}
        // The design lists 21 then 29; 21 = BRANCH_TO_IMPLICIT_TILE_LIST.
        {
            // Emit packet 20 with a placeholder, then emit the generic sub-list
            // inline, then patch packet 20 with the real [start,end) bus range.
            size_t pkt20_off = rcl.size();
            rcl << StartAddressOfGenericTileList(0, 0);

            u32 generic_begin = rcl.paddr();      // bus addr of first sub-list byte
            rcl << OP_TILE_COORDINATES_IMPLICIT;  // 125
            rcl << OP_END_OF_LOADS;               // 26
            rcl << PrimListFormat(LIST_TRIANGLES, false); // 56
            rcl << SetInstanceId(0);              // 54
            rcl << BranchToImplicitTileList(0);   // 21
            // 29: store color RT0 to FB. BGR memory order => r_b_swap=true,
            //   output format RGBA8, tiling RASTER, stride = width*4.
            rcl << StoreTileBufferGeneral(
                BUFFER_RENDER_TARGET_0, V3D_TILING_RASTER, false,
                V3D_DITHER_MODE_NONE, V3D_DECIMATE_MODE_SAMPLE_0,
                V3D_OUTPUT_IMAGE_FORMAT_RGBA8, false, false, true,
                width * 4, 0, FB_RT_ADDR);        // 29
            rcl << OP_END_OF_TILE_MARKER;         // 27
            rcl << OP_RETURN_FROM_SUB_LIST;       // 18
            u32 generic_end = rcl.paddr();        // bus addr just past sub-list

            // patch packet 20 with real [start,end)
            StartAddressOfGenericTileList fixed(generic_begin, generic_end);
            std::memcpy(rcl.startPtr<u8>() + pkt20_off, &fixed, sizeof(fixed));
        }

        // ---- 12x SUPERTILE_COORDINATES(23) : 4 wide x 3 tall
        for (u32 y = 0; y < frame_h_supertile; y++)
            for (u32 x = 0; x < frame_w_supertile; x++)
                rcl << SupertileCoorinates((u8)x, (u8)y);

        // ---- END_OF_RENDERING(13)
        rcl << OP_END_OF_RENDERING;
    }

    hexdump_file("golden_bin.bin", bcl.startPtr<u8>(), bcl.size());
    hexdump_file("golden_render.bin", rcl.startPtr<u8>(), rcl.size());
    dumphex("golden_bin.bin", bcl.startPtr<u8>(), bcl.size());
    dumphex("golden_render.bin", rcl.startPtr<u8>(), rcl.size());
}

// =========================================================================
//          TRIANGLE cross-check (Random06457's UNMODIFIED sequence)
// =========================================================================
// QPU blobs (verbatim from gl_rainbow_triangle.cpp).
static u64 g_frag_shader_buff[] = {
    0x3D103186BB800000ull,0x54003046BBC00000ull,0x3D10A18605829000ull,
    0x540030C6BBC80000ull,0x3D1121850582B000ull,0x54003006BBD00000ull,
    0x3D30618405828000ull,0x54203086BBC40000ull,0x3C0021830582A000ull,
    0x3C2031873583E185ull,0x3C0031873583E103ull,0x3C003186BB800000ull,
};
static u64 g_vtx_shader_buff[] = {
    0x3DE02187BC807000ull,0x3DE02189BC807001ull,0x3DE0218ABC807002ull,
    0x3DE02183BC807003ull,0x3D823186BB800000ull,0x3DE02184BC807004ull,
    0x3C403186BB800000ull,0x55E02005BCB871C5ull,0x3DE02186BC807006ull,
    0x3C403181F5818000ull,0x3DE02180F88370C4ull,0x54403086BBB80240ull,
    0x3DE02180F8837105ull,0x3DE02180F8837146ull,0x54403003F5B9A280ull,
    0x3DE02180F8837187ull,0x3DE02180F880F000ull,0x3C00318405828000ull,
    0x3DE02180F881F001ull,0x3DE02180F8827002ull,0x3DE02180F8837203ull,
    0x3C003186BB816000ull,0x3C203186BB800000ull,0x3C003186BB800000ull,
    0x3C003186BB800000ull,
};
static u64 g_coord_shader_buff[] = {
    0x3DE02184BC807000ull,0x3DE02185BC807001ull,0x3DE02183BC807002ull,
    0x3DE02180F8837100ull,0x3DE02180F8837141ull,0x3C403186BB800000ull,
    0x3DE02180F88370C2ull,0x3DE02180F882F003ull,0x3C403186BB800000ull,
    0x54403006BBB80100ull,0x54003081F5B98140ull,0x3DE02180F880F004ull,
    0x3C003183F581A000ull,0x3DE02180F881F005ull,0x3C003186BB816000ull,
    0x3C203186BB800000ull,0x3C003186BB800000ull,0x3C003186BB800000ull,
};
static f32 g_pos_attr_buff[] = { -1.f,-1.f,0.f, 1.f,-1.f,0.f, 0.f,1.f,0.f };
static u8  g_color_attr_buff[] = {0xFF,0x00,0x00,0xFF, 0x00,0xFF,0x00,0xFF, 0x00,0x00,0xFF,0xFF};

static void getSuperTiles(u32& sw, u32& sh, u32& fw, u32& fh, u32 tx, u32 ty)
{
    static constexpr u32 max_supertiles = 256;
    sw = 1; sh = 1;
    while (true) {
        fw = divRoundUp(tx, sw); fh = divRoundUp(ty, sh);
        if (fw * fh < max_supertiles) break;
        if (sw < sh) sw++; else sh++;
    }
}

static void buildTriangle()
{
    size_t width = 500, height = 500;
    CLEmitter bcl, rcl, indirect, attr0, attr1, state;

    // synthetic bases for each buffer
    const u32 BCL=0x00300000, RCL=0x00310000, IND=0x00320000,
              A0=0x00330000, A1=0x00340000, ST=0x00350000,
              RT=0x10100000, ZB=0x10200000, TALLOC=0x00400000, TSTATE=0x00500000;
    g_busmap.reg(bcl.startPtr<u8>(), BCL, bcl.capacity());
    g_busmap.reg(rcl.startPtr<u8>(), RCL, rcl.capacity());
    g_busmap.reg(indirect.startPtr<u8>(), IND, indirect.capacity());
    g_busmap.reg(attr0.startPtr<u8>(), A0, attr0.capacity());
    g_busmap.reg(attr1.startPtr<u8>(), A1, attr1.capacity());
    g_busmap.reg(state.startPtr<u8>(), ST, state.capacity());

    u32 pos_attr_addr = attr0.paddr();
    attr0 << g_pos_attr_buff;
    u32 colorAttrAddr = attr1.paddr();
    attr1 << g_color_attr_buff;

    u32 default_attr_addr = state.paddr();
    static constexpr size_t V3D_MAX_VS_INPUTS = 64;
    for (size_t i = 0; i < V3D_MAX_VS_INPUTS / 4; i++)
        state << 0.0f << 0.0f << 0.0f << 1.0f;
    u32 frag_shader_addr = state.paddr();  state << g_frag_shader_buff;
    u32 vtx_shader_addr  = state.paddr();  state << g_vtx_shader_buff;
    u32 coord_shader_addr= state.paddr();  state << g_coord_shader_buff;

    u32 frag_unif_addr = indirect.paddr();
    u32 vtx_unif_addr = indirect.paddr();
    indirect << 1.0f;
    indirect << (float)((width / 2) * 256.f);
    indirect << (float)((height / 2) * -256.f);
    indirect << 0.5f;
    indirect << 0.5f;
    u32 coord_unif_addr = indirect.paddr();
    indirect << 1.0f;
    indirect << (float)((width / 2) * 256.f);
    indirect << (float)((height / 2) * -256.f);
    indirect.align(32);

    u32 shader_state_record_addr = indirect.paddr();
    GLShaderStateRecord shader{};
    shader.enable_clipping = true;
    shader.fragment_shader_uses_real_pixel_centre_w_in_addition_to_centroid_w2 = true;
    shader.disable_implicit_point_line_varyings = true;
    shader.number_of_varyings_in_fragment_shader = 4;
    shader.coordinate_shader_output_vpm_segment_size = 1;
    shader.coordinate_shader_input_vpm_segment_size = 1;
    shader.vertex_shader_output_vpm_segment_size = 1;
    shader.vertex_shader_input_vpm_segment_size = 1;
    shader.address_of_default_attribute_values = default_attr_addr;
    shader.fragment_shader_code_address = frag_shader_addr >> 3;
    shader.fragment_shader_uniforms_address = frag_unif_addr;
    shader.fragment_shader_4_way_threadable = true;
    shader.fragment_shader_start_in_final_thread_section = false;
    shader.fragment_shader_propagate_nans = true;
    shader.vertex_shader_code_address = vtx_shader_addr >> 3;
    shader.vertex_shader_uniforms_address = vtx_unif_addr;
    shader.vertex_shader_4_way_threadable = true;
    shader.vertex_shader_start_in_final_thread_section = true;
    shader.vertex_shader_propagate_nans = true;
    shader.coordinate_shader_code_address = coord_shader_addr >> 3;
    shader.coordinate_shader_uniforms_address = coord_unif_addr;
    shader.coordinate_shader_4_way_threadable = true;
    shader.coordinate_shader_start_in_final_thread_section = true;
    shader.coordinate_shader_propagate_nans = true;
    indirect << shader;

    size_t attr_count = 0;
    GlShaderStateAttributeRecord pos_attr{};
    pos_attr.address = pos_attr_addr;
    pos_attr.number_of_values_read_by_vertex_shader = 3;
    pos_attr.number_of_values_read_by_coordinate_shader = 3;
    pos_attr.instance_divisor = 0;
    pos_attr.stride = 3 * sizeof(f32);
    pos_attr.maximum_index = 0xFFFFFF;
    pos_attr.vec_size = 3;
    pos_attr.type = 2;
    indirect << pos_attr;
    attr_count++;
    GlShaderStateAttributeRecord color_attr{};
    color_attr.address = colorAttrAddr;
    color_attr.number_of_values_read_by_vertex_shader = 4;
    color_attr.number_of_values_read_by_coordinate_shader = 0;
    color_attr.instance_divisor = 0;
    color_attr.stride = 4 * sizeof(u8);
    color_attr.maximum_index = 0xFFFFFF;
    color_attr.type = 4;
    color_attr.normalized_int_type = true;
    indirect << color_attr;
    attr_count++;

    // render target / z buffer addresses are synthetic; no host alloc needed.
    u32 render_target_paddr = RT;
    u32 z_buffer_paddr = ZB;
    size_t tile_width = 64, tile_height = 64;
    size_t vertex_count = sizeof(g_pos_attr_buff) / (sizeof(f32) * 3);
    size_t tilesX = divRoundUp(width, tile_width);
    size_t tilesY = divRoundUp(height, tile_height);
    u32 tile_alloc_paddr = TALLOC;
    (void)TSTATE;

    // BCL
    {
        bcl << TileBinningModeCfg(0, 0, 1, 0, false, false, width, height);
        bcl << OP_FLUSH_VCD_CACHE;
        bcl << OcclusionQueryCounter(0);
        bcl << OP_START_TILE_BINNING;
        bcl << ClipWindow(0, 0, width, height);
        bcl << CfgBits(true, true, true, false, 0, 0, false, 7, false, false, true, false, false, false, false);
        bcl << PointSize(1.0f);
        bcl << LineWidth(1.0f);
        bcl << ClipperXYScaling((float)((width / 2) * 256.f), (float)((height / 2) * -256.f));
        bcl << ClipperZScaleAndOffset(.5f, .5f);
        bcl << CLipperZMinMaxClippingPlanes(0.f, 1.f);
        bcl << ViewportOffset((float)(width / 2), (float)(height / 2), 0, 0);
        bcl << ColorWriteMasks(0);
        bcl << BlendConstantColor(0, 0, 0, 0);
        bcl << OP_ZERO_ALL_FLAT_SHADE_FLAGS;
        bcl << OP_ZERO_ALL_NON_PERSPECTIVE_FLAGS;
        bcl << OP_ZERO_ALL_CENTROID_FLAGS;
        bcl << TransformFeedbackSpecs(0, false);
        bcl << OcclusionQueryCounter(0);
        bcl << SampleState(0xF, 1.0f);
        bcl << VcmCacheSize(4, 4);
        bcl << GlShaderState(shader_state_record_addr, attr_count);
        bcl << VertexArrayPrims(4, vertex_count, 0);
        bcl << OP_FLUSH;
    }
    // RCL
    {
        rcl << TileRenderingModeCfgCommon(1, width, height, 0, false, false, 0, false, 2, false);
        rcl << TileRenderingModeCfgClearColorsPart1(0, 0xFF101010, 0x000000);
        rcl << TileRenderingModeCfgColor(0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        rcl << TileRenderingModeCfgZSClearValues(0, 1.0f);
        rcl << TileListInitialBlockSize(0, true);
        u32 sw, sh, fw, fh;
        getSuperTiles(sw, sh, fw, fh, tilesX, tilesY);
        {
            size_t layer_idx = 0;
            u32 tile_alloc_off = layer_idx * tilesX * tilesY * 64;
            rcl << MulticoreRenderingTileListSetBase(tile_alloc_paddr + tile_alloc_off);
            rcl << MulticoreRenderingSupertileCfg(sw, sh, fw, fh, tilesX, tilesY, 1, false, false);
            rcl << TileCoordinates(0, 0);
            rcl << OP_END_OF_LOADS;
            rcl << StoreTileBufferGeneral();
            rcl << ClearTileBuffers(true, true);
            rcl << OP_END_OF_TILE_MARKER;
            rcl << TileCoordinates(0, 0);
            rcl << OP_END_OF_LOADS;
            rcl << StoreTileBufferGeneral();
            rcl << OP_END_OF_TILE_MARKER;
            rcl << OP_FLUSH_VCD_CACHE;
            {
                u32 indirect_addr = indirect.paddr();
                indirect << OP_TILE_COORDINATES_IMPLICIT;
                indirect << OP_END_OF_LOADS;
                indirect << PrimListFormat(LIST_TRIANGLES, false);
                indirect << BranchToImplicitTileList(0);
                indirect << StoreTileBufferGeneral(
                    BUFFER_RENDER_TARGET_0, V3D_TILING_RASTER, false,
                    V3D_DITHER_MODE_NONE, V3D_DECIMATE_MODE_SAMPLE_0,
                    V3D_OUTPUT_IMAGE_FORMAT_RGBA8, false, false, true,
                    width * 4, 0, render_target_paddr);
                indirect << StoreTileBufferGeneral(
                    BUFFER_Z, V3D_TILING_UIF_XOR, false, V3D_DITHER_MODE_NONE,
                    V3D_DECIMATE_MODE_SAMPLE_0, V3D_OUTPUT_IMAGE_FORMAT_D16,
                    false, false, false, (height + 15) / (2 * 4), 0, z_buffer_paddr);
                indirect << ClearTileBuffers(true, true);
                indirect << OP_END_OF_TILE_MARKER;
                indirect << OP_RETURN_FROM_SUB_LIST;
                rcl << StartAddressOfGenericTileList(indirect_addr, indirect.paddr());
            }
        }
        size_t max_supertile_x = (width - 1) / (tile_width * sw);
        size_t max_supertile_y = (height - 1) / (tile_height * sh);
        for (size_t y = 0; y <= max_supertile_y; y++)
            for (size_t x = 0; x <= max_supertile_x; x++)
                rcl << SupertileCoorinates(x, y);
        rcl << OP_END_OF_RENDERING;
    }

    hexdump_file("triangle_bin.bin", bcl.startPtr<u8>(), bcl.size());
    hexdump_file("triangle_render.bin", rcl.startPtr<u8>(), rcl.size());
    dumphex("triangle_bin.bin", bcl.startPtr<u8>(), bcl.size());
    dumphex("triangle_render.bin", rcl.startPtr<u8>(), rcl.size());

    /* AIOS extension: also dump the indirect-buffer sub-objects (GL Shader State
     * Record, the two attribute records, the generic tile list) so the AIOS emitters
     * are gated byte-exact too -- they are not in any CL stream, only referenced by
     * address, so the bin/render goldens cannot catch a bug in them. */
    u8 *ind = indirect.startPtr<u8>();
    u32 rec_off = shader_state_record_addr - IND;   /* 0x20 */
    hexdump_file("triangle_record.bin",   ind + rec_off,         36);
    hexdump_file("triangle_attr0.bin",    ind + rec_off + 36,    16);
    hexdump_file("triangle_attr1.bin",    ind + rec_off + 52,    16);
    hexdump_file("triangle_tilelist.bin", ind + rec_off + 68,    36);
    dumphex("triangle_record.bin",   ind + rec_off,      36);
    dumphex("triangle_attr0.bin",    ind + rec_off + 36, 16);
    dumphex("triangle_attr1.bin",    ind + rec_off + 52, 16);
    dumphex("triangle_tilelist.bin", ind + rec_off + 68, 36);
}

int main()
{
    buildClear();
    buildTriangle();
    return 0;
}
