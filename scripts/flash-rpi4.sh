#!/usr/bin/env bash
# flash-rpi4.sh -- v0.4.133 one-shot SD card flasher for AIOS / RPi4.
#
# Wraps scripts/mksdcard.py with a dd-to-disk step plus several safety
# checks. Run from the repo root with the target device as the only
# argument:
#
#   ./scripts/flash-rpi4.sh /dev/disk4
#
# Refuses:
# * any device in /dev/disk0..2 (typical macOS system internal disks)
# * any device that is not a whole disk (e.g. partition slices)
# * a target whose diskutil info says "internal=yes"
# * proceeding without the user typing literally "YES"
#
# This script is macOS-only -- it depends on diskutil + the mksdcard.py
# newfs_msdos/hdiutil path. On Linux you can call mksdcard.py and dd by
# hand (Method 2 in hw/rpi4/BOOT_NOTES.md).

set -euo pipefail

if [ "$(uname)" != "Darwin" ]; then
    echo "FAIL: macOS only. On Linux, run mksdcard.py + dd by hand."
    echo "      See hw/rpi4/BOOT_NOTES.md."
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
KERNEL="$ROOT_DIR/build-rpi4/images/aios_root-image-arm-bcm2711"
EXT2="$ROOT_DIR/disk/disk_ext2.img"
IMG="$ROOT_DIR/disk/sdcard-rpi4.img"

if [ $# -ne 1 ]; then
    echo "usage: $0 /dev/diskN"
    echo
    echo "Find your SD card with: diskutil list"
    echo "Look for an external 'physical' disk matching the SD's size."
    exit 1
fi

TARGET="$1"

# Reject anything that does not look like /dev/disk<N>.
if [[ ! "$TARGET" =~ ^/dev/disk[0-9]+$ ]]; then
    echo "FAIL: target must be a whole-disk device like /dev/disk4."
    echo "      Got: $TARGET"
    echo "      Slices like /dev/disk4s1 are partitions, not disks."
    exit 1
fi

# Hard-block the obvious system disks. macOS conventionally has the
# internal NVMe at /dev/disk0 and APFS containers at /dev/disk1..2.
case "$TARGET" in
    /dev/disk0|/dev/disk1|/dev/disk2)
        echo "FAIL: $TARGET is almost certainly a system disk. Refusing."
        echo "      If you genuinely want this, edit the script."
        exit 1
        ;;
esac

# Pull diskutil info; refuse if the target is internal.
INFO=$(diskutil info "$TARGET" 2>/dev/null || true)
if [ -z "$INFO" ]; then
    echo "FAIL: diskutil info $TARGET returned nothing."
    echo "      Run 'diskutil list' to confirm the device exists."
    exit 1
fi

if echo "$INFO" | grep -qi "Device Location:[[:space:]]*Internal"; then
    echo "FAIL: $TARGET reports Device Location: Internal. Refusing."
    echo "      Use an external SD card reader."
    exit 1
fi

SIZE=$(echo "$INFO" | awk -F: '/Disk Size/ {print $2; exit}' | xargs)
NAME=$(echo "$INFO" | awk -F: '/Device \/ Media Name/ {print $2; exit}' | xargs)

# Pre-flight checks on the source artefacts.
if [ ! -f "$KERNEL" ]; then
    echo "FAIL: missing $KERNEL"
    echo "      Build first:  mkdir -p build-rpi4 && cd build-rpi4 \\"
    echo "        && cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \\"
    echo "             -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- \\"
    echo "             -DAIOS_PLATFORM=PLAT_RPI4 .. \\"
    echo "        && ninja"
    exit 1
fi

if [ ! -f "$EXT2" ]; then
    echo "FAIL: missing $EXT2"
    echo "      Build the disk first: python3 scripts/mkdisk.py ..."
    exit 1
fi

# Soft warning if the disk is older than the kernel -- the on-disk
# binaries may be missing recent libaios_posix.a fixes.
if [ "$EXT2" -ot "$KERNEL" ]; then
    echo "WARN: $EXT2 is older than the RPi4 kernel."
    echo "      Consider rebuilding the disk to pick up recent libaios changes."
fi

# Generate the SD image.
echo "[1/3] Building SD image at $IMG"
python3 "$SCRIPT_DIR/mksdcard.py" --output "$IMG" >/dev/null

if [ ! -f "$IMG" ]; then
    echo "FAIL: mksdcard.py did not produce $IMG"
    exit 1
fi

IMG_MB=$(( $(stat -f %z "$IMG") / 1024 / 1024 ))
echo "      $IMG ($IMG_MB MB)"

# Final confirmation.
echo
echo "[2/3] About to flash:"
echo "      target:  $TARGET ($SIZE)"
echo "      device:  $NAME"
echo "      source:  $IMG ($IMG_MB MB)"
echo
echo "      ALL DATA ON $TARGET WILL BE OVERWRITTEN."
read -r -p "Type YES to proceed: " CONFIRM
if [ "$CONFIRM" != "YES" ]; then
    echo "      Cancelled (need literal YES)."
    exit 1
fi

# Switch to the raw device (faster) and unmount before dd.
RAW="${TARGET/disk/rdisk}"
echo
echo "[3/3] Flashing $TARGET"
diskutil unmountDisk "$TARGET" >/dev/null
sudo dd if="$IMG" of="$RAW" bs=1m
diskutil eject "$TARGET"

echo
echo "OK: flashed $IMG to $TARGET"
echo "    Insert into RPi4, attach serial (115200 baud), power on."
