#!/usr/bin/env bash
# build_environment.sh -- take a fresh clone to a fully working AIOS build.
#
#   ./build_environment.sh             everything: check tools, fetch pinned
#                                      deps, apply patches, configure, build,
#                                      QEMU smoke test
#   ./build_environment.sh --check     host-tool check only
#   ./build_environment.sh --deps-only stop after deps are fetched + patched
#   ./build_environment.sh --rpi4      also configure + build the RPi4 tree
#   ./build_environment.sh --no-smoke  skip the QEMU boot test
#   ./build_environment.sh --capture-patches
#                                      (maintainer) re-export deps/patches/
#                                      from the current dep working trees
#
# Dependency pins live in DEPS.md (the single source of truth); this script
# greps them from the tables there. Sibling deps (sbase/dash/zsh/tcc/mbedtls)
# default to the directory NEXT TO this repo; override with AIOS_DEPS_ROOT.
# Idempotent: safe to re-run; it never clobbers an existing checkout (it warns
# if one is at the wrong pin or dirty) and re-applies patches only if missing.
set -u

AIOS="$(cd "$(dirname "$0")" && pwd)"
DEPS_ROOT="${AIOS_DEPS_ROOT:-$(dirname "$AIOS")}"
PATCHES="$AIOS/deps/patches"
DEPS_MD="$AIOS/DEPS.md"
OK=0; WARN=0; FAIL=0
say()  { printf '%s\n' "$*"; }
ok()   { printf '  [OK]   %s\n' "$*"; OK=$((OK+1)); }
warn() { printf '  [WARN] %s\n' "$*"; WARN=$((WARN+1)); }
fail() { printf '  [FAIL] %s\n' "$*"; FAIL=$((FAIL+1)); }
die()  { printf 'FATAL: %s\n' "$*" >&2; exit 1; }

DO_RPI4=0; DO_SMOKE=1; ONLY_CHECK=0; ONLY_DEPS=0; CAPTURE=0
for a in "$@"; do case "$a" in
  --rpi4) DO_RPI4=1;; --no-smoke) DO_SMOKE=0;; --check) ONLY_CHECK=1;;
  --deps-only) ONLY_DEPS=1;; --capture-patches) CAPTURE=1;;
  *) die "unknown flag: $a (see the header of this script)";;
esac; done

# ---- pin table (parsed from DEPS.md: "| name | `pin` | url | ...") ---------
pin_of()  { grep -E "^\| $1 \|" "$DEPS_MD" | sed -E 's/^[^`]*`([^`]+)`.*/\1/'; }
repo_of() { grep -E "^\| $1 \|" "$DEPS_MD" | sed -E 's/.*\| (https[^ ]+) \|.*/\1/'; }
SEL4_DEPS="seL4-kernel musllibc seL4_libs seL4_tools sel4runtime util_libs"
SIBLING_DEPS="sbase dash zsh tcc mbedtls"

