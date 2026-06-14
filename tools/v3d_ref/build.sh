#!/bin/sh
# Build the host golden-CL generator (self-contained in the repo).
#
# OUR shim include dir (-I shim) MUST come BEFORE the reference src dir so that
# our deterministic v3d_memory.hpp substitutes for the kernel one. All OTHER headers
# under ref_src/ (cl_emitter.hpp, v3d_cl.hpp, types.hpp, bit_utils.hpp,
# mbox_message.hpp) are the UNMODIFIED Random06457 originals (MIT, see ref_src/LICENSE).
#
# Originally driven off /tmp/random06457 + /tmp/v3d_ref; the needed reference headers
# were preserved into ref_src/ so the clear/triangle goldens regenerate from the repo
# without re-cloning. Regenerate: sh build.sh && ./gen ; decode: python3 decode.py
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REF="$HERE/ref_src"
SHIM="$HERE/shim"
clang++ -std=c++20 -O0 -g \
    -I "$SHIM" \
    -I "$REF" \
    -I "$REF/device/v3d" \
    -o "$HERE/gen" \
    "$HERE/gen.cpp"
echo "build OK"
