#!/bin/sh
# Build tcc from individual source files (avoids ONE_SOURCE memory pressure)
# Run inside AIOS: sh /tmp/build_tcc.sh

S=/usr/src/tcc
O=/tmp
CF="-DONE_SOURCE=0 -D_Thread_local= -DTCC_TARGET_ARM64 -DCONFIG_TCC_STATIC -I $S -I $S/include -nostdinc -I /usr/include"

echo "=== TCC self-host build ==="

for f in libtcc tccpp tccgen tccdbg tccelf tccasm tccrun tcctools arm64-gen arm64-link arm64-asm; do
    echo "  CC $f.c"
    tcc -c $CF $S/$f.c -o $O/$f.o
    if test $? -ne 0; then
        echo "FAIL: $f.c"
        exit 1
    fi
done

echo "  CC tcc.c"
tcc -c $CF $S/tcc.c -o $O/tcc_main.o
if test $? -ne 0; then
    echo "FAIL: tcc.c"
    exit 1
fi

echo "  LINK tcc2"
tcc $O/tcc_main.o $O/libtcc.o $O/tccpp.o $O/tccgen.o $O/tccdbg.o $O/tccelf.o $O/tccasm.o $O/tccrun.o $O/tcctools.o $O/arm64-gen.o $O/arm64-link.o $O/arm64-asm.o -o $O/tcc2
if test $? -ne 0; then
    echo "FAIL: link"
    exit 1
fi

echo "=== Testing tcc2 ==="
echo "int main(){printf(\"hello from tcc2\\n\");return 0;}" > $O/t.c
$O/tcc2 -o $O/t $O/t.c
$O/t

echo "=== DONE ==="