# ---- maintainer: re-capture the patch set ----------------------------------
if [ "$CAPTURE" = 1 ]; then
  say "== capturing deps/patches from current working trees =="
  mkdir -p "$PATCHES/files"
  git -C "$AIOS/deps/kernel"    diff -- src/plat/bcm2711 tools/dts > "$PATCHES/seL4-kernel.patch"
  git -C "$AIOS/deps/musllibc"  diff > "$PATCHES/musllibc.patch"
  git -C "$AIOS/deps/seL4_libs" diff > "$PATCHES/seL4_libs.patch"
  git -C "$AIOS/deps/seL4_tools" diff -- elfloader-tool > "$PATCHES/seL4_tools.patch"
  git -C "$DEPS_ROOT/tcc"       diff -- arm64-gen.c arm64-link.c > "$PATCHES/tcc.patch"
  git -C "$DEPS_ROOT/mbedtls"   diff -- include/mbedtls/mbedtls_config.h > "$PATCHES/mbedtls.patch"
  cp "$DEPS_ROOT/dash/src/config.h"   "$PATCHES/files/dash-config.h"
  cp "$DEPS_ROOT/zsh/Src/termcap.h"   "$PATCHES/files/zsh-termcap.h"
  cp "$AIOS/deps/seL4_tools/elfloader-tool/include/diag_fb_debug.h" \
     "$PATCHES/files/elfloader-diag_fb_debug.h"
  wc -l "$PATCHES"/*.patch "$PATCHES"/files/* ; exit 0
fi

# ---- phase 1: host tools ----------------------------------------------------
say "== phase 1: host tools =="
CROSS=""
for p in aarch64-linux-gnu- aarch64-unknown-linux-gnu- aarch64-none-linux-gnu-; do
  command -v "${p}gcc" >/dev/null 2>&1 && { CROSS="$p"; break; }
done
if [ -n "$CROSS" ]; then ok "cross compiler: ${CROSS}gcc ($(${CROSS}gcc -dumpversion))"
else fail "aarch64 cross gcc (macOS: brew install aarch64-unknown-linux-gnu | Linux: apt install gcc-aarch64-linux-gnu)"; fi
for t in cmake ninja python3 git; do
  command -v "$t" >/dev/null 2>&1 && ok "$t" || fail "$t (brew/apt install $t)"
done
command -v qemu-system-aarch64 >/dev/null 2>&1 \
  && ok "qemu-system-aarch64 ($(qemu-system-aarch64 --version | head -1 | sed 's/.*version //;s/ .*//'))" \
  || fail "qemu-system-aarch64 (brew install qemu | apt install qemu-system-arm)"
command -v dtc >/dev/null 2>&1 && ok "dtc" || fail "dtc (brew install dtc | apt install device-tree-compiler)"
if [ "$(uname)" = Darwin ]; then
  for f in gnu-sed texinfo; do
    [ -d "/opt/homebrew/opt/$f" ] && ok "$f" || warn "$f missing (brew install $f) -- the seL4 kernel build needs it"
  done
  command -v xmllint >/dev/null 2>&1 && ok "xmllint" || warn "xmllint missing (brew install libxml2)" 
  case ":$PATH:" in *gnu-sed/libexec/gnubin*) ok "gnu-sed on PATH";;
    *) warn 'gnu-sed not on PATH: export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"';; esac
  [ -x /opt/homebrew/opt/e2fsprogs/sbin/mke2fs ] && ok "e2fsprogs (log disk)" \
    || warn "e2fsprogs missing (brew install e2fsprogs) -- optional, enables the log disk"
else
  command -v xmllint >/dev/null 2>&1 || warn "libxml2-utils (apt) -- seL4 kernel build"
  command -v makeinfo >/dev/null 2>&1 || warn "texinfo (apt) -- seL4 kernel build"
  command -v mke2fs >/dev/null 2>&1 || warn "e2fsprogs -- optional, log disk"
fi
[ "$FAIL" -gt 0 ] && die "$FAIL required tool(s) missing -- install them and re-run"
[ "$ONLY_CHECK" = 1 ] && { say "host tools OK ($OK ok, $WARN warnings)"; exit 0; }

# ---- phase 2: fetch deps at their pins --------------------------------------
say "== phase 2: dependencies (pins from DEPS.md; sibling root: $DEPS_ROOT) =="
fetch() { # $1 name  $2 dest-dir
  local name="$1" dest="$2" pin repo cur
  pin="$(pin_of "$name")"; repo="$(repo_of "$name")"
  [ -n "$pin" ] || { fail "$name: no pin found in DEPS.md"; return; }
  if [ ! -d "$dest/.git" ] && ! git -C "$dest" rev-parse --git-dir >/dev/null 2>&1; then
    say "  cloning $name -> $dest"
    git clone "$repo" "$dest" || { fail "$name: clone failed"; return; }
    git -C "$dest" checkout --detach "$pin" || { fail "$name: pin $pin not found"; return; }
    ok "$name @ $pin (fresh)"
  else
    cur="$(git -C "$dest" rev-parse --short=9 HEAD 2>/dev/null)"
    case "$cur" in
      "$pin"*|"${pin:0:7}"*) ok "$name @ $pin (present)";;
      *) case "$pin" in "$cur"*) ok "$name @ $pin (present)";;
         *) warn "$name at $cur, pin is $pin -- leaving YOUR checkout alone (see DEPS.md)";; esac;;
    esac
  fi
}
mkdir -p "$AIOS/deps"
for d in $SEL4_DEPS; do
  case "$d" in
    seL4-kernel)
      # deps/kernel may already point at a kernel checkout (any layout) --
      # only clone our own copy when no kernel is reachable at all.
      if git -C "$AIOS/deps/kernel" rev-parse --git-dir >/dev/null 2>&1; then
        ok "seL4-kernel via existing deps/kernel ($(git -C "$AIOS/deps/kernel" rev-parse --short=9 HEAD))"
      else
        fetch "$d" "$AIOS/deps/seL4-kernel"
      fi;;
    *) fetch "$d" "$AIOS/deps/$d";;
  esac
