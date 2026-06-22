#!/usr/bin/env python3
"""Build a single-partition (FAT32-only) Etcher-ready boot image for the E1
reproducer: MBR + FAT32 boot partition with the RPi firmware + config.txt +
kernel8.img. Reuses scripts/mksdcard.py's RPi-firmware-compatible newfs_msdos
formatter (mformat produces a BPB the RPi firmware rejects -- see BOOT_NOTES).

Run after build.sh.  Output: experiments/e1_repro/e1_boot.img  -> flash with Etcher.
"""
import os, sys, struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import mksdcard as m  # noqa: E402

FW = os.path.join(ROOT, "disk", "rpi4-firmware")
BOOT_MB = 64

files = [
    (os.path.join(FW, "start4.elf"),            "start4.elf"),
    (os.path.join(FW, "fixup4.dat"),            "fixup4.dat"),
    (os.path.join(FW, "bcm2711-rpi-4-b.dtb"),   "bcm2711-rpi-4-b.dtb"),
    (os.path.join(HERE, "config.txt"),          "config.txt"),
    (os.path.join(HERE, "kernel8.img"),         "kernel8.img"),
]
for src, _ in files:
    if not os.path.exists(src):
        sys.exit("missing: %s (run build.sh first?)" % src)

fat_img = os.path.join(HERE, "fat.img")
out_img = os.path.join(HERE, "e1_boot.img")

# 1. FAT32 boot partition (firmware-compatible formatter + file copy).
m.create_fat32_image(fat_img, BOOT_MB, files)

# 2. Assemble MBR + FAT partition at the standard 1MB (sector 2048) offset.
boot_sectors = (BOOT_MB * 1024 * 1024) // m.SECTOR_SIZE
total_sectors = m.BOOT_START_SECTOR + boot_sectors
total_size = total_sectors * m.SECTOR_SIZE

with open(out_img, "wb") as f:
    f.seek(total_size - 1)
    f.write(b"\x00")
with open(fat_img, "rb") as f:
    fat = f.read()
with open(out_img, "r+b") as f:
    f.seek(m.BOOT_START_SECTOR * m.SECTOR_SIZE)
    f.write(fat)
    # MBR partition 1: FAT32 LBA (type 0x0C), bootable.
    f.seek(0x1BE)
    f.write(struct.pack("<BBBBBBBBII", 0x80, 0, 0, 0, 0x0C, 0, 0, 0,
                        m.BOOT_START_SECTOR, boot_sectors))
    f.seek(0x1FE)
    f.write(b"\x55\xAA")
os.remove(fat_img)
print("wrote %s (%d bytes) -- flash to the spare SD with balenaEtcher" % (out_img, total_size))
