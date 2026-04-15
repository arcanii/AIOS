#!/usr/bin/env python3
"""
mksdcard.py -- Create bootable RPi4 SD card image for AIOS

Builds a raw .img file with:
  Partition 1: FAT32 (64MB) -- RPi firmware + config.txt + kernel
  Partition 2: ext2  (128MB) -- AIOS system disk

Usage:
  python3 scripts/mksdcard.py [--mem 4096] [--output disk/sdcard-rpi4.img]

Requirements:
  - mtools (brew install mtools)
  - aarch64-linux-gnu-objcopy
  - RPi firmware files in disk/rpi4-firmware/
    (auto-downloaded on first run)
"""
import os
import sys
import struct
import subprocess
import shutil
import argparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(SCRIPT_DIR)

# Firmware download URLs (from RPi firmware repo, stable branch)
FIRMWARE_BASE = "https://github.com/raspberrypi/firmware/raw/stable/boot"
FIRMWARE_FILES = [
    "start4.elf", "fixup4.dat",
    "start.elf", "fixup.dat",          # fallback for older EEPROM
    "bcm2711-rpi-4-b.dtb",
]

# Sizes
BOOT_SIZE_MB = 64
SECTOR_SIZE = 512
BOOT_START_SECTOR = 2048  # 1MB offset (standard alignment)


def run(cmd, check=True):
    """Run a shell command, return stdout."""
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if check and r.returncode != 0:
        print(f"FAIL: {cmd}")
        if r.stderr:
            print(r.stderr.strip())
        sys.exit(1)
    return r.stdout.strip()


def download_firmware(fw_dir):
    """Download RPi4 firmware files if not cached."""
    os.makedirs(fw_dir, exist_ok=True)
    for fname in FIRMWARE_FILES:
        path = os.path.join(fw_dir, fname)
        if os.path.exists(path):
            continue
        url = f"{FIRMWARE_BASE}/{fname}"
        print(f"  Downloading {fname}...")
        run(f'curl -fsSL -o "{path}" "{url}"')
        if not os.path.exists(path) or os.path.getsize(path) == 0:
            print(f"FAIL: download {fname}")
            sys.exit(1)
    print(f"  Firmware cached in {fw_dir}")


def get_elf_entry_point(elf_path):
    """Read the entry point address from the ELF header.

    For seL4 images, the entry point is the .start section address.
    objcopy -O binary places .start at binary offset 0, so the RPi
    firmware must load kernel8.img at this address and jump to it.
    """
    out = run(f'aarch64-linux-gnu-readelf -h "{elf_path}"')
    for line in out.split("\n"):
        if "Entry point" in line:
            parts = line.split()
            for p in parts:
                if p.startswith("0x"):
                    return int(p, 16)
    return None


def convert_elf_to_bin(elf_path, bin_path):
    """Convert ELF to flat binary with ARM64 relocator stub.

    RPi4 firmware requires ARM64 Image header (magic ARMd at 0x30)
    and always relocates to 0x80000. The 4KB relocator stub copies
    the seL4 payload to its correct link address and jumps to it.
    """
    import struct, subprocess as sp

    # Step 1: flat binary
    raw_path = bin_path + ".raw"
    run(f'aarch64-linux-gnu-objcopy -O binary "{elf_path}" "{raw_path}"')

    # Step 2: read entry point
    entry = get_elf_entry_point(elf_path)
    if entry is None:
        print("  FAIL: cannot read entry point")
        return

    # Step 3: read payload, pad to 16-byte alignment
    with open(raw_path, "rb") as f:
        payload = f.read()
    pad = (16 - (len(payload) % 16)) % 16
    payload += b'\x00' * pad
    ps = len(payload)
    stub_size = 4096

    # Step 4: generate relocator assembly
    dl, dh = entry & 0xFFFF, (entry >> 16) & 0xFFFF
    sl, sh = ps & 0xFFFF, (ps >> 16) & 0xFFFF
    asm_src = f""".section .text
.global _start
_start:
    b relocate
    .word 0
    .quad 0
    .quad {stub_size + ps}
    .quad 0
    .quad 0
    .quad 0
    .word 0x644d5241
    .word 0
    .quad 0
relocate:
    mov x20, x0
    msr daifset, #0xf
    adr x5, _start
    mov x0, x5
    add x0, x0, #{stub_size}
    movz x1, #0x{dl:x}
    movk x1, #0x{dh:x}, lsl #16
    movz x2, #0x{sl:x}
    movk x2, #0x{sh:x}, lsl #16
    mov x10, x1
1:  ldp x3, x4, [x0], #16
    stp x3, x4, [x1], #16
    subs x2, x2, #16
    b.gt 1b
    dsb sy
    isb
    ic iallu
    dsb sy
    isb
    mov x0, x20
    br x10
.balign {stub_size}
"""
    asm_path = bin_path + ".S"
    with open(asm_path, "w") as f:
        f.write(asm_src)

    # Step 5: assemble + link + objcopy
    obj_path = bin_path + ".o"
    elf_stub = bin_path + ".stub.elf"
    stub_bin = bin_path + ".stub.bin"
    for cmd in [
        ["aarch64-linux-gnu-as", "-o", obj_path, asm_path],
        ["aarch64-linux-gnu-ld", "-Ttext=0", "-o", elf_stub, obj_path],
        ["aarch64-linux-gnu-objcopy", "-O", "binary", elf_stub, stub_bin],
    ]:
        r = sp.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  FAIL: {cmd[0]}: {r.stderr}")
            return

    with open(stub_bin, "rb") as f:
        stub = f.read()
    stub += b'\x00' * (stub_size - len(stub))

    # Step 6: combine stub + payload
    with open(bin_path, "wb") as f:
        f.write(stub + payload)

    total = stub_size + ps
    print(f"  Relocator stub: {stub_size} bytes (entry 0x{entry:x})")
    print(f"  Payload: {ps} bytes")
    print(f"  Total kernel8.img: {total} bytes")

    # Cleanup temp files
    for p in [raw_path, asm_path, obj_path, elf_stub, stub_bin]:
        if os.path.exists(p):
            os.remove(p)


