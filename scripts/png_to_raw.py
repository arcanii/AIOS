#!/usr/bin/env python3
"""
png_to_raw.py -- Convert PNG/JPEG to AIOS raw framebuffer format

Output format (aios_raw_img):
  Bytes  0-3:  width  (uint32 LE)
  Bytes  4-7:  height (uint32 LE)
  Bytes  8-11: format (uint32 LE, 0 = XRGB8888)
  Bytes 12-15: reserved (0)
  Bytes 16+:   width * height * 4 bytes XRGB pixel data

Usage:
  python3 scripts/png_to_raw.py input.png [-o output.raw] [--resize WxH]
  python3 scripts/png_to_raw.py input.png --resize 1024x768
  python3 scripts/png_to_raw.py input.png  # default: disk/rootfs/images/splash.raw

Requires: pip3 install Pillow
"""
import sys
import os
import struct
import argparse

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow not installed. Run: pip3 install Pillow")
    sys.exit(1)

def main():
    parser = argparse.ArgumentParser(description="Convert image to AIOS raw format")
    parser.add_argument("input", help="Input image file (PNG, JPEG, BMP, etc.)")
    parser.add_argument("-o", "--output", default=None, help="Output .raw file")
    parser.add_argument("--resize", default=None, help="Resize to WxH (e.g. 1024x768)")
    args = parser.parse_args()

    if args.output is None:
        repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        args.output = os.path.join(repo, "disk", "rootfs", "images", "splash.raw")

    img = Image.open(args.input)

    if args.resize:
        w, h = args.resize.split("x")
        img = img.resize((int(w), int(h)), Image.LANCZOS)

    img = img.convert("RGBA")
    width, height = img.size

    os.makedirs(os.path.dirname(args.output), exist_ok=True)

    with open(args.output, "wb") as f:
        # 16-byte header
        f.write(struct.pack("<III I", width, height, 0, 0))
        # Pixel data: convert RGBA to XRGB8888 (0x00RRGGBB)
        for y in range(height):
            for x in range(width):
                r, g, b, a = img.getpixel((x, y))
                f.write(struct.pack("<I", (r << 16) | (g << 8) | b))

    raw_size = 16 + width * height * 4
    print(f"Converted: {args.input} -> {args.output}")
    print(f"  {width}x{height} XRGB8888, {raw_size} bytes ({raw_size // 1024} KB)")

if __name__ == "__main__":
    main()