done
# deps/kernel symlink (respect a pre-existing layout, e.g. a checkout elsewhere)
if [ ! -e "$AIOS/deps/kernel" ]; then
  ln -s seL4-kernel "$AIOS/deps/kernel" && ok "deps/kernel -> seL4-kernel symlink"
else ok "deps/kernel exists ($(readlink "$AIOS/deps/kernel" 2>/dev/null || echo dir))"; fi
# tools/seL4 (cmake-tool) symlink used by the build system
mkdir -p "$AIOS/tools"
if [ ! -e "$AIOS/tools/seL4" ]; then
  ln -s ../deps/seL4_tools/cmake-tool "$AIOS/tools/seL4" && ok "tools/seL4 -> cmake-tool symlink"
else ok "tools/seL4 exists"; fi
for d in $SIBLING_DEPS; do fetch "$d" "$DEPS_ROOT/$d"; done

# ---- phase 3: patches + vendored files (idempotent) -------------------------
say "== phase 3: patch set (deps/patches) =="
apply() { # $1 repo-dir  $2 patch-file
  local dir="$1" p="$PATCHES/$2"
  [ -s "$p" ] || { warn "$2 missing/empty"; return; }
  if git -C "$dir" apply --check "$p" 2>/dev/null; then
    git -C "$dir" apply "$p" && ok "$2 applied" || fail "$2 apply error"
  elif git -C "$dir" apply --reverse --check "$p" 2>/dev/null; then
    ok "$2 already applied"
  else
    warn "$2 does not apply cleanly (tree diverged from its pin?)"
  fi
}
apply "$AIOS/deps/kernel"     seL4-kernel.patch
apply "$AIOS/deps/musllibc"   musllibc.patch
apply "$AIOS/deps/seL4_libs"  seL4_libs.patch
apply "$AIOS/deps/seL4_tools" seL4_tools.patch
apply "$DEPS_ROOT/tcc"        tcc.patch
apply "$DEPS_ROOT/mbedtls"    mbedtls.patch
vendor() { # $1 src-in-patches/files  $2 dest
  if [ -f "$2" ]; then ok "$(basename "$2") present"
  else cp "$PATCHES/files/$1" "$2" && ok "$(basename "$2") installed" || fail "$1 -> $2"; fi
}
vendor dash-config.h            "$DEPS_ROOT/dash/src/config.h"
vendor zsh-termcap.h            "$DEPS_ROOT/zsh/Src/termcap.h"
vendor elfloader-diag_fb_debug.h "$AIOS/deps/seL4_tools/elfloader-tool/include/diag_fb_debug.h"

# dash generated headers (mksyntax & co run on the HOST compiler)
if [ ! -f "$DEPS_ROOT/dash/src/syntax.c" ]; then
  say "  generating dash headers (mksyntax/mknodes/mkinit/mkbuiltins/mksignames)"
  ( cd "$DEPS_ROOT/dash/src" \
    && cc -o mksyntax mksyntax.c && ./mksyntax \
    && cc -o mknodes mknodes.c && ./mknodes nodetypes nodes.c.pat \
    && cc -o mkinit mkinit.c && ./mkinit *.c \
    && sh mkbuiltins shell.h builtins.def 2>/dev/null || true \
    && cc -o mksignames mksignames.c && ./mksignames ) \
    && ok "dash generated sources" || warn "dash header generation had errors (see docs/DASH_PORT.md)"