def create_config_txt(path, mem_mb, kernel_addr=None):
    """Create RPi4 config.txt for AIOS."""
    config = "# AIOS RPi4 boot config\n"
    config += "arm_64bit=1\n"
    config += "kernel=kernel8.img\n"
    # kernel_address removed: firmware ignores it for ARM64 images
    # Relocator stub handles placement
    config += "enable_uart=1\n"
    config += "uart_2ndstage=1\n"
    # dtoverlay=disable-bt removed: GPIO 14/15 must be mini UART
    config += "core_freq=250\n"
    config += "core_freq_min=250\n"
    config += "disable_overscan=1\n"
    config += f"total_mem={mem_mb}\n"
    config += "gpu_mem=64\n"
    with open(path, "w") as f:
        f.write(config)


def create_fat32_image(fat_img, size_mb, files):
    """Create a FAT32 image and copy files into it using mtools."""
    size_bytes = size_mb * 1024 * 1024

    # Create empty image
    with open(fat_img, "wb") as f:
        f.seek(size_bytes - 1)
        f.write(b"\x00")

    # Format as FAT32 with hidden sectors matching partition offset
    run(f'mformat -i "{fat_img}" -F -v AIOSBOOT -H {BOOT_START_SECTOR} '
        f'-c 32 -T {size_bytes // SECTOR_SIZE} ::')

    # Copy files
    for src, dst_name in files:
        run(f'mcopy -i "{fat_img}" "{src}" "::{dst_name}"')
        sz = os.path.getsize(src)
        print(f"  {dst_name} ({sz} bytes)")


def write_mbr(img_path, boot_sectors, ext2_start_sector, ext2_sectors):
    """Write MBR partition table with 2 partitions."""
    with open(img_path, "r+b") as f:
        # Partition 1: FAT32 LBA (type 0x0C)
        p1 = struct.pack("<BBBBBBBBII",
            0x80,           # active/bootable
            0x00, 0x00, 0x00,  # CHS start (ignored for LBA)
            0x0C,           # FAT32 LBA
            0x00, 0x00, 0x00,  # CHS end (ignored)
            BOOT_START_SECTOR,  # LBA start
            boot_sectors)       # sector count

        # Partition 2: Linux ext2 (type 0x83)
        p2 = struct.pack("<BBBBBBBBII",
            0x00,
            0x00, 0x00, 0x00,
            0x83,           # Linux
            0x00, 0x00, 0x00,
            ext2_start_sector,
            ext2_sectors)

        # Write partition table at MBR offset 446
        f.seek(446)
        f.write(p1)
        f.write(p2)
        f.write(b"\x00" * 32)  # partitions 3-4 empty

        # MBR signature
        f.seek(510)
        f.write(b"\x55\xAA")


