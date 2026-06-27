#!/bin/sh
# mkaiosroot.sh -- build an AIOS root filesystem: the self-contained world a confined AIOS guest sees.
#
# AIOS is a userspace kernel (it runs as a process on the host Linux), so this is NOT a bootable
# kernel image -- it is the AIOS USERLAND: the AIOS-ABI binaries + config that the AIOS kernel serves
# and CONFINES. Launched with AIOS_ROOT set, every guest path resolves inside this tree
# (openat2 RESOLVE_IN_ROOT) and a guest can only exec binaries inside it, so the shell + utilities see
# only this image, never the host filesystem.
#
# Run the resulting image as a confined AIOS system. NOTE: the INIT binary is the trusted entry and is
# loaded by its HOST path (so name the image's shell by its real path); every path the shell resolves
# AFTER that is confined to AIOS_ROOT.
#   AIOS_ROOT="$PWD/aiosroot" PATH=/bin ./aios-uk "$PWD/aiosroot/bin/sh"
# or ship it: the script also writes aiosroot.tar (untar onto any host, point AIOS_ROOT at it).
set -eu

UK=$(cd "$(dirname "$0")" && pwd)
ROOT="${1:-$UK/aiosroot}"

# the binaries must be built first (make all)
test -x "$UK/dash" || { echo "build first: make all (need ./dash + ./sbase-*)" >&2; exit 1; }

rm -rf "$ROOT"
mkdir -p "$ROOT/bin" "$ROOT/etc" "$ROOT/tmp"

# the shell: dash as /bin/sh AND /bin/dash
cp "$UK/dash" "$ROOT/bin/dash"
cp "$UK/dash" "$ROOT/bin/sh"

# the coreutils (vendored sbase, built as sbase-<name>) installed at their standard names
for u in true false echo cat wc mkdir rm ls head tail cp mv ln chmod sort grep; do
	cp "$UK/sbase-$u" "$ROOT/bin/$u"
done

# a minimal passwd/group database (so ls -l and grep show real names inside the image)
cat > "$ROOT/etc/passwd" <<'EOF'
root:x:0:0:root:/:/bin/sh
aios:x:1000:1000:AIOS user:/home/aios:/bin/sh
EOF
cat > "$ROOT/etc/group" <<'EOF'
root:x:0:
aios:x:1000:
EOF

# a tiny motd so the image is identifiable
echo "AIOS userspace -- a confined world served by the AIOS userspace kernel." > "$ROOT/etc/motd"

# a shippable archive of the image (an ext4 image is also possible: the host mounts it, then points
# AIOS_ROOT at the mountpoint -- AIOS confines via a dir fd, the host owns the actual filesystem).
( cd "$ROOT/.." && tar cf "$UK/aiosroot.tar" "$(basename "$ROOT")" )

echo "built AIOS root image:"
echo "  tree:    $ROOT  ($(find "$ROOT" -type f | wc -l | tr -d ' ') files, $(du -sh "$ROOT" 2>/dev/null | cut -f1))"
echo "  archive: $UK/aiosroot.tar"
echo "  run:     AIOS_ROOT=\"$ROOT\" PATH=/bin $UK/aios-uk \"$ROOT/bin/sh\""
