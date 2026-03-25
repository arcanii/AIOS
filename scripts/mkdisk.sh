#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════
# AIOS Disk Image Builder
#
# Creates a FAT16 disk image with test files and optional
# model/tokenizer binaries for the AIOS project.
#
# Usage:
#   ./scripts/mkdisk.sh                    # default 64 MiB
#   ./scripts/mkdisk.sh -s 128             # 128 MiB image
#   ./scripts/mkdisk.sh -o my_disk.img     # custom output name
#   ./scripts/mkdisk.sh -m stories42M.bin  # different model
#
# Requires: mtools (brew install mtools / apt install mtools)
#
# Copyright (c) 2025 AIOS Project
# SPDX-License-Identifier: MIT
# ══════════════════════════════════════════════════════════

set -euo pipefail

# ── Defaults ─────────────────────────────────────────────
DISK_IMG="disk.img"
SIZE_MB=64
MODEL_BIN="stories15M.bin"
MODEL_FAT="STORIES.BIN"
TOK_BIN="tokenizer.bin"
TOK_FAT="TOK.BIN"
HELLO_TEXT="Hello from AIOS disk! This is our FAT filesystem."

# ── Parse arguments ──────────────────────────────────────
usage() {
    echo "Usage: $0 [-o output.img] [-s size_mb] [-m model.bin] [-t tokenizer.bin] [-h]"
    echo ""
    echo "Options:"
    echo "  -o FILE    Output image filename (default: disk.img)"
    echo "  -s SIZE    Image size in MiB (default: 64)"
    echo "  -m FILE    Model binary to include (default: stories15M.bin)"
    echo "  -t FILE    Tokenizer binary to include (default: tokenizer.bin)"
    echo "  -h         Show this help"
    exit 1
}

while getopts "o:s:m:t:h" opt; do
    case $opt in
        o) DISK_IMG="$OPTARG" ;;
        s) SIZE_MB="$OPTARG" ;;
        m) MODEL_BIN="$OPTARG" ;;
        t) TOK_BIN="$OPTARG" ;;
        h) usage ;;
        *) usage ;;
    esac
done

# ── Check dependencies ───────────────────────────────────
for cmd in dd mformat mcopy mdir; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "ERROR: '$cmd' not found. Install mtools:"
        echo "  macOS:  brew install mtools"
        echo "  Ubuntu: sudo apt install mtools"
        exit 1
    fi
done

# ── Compute geometry ─────────────────────────────────────
# FAT16 with 512-byte sectors, 16 heads, 63 sectors/track
HEADS=16
SECTORS=63
BYTES_PER_SECTOR=512
BYTES_PER_TRACK=$((HEADS * SECTORS * BYTES_PER_SECTOR))
TOTAL_BYTES=$((SIZE_MB * 1024 * 1024))
TRACKS=$((TOTAL_BYTES / BYTES_PER_TRACK))

echo "════════════════════════════════════════════"
echo "  AIOS Disk Image Builder"
echo "════════════════════════════════════════════"
echo "  Output:   $DISK_IMG"
echo "  Size:     ${SIZE_MB} MiB"
echo "  Geometry: t=${TRACKS} h=${HEADS} s=${SECTORS}"
echo ""

# ── Remove old image ─────────────────────────────────────
if [ -f "$DISK_IMG" ]; then
    echo "  Removing existing $DISK_IMG..."
    rm -f "$DISK_IMG"
fi

# ── Create blank image ───────────────────────────────────
echo "  Creating ${SIZE_MB} MiB blank image..."
dd if=/dev/zero of="$DISK_IMG" bs=1M count="$SIZE_MB" status=none

# ── Format as FAT16 ─────────────────────────────────────
echo "  Formatting as FAT16..."
mformat -i "$DISK_IMG" \
    -t "$TRACKS" \
    -h "$HEADS" \
    -s "$SECTORS" \
    -M "$BYTES_PER_SECTOR" \
    -v AIOS ::

# ── Add hello.txt ────────────────────────────────────────
echo "  Adding hello.txt..."
printf "%s\n" "$HELLO_TEXT" | mcopy -i "$DISK_IMG" - ::hello.txt

# ── Add model binary (if present) ────────────────────────
if [ -f "$MODEL_BIN" ]; then
    MODEL_SIZE=$(stat -f%z "$MODEL_BIN" 2>/dev/null || stat -c%s "$MODEL_BIN" 2>/dev/null)
    MODEL_SIZE_MB=$((MODEL_SIZE / 1024 / 1024))
    echo "  Adding $MODEL_BIN -> $MODEL_FAT (${MODEL_SIZE_MB} MiB)..."

    # Check if model fits
    if [ "$MODEL_SIZE" -gt "$((TOTAL_BYTES - 1024 * 1024))" ]; then
        echo "  WARNING: model may not fit in ${SIZE_MB} MiB image!"
        echo "  Try: $0 -s $((MODEL_SIZE_MB + 16))"
    fi

    mcopy -i "$DISK_IMG" "$MODEL_BIN" ::"$MODEL_FAT"
else
    echo "  $MODEL_BIN not found (skipping model)"
    echo "  Download with:"
    echo "    curl -L -o $MODEL_BIN \\"
    echo "      https://huggingface.co/karpathy/tinyllamas/resolve/main/$MODEL_BIN"
fi

# ── Add tokenizer binary (if present) ───────────────────
if [ -f "$TOK_BIN" ]; then
    TOK_SIZE=$(stat -f%z "$TOK_BIN" 2>/dev/null || stat -c%s "$TOK_BIN" 2>/dev/null)
    TOK_SIZE_KB=$((TOK_SIZE / 1024))
    echo "  Adding $TOK_BIN -> $TOK_FAT (${TOK_SIZE_KB} KiB)..."
    mcopy -i "$DISK_IMG" "$TOK_BIN" ::"$TOK_FAT"
else
    echo "  $TOK_BIN not found (skipping tokenizer)"
    echo "  Download with:"
    echo "    curl -L -o $TOK_BIN \\"
    echo "      https://github.com/karpathy/llama2.c/raw/master/tokenizer.bin"
fi

# ── Show contents ────────────────────────────────────────
echo ""
echo "  Disk contents:"
echo "  ────────────────────────────────────────"
mdir -i "$DISK_IMG" :: | sed 's/^/  /'
echo ""
echo "  Done! Image ready at: $DISK_IMG"
echo "════════════════════════════════════════════"

