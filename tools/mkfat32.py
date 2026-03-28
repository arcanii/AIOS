#!/usr/bin/env python3
"""
AIOS FAT32 Disk Image Creator

Usage: python3 tools/mkfat32.py [output] [size_mb]
  output   - output file (default: disk_fat32.img)
  size_mb  - image size in MiB (default: 256)
"""
import struct, sys, os

def mkfat32(img="disk_fat32.img", size_mb=256):
    disk_size = size_mb * 1024 * 1024
    sector_size = 512
    total_sectors = disk_size // sector_size

    sectors_per_cluster = 8
    reserved_sectors = 32
    num_fats = 2
    root_cluster = 2

    data_start_estimate = reserved_sectors + 2 * 512
    total_clusters_est = (total_sectors - data_start_estimate) // sectors_per_cluster
    fat_entries_per_sector = sector_size // 4
    sectors_per_fat = (total_clusters_est + 2 + fat_entries_per_sector - 1) // fat_entries_per_sector

    data_start = reserved_sectors + num_fats * sectors_per_fat
    total_clusters = (total_sectors - data_start) // sectors_per_cluster

    print(f"Creating {img} ({size_mb} MiB FAT32)")
    print(f"  Sectors/FAT: {sectors_per_fat}, Data start: sector {data_start}")
    print(f"  Total clusters: {total_clusters}, Root cluster: {root_cluster}")

    data = bytearray(disk_size)

    # ── BPB / Boot Sector ──
    bpb = bytearray(512)
    bpb[0:3] = b'\xEB\x58\x90'
    bpb[3:11] = b'AIOS    '
    struct.pack_into('<H', bpb, 11, sector_size)
    bpb[13] = sectors_per_cluster
    struct.pack_into('<H', bpb, 14, reserved_sectors)
    bpb[16] = num_fats
    struct.pack_into('<H', bpb, 17, 0)
    struct.pack_into('<H', bpb, 19, 0)
    bpb[21] = 0xF8
    struct.pack_into('<H', bpb, 22, 0)
    struct.pack_into('<H', bpb, 24, 63)
    struct.pack_into('<H', bpb, 26, 16)
    struct.pack_into('<I', bpb, 28, 0)
    struct.pack_into('<I', bpb, 32, total_sectors)

    struct.pack_into('<I', bpb, 36, sectors_per_fat)
    struct.pack_into('<H', bpb, 40, 0)
    struct.pack_into('<H', bpb, 42, 0)
    struct.pack_into('<I', bpb, 44, root_cluster)
    struct.pack_into('<H', bpb, 48, 1)
    struct.pack_into('<H', bpb, 50, 6)
    bpb[66] = 0x29
    struct.pack_into('<I', bpb, 67, 0x12345678)
    bpb[71:82] = b'AIOS       '
    bpb[82:90] = b'FAT32   '
    bpb[510] = 0x55; bpb[511] = 0xAA

    data[0:512] = bpb

    # ── FSInfo sector (sector 1) ──
    fsinfo = bytearray(512)
    struct.pack_into('<I', fsinfo, 0, 0x41615252)
    struct.pack_into('<I', fsinfo, 484, 0x61417272)
    struct.pack_into('<I', fsinfo, 488, total_clusters - 1)
    struct.pack_into('<I', fsinfo, 492, 3)
    fsinfo[510] = 0x55; fsinfo[511] = 0xAA
    data[512:1024] = fsinfo

    # ── Backup boot sector (sector 6) ──
    data[6*512:(6+1)*512] = bpb

    # ── FAT tables ──
    fat = bytearray(sectors_per_fat * sector_size)
    struct.pack_into('<I', fat, 0, 0x0FFFFFF8)
    struct.pack_into('<I', fat, 4, 0x0FFFFFFF)
    struct.pack_into('<I', fat, 8, 0x0FFFFFFF)

    fat1_start = reserved_sectors * sector_size
    fat2_start = (reserved_sectors + sectors_per_fat) * sector_size
    data[fat1_start:fat1_start + len(fat)] = fat
    data[fat2_start:fat2_start + len(fat)] = fat

    # ── Root directory (cluster 2) ──
    root_offset = data_start * sector_size
    vol = bytearray(32)
    vol[0:11] = b'AIOS       '
    vol[11] = 0x08
    data[root_offset:root_offset+32] = vol

    # ── Test file: HELLO.TXT ──
    hello_content = b'Hello from AIOS FAT32 filesystem!\n'
    hello_cluster = 3

    struct.pack_into('<I', fat, hello_cluster * 4, 0x0FFFFFFF)
    data[fat1_start:fat1_start + len(fat)] = fat
    data[fat2_start:fat2_start + len(fat)] = fat

    hello_offset = data_start * sector_size + (hello_cluster - 2) * sectors_per_cluster * sector_size
    data[hello_offset:hello_offset + len(hello_content)] = hello_content

    entry = bytearray(32)
    entry[0:11] = b'HELLO   TXT'
    entry[11] = 0x20
    struct.pack_into('<H', entry, 20, 0)
    struct.pack_into('<H', entry, 26, hello_cluster)
    struct.pack_into('<I', entry, 28, len(hello_content))
    data[root_offset+32:root_offset+64] = entry

    with open(img, 'wb') as f:
        f.write(data)

    print(f"  HELLO.TXT: cluster {hello_cluster}, {len(hello_content)} bytes")
    print(f"  FS signature: '{bpb[82:90].decode('ascii')}'")
    print("Done.")

if __name__ == '__main__':
    output = sys.argv[1] if len(sys.argv) > 1 else 'disk_fat32.img'
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 256
    mkfat32(output, size)
