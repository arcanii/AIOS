#!/bin/bash
# Build the E1 bare-metal reproducer -> kernel8.img (standalone, links at 0x80000).
set -e
cd "$(dirname "$0")"

CROSS=aarch64-linux-gnu-
CC=${CROSS}gcc
CFLAGS="-ffreestanding -nostdlib -fno-pic -fno-pie -fno-stack-protector \
  -mgeneral-regs-only -mstrict-align -O2 -Wall -Wextra -std=gnu11"

$CC $CFLAGS -c boot.S -o boot.o
$CC $CFLAGS -c repro.c -o repro.o
${CROSS}ld -T repro.ld -o e1_repro.elf boot.o repro.o
${CROSS}objcopy -O binary e1_repro.elf kernel8.img

SZ=$(stat -f%z kernel8.img 2>/dev/null || stat -c%s kernel8.img)
echo "OK  kernel8.img  ${SZ} bytes"

# --- sanity checks (catch boot-fatal mistakes before flashing) ---
echo "--- checks ---"
# 1. entry branch at 0x0 and "ARMd" magic (41 52 4d 64) at offset 0x30
MAGIC=$(${CROSS}objdump -s -j .text e1_repro.elf | awk '/ 80030 /{print $2$3}' | head -1)
echo "bytes@0x30: ${MAGIC}  (expect ...41524d64 little-endian = 'ARMd')"
# 2. l1_table must be 4KB-aligned (else the MMU walk faults)
L1=$(${CROSS}nm e1_repro.elf | awk '/ l1_table$/{print $1}')
echo "l1_table @ 0x${L1}  (must end in 000)"
# 3. _start at 0x80000
ST=$(${CROSS}nm e1_repro.elf | awk '/ _start$/{print $1}')
echo "_start   @ 0x${ST}  (must be 0000000000080000)"
echo "--- disassembly head ---"
${CROSS}objdump -d e1_repro.elf | sed -n '/<_start>:/,/<repro_main>:/p' | head -30
