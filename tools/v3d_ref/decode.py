#!/usr/bin/env python3
# Packet-by-packet decoder for the emitted V3D 4.2 CLs. Mirrors v3d_cl.hpp struct
# sizes exactly (each struct = opcode byte + payload, total = sizeof()).
import struct, sys

# opcode -> (name, total_len_bytes). Single-byte opcodes have len 1.
LEN = {
    4:("FLUSH",1), 6:("START_TILE_BINNING",1), 13:("END_OF_RENDERING",1),
    18:("RETURN_FROM_SUB_LIST",1), 19:("FLUSH_VCD_CACHE",1),
    20:("START_ADDRESS_OF_GENERIC_TILE_LIST",9),
    21:("BRANCH_TO_IMPLICIT_TILE_LIST",2),
    23:("SUPERTILE_COORDINATES",3),
    25:("CLEAR_TILE_BUFFERS",2), 26:("END_OF_LOADS",1), 27:("END_OF_TILE_MARKER",1),
    29:("STORE_TILE_BUFFER_GENERAL",13), 30:("LOAD_TILE_BUFFER_GENERAL",13),
    36:("VERTEX_ARRAY_PRIMS",10), 54:("SET_INSTANCEID",5), 56:("PRIM_LIST_FORMAT",2),
    64:("GL_SHADER_STATE",5), 71:("VCM_CACHE_SIZE",2),
    74:("TRANSFORM_FEEDBACK_SPECS",2), 86:("BLEND_CONSTANT_COLOR",9),
    87:("COLOR_WRITE_MASKS",5), 88:("ZERO_ALL_CENTROID_FLAGS",1),
    91:("SAMPLE_STATE",5), 92:("OCCLUSION_QUERY_COUNTER",5), 96:("CFG_BITS",4),
    97:("ZERO_ALL_FLAT_SHADE_FLAGS",1), 99:("ZERO_ALL_NON_PERSPECTIVE_FLAGS",1),
    104:("POINT_SIZE",5), 105:("LINE_WIDTH",5), 107:("CLIP_WINDOW",9),
    108:("VIEWPORT_OFFSET",9), 109:("CLIPPER_Z_MIN_MAX_CLIPPING_PLANES",9),
    110:("CLIPPER_XY_SCALING",9), 111:("CLIPPER_Z_SCALE_AND_OFFSET",9),
    119:("NUMBER_OF_LAYERS",2), 120:("TILE_BINNING_MODE_CFG",9),
    121:("TILE_RENDERING_MODE_CFG",9), 122:("MULTICORE_RENDERING_SUPERTILE_CFG",9),
    123:("MULTICORE_RENDERING_TILE_LIST_SET_BASE",5), 124:("TILE_COORDINATES",4),
    125:("TILE_COORDINATES_IMPLICIT",1), 126:("TILE_LIST_INITIAL_BLOCK_SIZE",2),
}

def bitsval(payload, bitoff, nbits):
    # little-endian bit extraction from a bytes payload (LSB-first within bytes).
    v = int.from_bytes(payload, "little")
    return (v >> bitoff) & ((1 << nbits) - 1)

