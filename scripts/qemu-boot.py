#!/usr/bin/env python3
"""AIOS QEMU launcher — boots QEMU virt with configurable options."""
import os
import sys
import subprocess

AIOS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def main():
    build = os.path.join(AIOS, "build-04")
    disk = os.path.join(AIOS, "disk", "disk_ext2.img")
    log_disk = os.path.join(AIOS, "disk", "log_ext2.img")
    kernel = os.path.join(build, "images", "aios_root-image-arm-qemu-arm-virt")

    if not os.path.isfile(kernel):
        print(f"ERROR: kernel image not found: {kernel}")
        sys.exit(1)
    if not os.path.isfile(disk):
        print(f"ERROR: disk image not found: {disk}")
        sys.exit(1)

    cmd = [
        "qemu-system-aarch64",
        "-machine", "virt,virtualization=on",
        "-cpu", "cortex-a53",
        "-smp", "4",
        "-m", "2G",
        "-nographic",
        "-serial", "mon:stdio",
        "-drive", f"file={disk},format=raw,if=none,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
    ]

    # Log disk (optional)
    if os.path.isfile(log_disk):
        cmd += [
            "-drive", f"file={log_disk},format=raw,if=none,id=hd1",
            "-device", "virtio-blk-device,drive=hd1",
        ]

    # Network (pass --net to enable)
    if "--net" in sys.argv:
        cmd += [
            "-device", "virtio-net-device,netdev=net0",
            "-netdev", "user,id=net0,hostfwd=tcp::8080-:80",
        ]

    # Display (pass --display to enable)
    if "--display" in sys.argv:
        cmd += ["-device", "ramfb"]

    cmd += ["-kernel", kernel]

    print(f"Booting AIOS (QEMU virt, 4-core, 2G RAM)")
    if "--net" in sys.argv:
        print("  Network: enabled (hostfwd 8080->80)")
    if "--display" in sys.argv:
        print("  Display: ramfb enabled")
    print(f"  Kernel: {kernel}")
    print()

    os.execvp(cmd[0], cmd)

if __name__ == "__main__":
    main()
