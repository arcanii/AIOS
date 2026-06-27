#!/bin/sh
# build_appliance.sh -- assemble the minimal "Linux boots straight into AIOS" appliance.
#
# Produces two artifacts in appliance/out/:
#   Image                  -- a minimal Linux (default 6.18.x) kernel for QEMU 'virt' aarch64
#   aios-initramfs.cpio.gz -- an initramfs holding ONLY: /init (the PID-1 launcher), /aios-uk
#                             (the AIOS userspace kernel, static), and /aiosroot (the AIOS userland)
# then `appliance/run_qemu.sh` boots it.  Linux is the thin substrate; AIOS is the kernel on top.
#
# Run it INSIDE the aarch64 Linux build env (colima container as root, or the Pi).  Native aarch64 --
# no cross-compile.  See docs/AIOS_KERNEL_DEPENDENCIES.md for what the kernel config must provide.
#
# Env knobs:
#   KVER=6.18.1     kernel version to fetch + build (default: latest 6.18.x resolved from kernel.org)
#   BASE=defconfig  config base: 'defconfig' (reliable boot) or 'tinyconfig' (strict-minimal aspiration)
#   PAL=linux       which PAL aios-uk is built with (linux | seccomp)
#   SKIP_KERNEL=1   skip the kernel build (reuse appliance/out/Image) -- just rebuild the initramfs
#   JOBS=$(nproc)   parallelism for the kernel build
set -eu

UK=$(cd "$(dirname "$0")/.." && pwd)        # the uk/ tree
APP="$UK/appliance"
OUT="$APP/out"; BLD="$APP/build"
KVER=${KVER:-6.18}; BASE=${BASE:-defconfig}; PAL=${PAL:-linux}; JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
mkdir -p "$OUT" "$BLD"

echo "== 1/5  build the AIOS userland (dash + sbase + guests + aios-uk) =="
make -C "$UK" --no-print-directory all >/dev/null

echo "== 2/5  static host binaries (no libc in the initramfs) =="
# aios-uk static, with the chosen PAL backend (same source list as the Makefile's KERNEL_SRC).
( cd "$UK" && cc -O2 -Wall -Iinclude -static -o "$BLD/aios-uk" kernel/aios_kernel.c "pal/pal_$PAL.c" )
cc -O2 -static -o "$BLD/aios_init" "$APP/aios_init.c"

echo "== 3/5  build the AIOS root filesystem (mkaiosroot.sh) =="
rm -rf "$BLD/aiosroot"
( cd "$UK" && sh mkaiosroot.sh "$BLD/aiosroot" >/dev/null )

echo "== 4/5  assemble the initramfs cpio =="
# Stage on a LOCAL fs, not the (virtiofs) mount -- virtiofs cannot hold the /dev device nodes (mknod
# fails there even as root). We build the cpio locally, then write only the regular .cpio.gz to $OUT.
ROOT=$(mktemp -d); trap 'rm -rf "$ROOT"' EXIT
mkdir -p "$ROOT"/proc "$ROOT"/sys "$ROOT"/dev
cp "$BLD/aios_init" "$ROOT/init"
cp "$BLD/aios-uk"   "$ROOT/aios-uk"
cp -a "$BLD/aiosroot" "$ROOT/aiosroot"
# device nodes (mknod needs root + a real fs -- true on the local layer; on the Pi run via sudo, or
# point CONFIG_INITRAMFS_SOURCE at a gen_init_cpio spec to encode the nodes without mknod).
mknod "$ROOT/dev/console" c 5 1 || echo "  (warn: mknod /dev/console failed -- run as root)"
mknod "$ROOT/dev/null"    c 1 3 || true
( cd "$ROOT" && find . | cpio --quiet -o -H newc | gzip -9 ) > "$OUT/aios-initramfs.cpio.gz"
echo "  -> $OUT/aios-initramfs.cpio.gz ($(wc -c < "$OUT/aios-initramfs.cpio.gz") bytes)"

if [ "${SKIP_KERNEL:-0}" = 1 ]; then echo "== 5/5  SKIP_KERNEL=1: reusing $OUT/Image =="; exit 0; fi

echo "== 5/5  fetch + build minimal Linux $KVER (this is the slow step) =="
SRC="$BLD/linux-$KVER"
if [ ! -d "$SRC" ]; then
    TARBALL="linux-$KVER.tar.xz"
    URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/$TARBALL"
    echo "  downloading $URL"
    ( cd "$BLD" && { wget -q "$URL" || curl -fsSLO "$URL"; } && tar xf "$TARBALL" )
fi
make -C "$SRC" ARCH=arm64 "$BASE" >/dev/null
"$SRC/scripts/kconfig/merge_config.sh" -m -O "$SRC" "$SRC/.config" "$APP/aios.config" >/dev/null
make -C "$SRC" ARCH=arm64 olddefconfig >/dev/null
echo "  building Image -j$JOBS ..."
make -C "$SRC" ARCH=arm64 Image -j"$JOBS"
cp "$SRC/arch/arm64/boot/Image" "$OUT/Image"
echo "  -> $OUT/Image ($(wc -c < "$OUT/Image") bytes)"

echo
echo "DONE.  Boot it:  sh appliance/run_qemu.sh"