def main():
    parser = argparse.ArgumentParser(description="Create RPi4 SD card image")
    parser.add_argument("--output", default=os.path.join(ROOT_DIR, "disk", "sdcard-rpi4.img"),
                        help="Output image path")
    parser.add_argument("--mem", type=int, default=4096,
                        help="RPi4 memory in MB (1024/2048/4096/8192)")
    parser.add_argument("--kernel",
                        default=os.path.join(ROOT_DIR, "build-rpi4", "images",
                                             "aios_root-image-arm-bcm2711"),
                        help="Path to kernel ELF")
    parser.add_argument("--ext2",
                        default=os.path.join(ROOT_DIR, "disk", "disk_ext2.img"),
                        help="Path to ext2 system image")
    args = parser.parse_args()

    print("=== AIOS RPi4 SD Card Image Builder ===")

    # Check prerequisites
    if not os.path.exists(args.kernel):
        print(f"FAIL: kernel not found: {args.kernel}")
        print("  Build RPi4 target first:")
        print("  mkdir build-rpi4 && cd build-rpi4")
        print("  cmake -G Ninja -DAIOS_SETTINGS=settings-rpi4.cmake ...")
        sys.exit(1)

    if not os.path.exists(args.ext2):
        print(f"FAIL: ext2 image not found: {args.ext2}")
        print("  Run: python3 scripts/mkdisk.py disk/disk_ext2.img ...")
        sys.exit(1)

    for tool in ["mformat", "mcopy", "aarch64-linux-gnu-objcopy"]:
        if shutil.which(tool) is None:
            print(f"FAIL: {tool} not found")
            if "mformat" in tool or "mcopy" in tool:
                print("  Install: brew install mtools")
            sys.exit(1)

    # Paths
    fw_dir = os.path.join(ROOT_DIR, "disk", "rpi4-firmware")
    tmp_dir = os.path.join(ROOT_DIR, "disk", ".sdcard-tmp")
    os.makedirs(tmp_dir, exist_ok=True)

    # Step 1: Download firmware
    print("\n[1/5] Firmware")
    download_firmware(fw_dir)

    # Step 2: Convert kernel ELF to binary
    print("\n[2/5] Kernel")
    kernel_bin = os.path.join(tmp_dir, "kernel8.img")
    convert_elf_to_bin(args.kernel, kernel_bin)

    # Step 3: Create config.txt (with entry point from ELF)
    print("\n[3/5] Config")
    entry_addr = get_elf_entry_point(args.kernel)
    if entry_addr is None:
        print("FAIL: cannot read ELF entry point")
        sys.exit(1)
    config_path = os.path.join(tmp_dir, "config.txt")
    create_config_txt(config_path, args.mem)
    print(f"  config.txt (mem={args.mem}MB, kernel_address=0x{entry_addr:x})")

    # Step 4: Create FAT32 boot partition
    print("\n[4/5] Boot partition (FAT32, %dMB)" % BOOT_SIZE_MB)
    fat_img = os.path.join(tmp_dir, "boot.fat32")

    boot_files = [
        (os.path.join(fw_dir, "start4.elf"), "start4.elf"),
        (os.path.join(fw_dir, "fixup4.dat"), "fixup4.dat"),
        (os.path.join(fw_dir, "start.elf"), "start.elf"),
        (os.path.join(fw_dir, "fixup.dat"), "fixup.dat"),
        # Old EEPROM may request start_cd.elf instead of start4.elf
        (os.path.join(fw_dir, "start4.elf"), "start_cd.elf"),
        (os.path.join(fw_dir, "bcm2711-rpi-4-b.dtb"), "bcm2711-rpi-4-b.dtb"),
        (config_path, "config.txt"),
        (kernel_bin, "kernel8.img"),
    ]
    create_fat32_image(fat_img, BOOT_SIZE_MB, boot_files)

    # Step 5: Assemble final image
    print("\n[5/5] Assembling SD card image")
    ext2_size = os.path.getsize(args.ext2)

    boot_sectors = (BOOT_SIZE_MB * 1024 * 1024) // SECTOR_SIZE
    ext2_start_sector = BOOT_START_SECTOR + boot_sectors
    ext2_sectors = ext2_size // SECTOR_SIZE
    total_size = (ext2_start_sector + ext2_sectors) * SECTOR_SIZE

    # Create image with empty space for MBR + alignment
    with open(args.output, "wb") as f:
        f.seek(total_size - 1)
        f.write(b"\x00")

    # Write MBR
    write_mbr(args.output, boot_sectors, ext2_start_sector, ext2_sectors)

    # Write FAT32 boot partition
    boot_offset = BOOT_START_SECTOR * SECTOR_SIZE
    with open(fat_img, "rb") as src:
        fat_data = src.read()
    with open(args.output, "r+b") as f:
        f.seek(boot_offset)
        f.write(fat_data)

    # Write ext2 system partition
    ext2_offset = ext2_start_sector * SECTOR_SIZE
    with open(args.ext2, "rb") as src:
        ext2_data = src.read()
    with open(args.output, "r+b") as f:
        f.seek(ext2_offset)
        f.write(ext2_data)

    # Cleanup temp files
    shutil.rmtree(tmp_dir, ignore_errors=True)

    total_mb = total_size // (1024 * 1024)
    print(f"\nCreated {args.output} ({total_mb} MB)")
    print(f"  Partition 1: FAT32 boot ({BOOT_SIZE_MB}MB)")
    print(f"  Partition 2: ext2 system ({ext2_size // (1024*1024)}MB)")
    print("\nWARNING: mtools FAT32 may not work with RPi firmware.")
    print("  Use Method 2 (diskutil + cp) if boot fails.")
    print("  See hw/rpi4/BOOT_NOTES.md")
    print("\nFlash to SD card:")
    print(f"  diskutil list                    # find SD card (e.g. /dev/disk4)")
    print(f"  diskutil unmountDisk /dev/diskN")
    print(f"  sudo dd if={args.output} of=/dev/rdiskN bs=1m")
    print(f"  diskutil eject /dev/diskN")


if __name__ == "__main__":
    main()