def decode(name, data):
    print(f"### {name} ({len(data)} bytes) ###")
    off = 0
    while off < len(data):
        op = data[off]
        if op not in LEN:
            print(f"  [{off:#06x}] op={op} UNKNOWN -- stop"); break
        nm, ln = LEN[op]
        pkt = data[off:off+ln]
        pl = pkt[1:]  # payload after opcode
        d = ""
        if op == 120:  # TBMC
            b1, b2 = pl[0], pl[1]
            iblk = (b1>>2)&3; blk = (b1>>4)&3
            nrt = (b2&0xF)+1; bpp=(b2>>4)&3; msaa=(b2>>6)&1; db=(b2>>7)&1
            w = struct.unpack_from("<H", pl,4)[0]+1; h = struct.unpack_from("<H", pl,6)[0]+1
            d = f"init_blk={iblk} blk={blk} nRT={nrt} maxBpp={bpp} msaa={msaa} dbuf={db} w={w} h={h}"
        elif op == 92:
            d = f"address={struct.unpack('<I',pl)[0]:#x}"
        elif op == 121:  # TRMC, sub_id in low nibble of byte1
            sub = pl[0] & 0xF
            if sub == 0:
                nrt=(pl[0]>>4)+1
                w=struct.unpack_from("<H",pl,1)[0]; h=struct.unpack_from("<H",pl,3)[0]
                # bytes 5..6 hold the bitfield word
                w2=struct.unpack_from("<H",pl,5)[0]
                bpp=w2&3; msaa=(w2>>2)&1; db=(w2>>3)&1; zdir=(w2>>5)&1; zdis=(w2>>6)&1
                idt=(w2>>7)&0xF; esc=(w2>>11)&1
                d=f"COMMON sub=0 nRT={nrt} w={w} h={h} maxBpp={bpp} msaa={msaa} dbuf={db} earlyZdir={zdir} earlyZdis={zdis} internalDepthType={idt} earlyStencilClear={esc}"
            elif sub == 1:
                v=int.from_bytes(pl,"little")
                f=[]
                pos=4
                for i in range(4):
                    bpp=(v>>pos)&3; pos+=2
                    typ=(v>>pos)&0xF; pos+=4
                    clamp=(v>>pos)&3; pos+=2
                    f.append(f"RT{i}(bpp={bpp},type={typ},clamp={clamp})")
                d="COLOR sub=1 "+" ".join(f)
            elif sub == 2:
                stencil=pl[1]; z=struct.unpack_from("<f",pl,2)[0]
                d=f"ZS_CLEAR sub=2 stencil={stencil} z={z}"
            elif sub == 3:
                v=int.from_bytes(pl,"little")
                rt=(v>>4)&0xF; low=(v>>8)&0xFFFFFFFF; nxt=(v>>40)&0xFFFFFF
                d=f"CLEAR_COLORS sub=3 RT={rt} clear_low32={low:#010x} clear_next24={nxt:#x}"
            else:
                d=f"sub_id={sub} (unknown)"
        elif op == 126:
            b=pl[0]; d=f"first_block_size={b&3} (0=64B,1=128B,2=256B) auto_chained={(b>>2)&1}"
        elif op == 123:
            v=struct.unpack("<I",pl)[0]; setn=v&0xF; addr=(v>>6)&((1<<26)-1)
            d=f"tile_list_set={setn} addr={(addr<<6):#x} (raw>>6={addr:#x})"
        elif op == 122:
            # payload is 8 bytes: 4 u8 then a 4-byte unit holding
            # frame_w_tiles:12, frame_h_tiles:12, multicore:1, _:3, raster:1, nbtl:3
            sw=pl[0]+1; sh=pl[1]+1; fws=pl[2]; fhs=pl[3]
            w2=struct.unpack_from("<I",pl,4)[0]
            twt=w2&0xFFF; tht=(w2>>12)&0xFFF
            mc=(w2>>24)&1; ro=(w2>>28)&1; nbtl=((w2>>29)&7)+1
            d=f"superTile={sw}x{sh}tiles frame={fws}x{fhs}supertiles frame={twt}x{tht}tiles multicore={mc} rasterOrder={ro} nBinTileLists={nbtl}"
        elif op == 124:
            v=int.from_bytes(pl,"little"); col=v&0xFFF; row=(v>>12)&0xFFF
            d=f"col={col} row={row}"
        elif op == 25:
            b=pl[0]; d=f"clear_all_RT={b&1} clear_zstencil={(b>>1)&1}"
        elif op == 29:  # STORE_TILE_BUFFER_GENERAL (packed; total 13 bytes)
            # pl[0]: buffer:4 memfmt:3 flipy:1
            # pl[1..2] u16: dither:2 dec:2 ofmt:6 clr:1 chrev:1 rbswap:1 _:3
            # pl[3..5] (3 bytes/24 bits): _:4 stride:20
            # pl[6..7] u16 height
            # pl[8..11] u32 address
            b0=pl[0]; buf=b0&0xF; memfmt=(b0>>4)&7; flipy=(b0>>7)&1
            w16=struct.unpack_from("<H",pl,1)[0]
            dither=w16&3; dec=(w16>>2)&3; ofmt=(w16>>4)&0x3F; clr=(w16>>10)&1; chrev=(w16>>11)&1; rbswap=(w16>>12)&1
            w24=int.from_bytes(pl[3:6],"little"); stride=(w24>>4)&((1<<20)-1)
            height=struct.unpack_from("<H",pl,6)[0]
            addr=struct.unpack_from("<I",pl,8)[0]
            d=f"buffer={buf}{'(NONE)' if buf==8 else ''} memFmt={memfmt} flipY={flipy} dither={dither} decimate={dec} outFmt={ofmt} clearOnStore={clr} chanRev={chrev} rbSwap={rbswap} stride={stride} height={height} addr={addr:#x}"
        elif op == 20:
            s=struct.unpack_from("<I",pl,0)[0]; e=struct.unpack_from("<I",pl,4)[0]
            d=f"start={s:#x} end={e:#x}"
        elif op == 21:
            d=f"tile_list_set_number={pl[0]}"
        elif op == 56:
            b=pl[0]; d=f"primitive_type={b&0x3F}(2=triangles) tri_strip_or_fan={(b>>7)&1}"
        elif op == 54:
            d=f"instance_id={struct.unpack('<I',pl)[0]}"
        elif op == 23:
            d=f"supertile col={pl[0]} row={pl[1]}"
        elif op in (4,6,13,18,19,26,27,88,97,99,125):
            d="(opcode only)"
        else:
            d="payload="+pl.hex()
        print(f"  [{off:#06x}] op={op:<3} {nm:<38} len={ln:<2} {d}")
        off += ln
    print()

for path,label in [("golden_bin.bin","BIN CL (CLEAR)"),("golden_render.bin","RENDER CL (CLEAR)"),
                   ("triangle_bin.bin","BIN CL (TRIANGLE)"),("triangle_render.bin","RENDER CL (TRIANGLE)")]:
    decode(label, open(""+path,"rb").read())
