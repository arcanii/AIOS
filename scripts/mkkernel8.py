#!/usr/bin/env python3
"""Build a standalone kernel8.img from the build-rpi4 seL4 image.

For FAST in-place kernel updates: copy the resulting kernel8.img onto the SD
card's FAT (boot) partition instead of re-flashing the whole ~193 MB SD image.
This is valid ONLY when the kernel / root task changed and the ext2 SYSTEM
partition did NOT (kernel8.img is the only file the root task lives in). For
userspace / app changes, push over the network instead -- do NOT use this.

kernel8.img here is byte-identical to what scripts/mksdcard.py writes into the
FAT partition (same convert_elf_to_bin: 4KB ARM64 relocator stub + payload).

Usage:
  python3 scripts/mkkernel8.py [--kernel <elf>] [--output disk/kernel8.img]

Then on macOS (the FAT partition mounts read-write natively):
  cp disk/kernel8.img /Volumes/<bootvol>/kernel8.img
  diskutil eject /Volumes/<bootvol>
  # reinsert into the Pi and power on
"""
import os, sys, argparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, SCRIPT_DIR)
import mksdcard  # reuse convert_elf_to_bin (has a __main__ guard -- safe to import)


def main():
    ap = argparse.ArgumentParser(description="Build a standalone RPi4 kernel8.img")
    ap.add_argument("--kernel",
                    default=os.path.join(ROOT, "build-rpi4", "images",
                                         "aios_root-image-arm-bcm2711"),
                    help="Path to the seL4 image ELF")
    ap.add_argument("--output", default=os.path.join(ROOT, "disk", "kernel8.img"),
                    help="Output kernel8.img path")
    a = ap.parse_args()

    if not os.path.exists(a.kernel):
        print(f"FAIL: seL4 image not found: {a.kernel}")
        print("  Build it first: ninja -C build-rpi4")
        sys.exit(1)

    print("=== AIOS kernel8.img builder (in-place FAT update) ===")
    mksdcard.convert_elf_to_bin(a.kernel, a.output)
    if not os.path.exists(a.output):
        print("FAIL: kernel8.img was not produced")
        sys.exit(1)

    print(f"\nWrote {a.output} ({os.path.getsize(a.output)} bytes)")
    print("Copy it onto the SD card's FAT (boot) partition, then reboot:")
    print("  diskutil list                         # find the boot volume")
    print("  cp disk/kernel8.img /Volumes/<bootvol>/kernel8.img")
    print("  diskutil eject /Volumes/<bootvol>")
    print("NOTE: kernel/root-task changes only. App changes ship over the network.")


if __name__ == "__main__":
    main()
