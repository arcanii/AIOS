#!/bin/sh
# run_qemu.sh -- boot the minimal AIOS appliance under QEMU (aarch64 'virt'). No KVM needed (TCG),
# so it runs inside the colima container too. Build the artifacts first with build_appliance.sh.
#
# Expected: Linux <6.18> boots on ttyAMA0, /init mounts /proc, sets AIOS_ROOT=/aiosroot, and execs
# the AIOS userspace kernel -> a confined AIOS dash shell. `ls -l /bin` shows the AIOS userland; the
# host filesystem is unreachable. Exit QEMU with Ctrl-A x.
set -eu
APP=$(cd "$(dirname "$0")" && pwd); OUT="$APP/out"
KERNEL=${KERNEL:-$OUT/Image}; INITRD=${INITRD:-$OUT/aios-initramfs.cpio.gz}
MEM=${MEM:-512}; CPU=${CPU:-cortex-a72}

[ -f "$KERNEL" ] || { echo "missing $KERNEL -- run build_appliance.sh first"; exit 1; }
[ -f "$INITRD" ] || { echo "missing $INITRD -- run build_appliance.sh first"; exit 1; }

exec qemu-system-aarch64 \
    -M virt -cpu "$CPU" -smp 1 -m "$MEM" \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -append "console=ttyAMA0 panic=-1 rdinit=/init" \
    -nographic -no-reboot
