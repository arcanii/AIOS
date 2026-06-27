#!/bin/sh
# run.sh -- run the AIOS userspace-kernel acceptance gate inside colima's aarch64 Linux VM.
#
# The Mac host is darwin and cannot ptrace Linux, so we run in an aarch64 `gcc:13` container via
# docker (--cap-add=SYS_PTRACE allows ptrace + process_vm_readv on the guests). The uk/ tree is
# mounted read-write so build artifacts land back on the host.
#
# The gate itself lives in the NATIVE script gate.sh (no docker assumptions) so the SAME gate runs
# directly on the RPi4 ( ssh pi@raspberrypi.local 'cd ~/uk && sh gate.sh' ). It builds the suite and
# runs the 16-key gate TWICE -- PAL=linux then PAL=seccomp -- and exits 0 iff BOTH backends pass
# (the portability proof: one host-agnostic kernel, two host trap mechanisms). stdbuf -oL = a
# real-time log (the inner shell is otherwise block-buffered, which hides where a hang is).
set -eu

UK_DIR=$(cd "$(dirname "$0")" && pwd)
IMAGE=${IMAGE:-gcc:13}

docker run --rm --platform linux/arm64 --cap-add=SYS_PTRACE \
    -v "$UK_DIR":/uk -w /uk "$IMAGE" \
    stdbuf -oL sh /uk/gate.sh
