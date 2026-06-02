#!/usr/bin/env bash
# flash-rpi4.sh -- one-shot SD card flasher for AIOS / RPi4.
#
# Wraps scripts/mksdcard.py with a dd-to-disk step plus safety checks and
# a post-flash hash verification. Run from the repo root:
#
#   ./scripts/flash-rpi4.sh /dev/disk4
#   ./scripts/flash-rpi4.sh --allow-internal /dev/disk6   # built-in reader
#
# Refuses:
# * any device in /dev/disk0..2 (typical macOS system internal disks)
# * any device that is not a whole disk (e.g. partition slices)
# * proceeding without the user typing literally "YES"
#
# Device Location: Internal is allowed only when the device looks like a
# removable SD card (Protocol Secure Digital + Removable Media Removable
# + not read-only) -- the Mac built-in SDXC reader reports Internal -- or
# when --allow-internal is passed explicitly.
#
# After dd it remounts the card and compares the FAT kernel8.img hash to
# the source image, failing loudly on mismatch (catches silent
# non-flashes where the card keeps booting an old image).
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

ALLOW_INTERNAL=0
TARGET=""
for arg in "$@"; do
    case "$arg" in
        --allow-internal) ALLOW_INTERNAL=1 ;;
        -*)
            echo "FAIL: unknown option: $arg"
            exit 1
            ;;
        *)
            if [ -n "$TARGET" ]; then
                echo "FAIL: more than one target device given."
                exit 1
            fi
            TARGET="$arg"
            ;;
    esac
done

if [ -z "$TARGET" ]; then
    echo "usage: $0 [--allow-internal] /dev/diskN"
    echo
    echo "Find your SD card with: diskutil list"
    echo "Look for an external physical disk matching the SD size, or the"
    echo "built-in SD reader (diskutil info shows Protocol: Secure Digital)."
    exit 1
fi

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

# Built-in SD readers (Mac SDXC slot) report Device Location: Internal
# even though the media is removable. Allow when the device is clearly a
# removable SD card, or when --allow-internal was passed; otherwise
# refuse (it could be a real internal disk).
if echo "$INFO" | grep -qi "Device Location:[[:space:]]*Internal"; then
    IS_SD=0
    if echo "$INFO" | grep -qi "Protocol:[[:space:]]*Secure Digital" \
       && echo "$INFO" | grep -qi "Removable Media:[[:space:]]*Removable" \
       && echo "$INFO" | grep -qi "Media Read-Only:[[:space:]]*No"; then
        IS_SD=1
    fi
    if [ "$IS_SD" -eq 1 ]; then
        echo "WARN: $TARGET is Device Location: Internal but looks like a"
        echo "      removable SD card (Secure Digital, Removable). Allowing."
    elif [ "$ALLOW_INTERNAL" -eq 1 ]; then
        echo "WARN: $TARGET is Device Location: Internal; allowed via --allow-internal."
    else
        echo "FAIL: $TARGET reports Device Location: Internal. Refusing."
        echo "      If this is the Mac built-in SD reader, re-run with"
        echo "      --allow-internal, or use an external SD card reader."
        exit 1
    fi
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
echo "[1/4] Building SD image at $IMG"
python3 "$SCRIPT_DIR/mksdcard.py" --output "$IMG" >/dev/null

if [ ! -f "$IMG" ]; then
    echo "FAIL: mksdcard.py did not produce $IMG"
    exit 1
fi

IMG_MB=$(( $(stat -f %z "$IMG") / 1024 / 1024 ))
echo "      $IMG ($IMG_MB MB)"

# Final confirmation.
echo
echo "[2/4] About to flash:"
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
echo "[3/4] Flashing $TARGET"
diskutil unmountDisk "$TARGET" >/dev/null
sudo dd if="$IMG" of="$RAW" bs=1m
sync

# Post-flash verification: the card FAT kernel8.img must match the source
# image kernel8.img. Catches silent non-flashes (wrong device, a dd that
# did nothing, a stale card still booting an old image).
echo
echo "[4/4] Verifying kernel8.img on card matches source"
diskutil mountDisk "$TARGET" >/dev/null 2>&1 || true
CARD_MNT=$(mount | grep -i "${TARGET}s1 " | awk '{print $3}' | head -1 || true)
[ -z "$CARD_MNT" ] && CARD_MNT="/Volumes/AIOSBOOT"

SRC_OUT=$(hdiutil attach -readonly -nobrowse "$IMG" 2>/dev/null || true)
SRC_DEV=$(printf '%s\n' "$SRC_OUT" | awk 'NR==1{print $1}' || true)
SRC_MNT=$(printf '%s\n' "$SRC_OUT" | grep -o '/Volumes/.*' | head -1 || true)

CARD_H=""
SRC_H=""
if [ -f "$CARD_MNT/kernel8.img" ]; then
    CARD_H=$(shasum -a 256 "$CARD_MNT/kernel8.img" | awk '{print $1}' || true)
fi
if [ -n "$SRC_MNT" ] && [ -f "$SRC_MNT/kernel8.img" ]; then
    SRC_H=$(shasum -a 256 "$SRC_MNT/kernel8.img" | awk '{print $1}' || true)
fi

# Clean up mounts before reporting.
[ -n "$SRC_DEV" ] && hdiutil detach "$SRC_DEV" >/dev/null 2>&1 || true
diskutil eject "$TARGET" >/dev/null 2>&1 || true

if [ -n "$CARD_H" ] && [ -n "$SRC_H" ]; then
    if [ "$CARD_H" = "$SRC_H" ]; then
        echo "      OK: kernel8.img matches ($CARD_H)"
    else
        echo "      MISMATCH -- do NOT boot this card:"
        echo "        card:   $CARD_H"
        echo "        source: $SRC_H"
        exit 1
    fi
else
    echo "      WARN: could not read kernel8.img to verify"
    echo "            (card=$CARD_MNT src=$SRC_MNT). dd reported success;"
    echo "            verification was inconclusive -- check by hand."
fi

echo
echo "OK: flashed $IMG to $TARGET"
echo "    Insert into RPi4, attach serial (115200 baud), power on."