else ok "dash generated sources present"; fi
[ "$ONLY_DEPS" = 1 ] && { say "deps ready ($OK ok, $WARN warnings)"; exit 0; }

# ---- phase 4: configure -----------------------------------------------------
say "== phase 4: cmake configure =="
configure() { # $1 build-dir  $2... extra -D flags
  local bdir="$1"; shift
  if [ -f "$AIOS/$bdir/build.ninja" ]; then ok "$bdir already configured"; return; fi
  mkdir -p "$AIOS/$bdir"
  ( cd "$AIOS/$bdir" && cmake -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$AIOS/deps/kernel/gcc.cmake" \
      -DCROSS_COMPILER_PREFIX="$CROSS" \
      -DKernelSel4Arch=aarch64 -DKernelMaxNumNodes=4 "$@" .. >/dev/null ) \
    && ok "$bdir configured" || fail "$bdir cmake configure failed"
}
configure build-04 -DKernelPlatform=qemu-arm-virt
[ "$DO_RPI4" = 1 ] && configure build-rpi4 -DKernelPlatform=bcm2711 -DRPI4_MEMORY=4096
[ "$FAIL" -gt 0 ] && die "configure failed"

# ---- phase 5: build ---------------------------------------------------------
say "== phase 5: build (kernel + root task + userspace + disk image) =="
ninja -C "$AIOS/build-04" || die "ninja build-04 failed"
( cd "$AIOS" && python3 scripts/build_apps.py ) || die "build_apps.py failed"
if [ "$DO_RPI4" = 1 ]; then ninja -C "$AIOS/build-rpi4" || die "ninja build-rpi4 failed"; fi
# optional log disk
if [ ! -f "$AIOS/disk/log_ext2.img" ]; then
  MKE2FS="$(command -v mke2fs || echo /opt/homebrew/opt/e2fsprogs/sbin/mke2fs)"
  if [ -x "$MKE2FS" ]; then
    dd if=/dev/zero of="$AIOS/disk/log_ext2.img" bs=1m count=16 2>/dev/null \
      || dd if=/dev/zero of="$AIOS/disk/log_ext2.img" bs=1M count=16 2>/dev/null
    "$MKE2FS" -q -b 1024 -I 128 -t ext2 -L "aios-log" "$AIOS/disk/log_ext2.img" \
      && ok "log disk created" || warn "log disk creation failed (optional)"
  else warn "mke2fs not found -- skipping optional log disk"; fi
else ok "log disk present"; fi

# ---- phase 6: QEMU smoke test ----------------------------------------------
if [ "$DO_SMOKE" = 1 ]; then
  say "== phase 6: QEMU smoke test (boot to login prompt) =="
  SLOG="$(mktemp /tmp/aios-smoke.XXXXXX)"
  qemu-system-aarch64 -machine virt,virtualization=on -cpu cortex-a53 -smp 4 -m 2G \
    -display none -no-reboot -serial "file:$SLOG" \
    -drive "file=$AIOS/disk/disk_ext2.img,format=raw,if=none,id=hd0" \
    -device virtio-blk-device,drive=hd0 \
    -kernel "$AIOS/build-04/images/aios_root-image-arm-qemu-arm-virt" &
  QPID=$!
  BOOTED=0
  for _ in $(seq 1 60); do
    sleep 2
    grep -q "AIOS login:" "$SLOG" 2>/dev/null && { BOOTED=1; break; }
    kill -0 "$QPID" 2>/dev/null || break
  done
  kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
  if [ "$BOOTED" = 1 ]; then ok "QEMU boots to the login prompt"
  else fail "no login prompt within 120s -- serial log: $SLOG"; fi
fi

say ""
say "== done: $OK ok, $WARN warnings, $FAIL failures =="
if [ "$FAIL" = 0 ]; then
  say "Next: python3 scripts/qemu-boot.py   (interactive boot; login root/root)"
  say "RPi4: ./build_environment.sh --rpi4, then python3 scripts/mksdcard.py (see README)"
fi
exit "$FAIL"
