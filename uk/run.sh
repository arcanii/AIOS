#!/bin/sh
# Build + run the AIOS userspace-kernel first-light inside an aarch64 Linux container.
#
# The Mac host is darwin and cannot ptrace Linux, so we run in colima's aarch64 Linux VM via
# docker. --cap-add=SYS_PTRACE allows PTRACE_SYSEMU + process_vm_readv on the guest. The repo's
# uk/ tree is mounted read-write so build artifacts land back on the host.
#
# Expected: the guest prints its line via the AIOS kernel, then exits through the AIOS ABI with
# code 42 (so "guest exit status: 42" below is SUCCESS, not an error).
set -eu

UK_DIR=$(cd "$(dirname "$0")" && pwd)
IMAGE=${IMAGE:-gcc:13}

docker run --rm --platform linux/arm64 --cap-add=SYS_PTRACE \
    -v "$UK_DIR":/uk -w /uk "$IMAGE" \
    sh -c 'make --no-print-directory clean && make --no-print-directory all &&
           echo "=== M1: guest_hello (WRITE + EXIT) ===" &&
           ./aios-uk ./guest_hello; echo "  [exit $?]";
           echo "=== M2: guest_fileio (VFS: OPEN/WRITE/READ/CLOSE) ===" &&
           ./aios-uk ./guest_fileio; echo "  [exit $?]";
           echo "=== host shell confirms the AIOS program wrote a REAL file: ===";
           ls -l /tmp/aios_m2.txt && cat /tmp/aios_m2.txt;
           echo "=== M3a: guest_cat /etc/hostname (real cat, filename from argv) ===" &&
           ./aios-uk ./guest_cat /etc/hostname; echo "  [exit $?]";
           echo "=== M3b: prog_args (ordinary C: main/printf/malloc/argv via libaios) ===" &&
           ./aios-uk ./prog_args first second; echo "  [exit $?]";
           echo "=== M3c.1: prog_wc -- a real wc on a file, then on host-piped stdin ===" &&
           ./aios-uk ./prog_wc /etc/hostname; echo "  [file exit $?]";
           printf "one two three\nfour five\n" | ./aios-uk ./prog_wc; echo "  [stdin exit $?]";
           echo "=== M3c.2: prog_bigalloc -- real mmap-backed malloc (kernel injects mmap) ===" &&
           ./aios-uk ./prog_bigalloc; echo "  [exit $?]";
           echo "=== M3c.3: prog_tail -n 3 /etc/os-release (fstat + lseek) ===" &&
           ./aios-uk ./prog_tail -n 3 /etc/os-release; echo "  [exit $?]";
           echo "=== M3d.1: prog_exec -- replace my image (AIOS_SYS_EXEC) ===" &&
           ./aios-uk ./prog_exec ./prog_args one two; echo "  [exec->prog_args exit $?]";
           ./aios-uk ./prog_exec /no/such/program; echo "  [exec-failure exit $?]";
           echo "=== M3d.2: prog_spawn -- the shell core (fork -> child exec -> parent waitpid) ===" &&
           ./aios-uk ./prog_spawn ./prog_args hello world; echo "  [spawn exit $?]";
           ./aios-uk ./prog_spawn ./prog_wc /etc/hostname; echo "  [spawn wc exit $?]";
           echo "=== M3d.2 gate: prog_fork -- N children fork/exit/wait (exit 0 iff sum==6) ===" &&
           ./aios-uk ./prog_fork; rc=$?; echo "  [exit $rc]";
           test "$rc" = 0'
