# AIOS (Open Aries)

A research microkernel operating system built on seL4, targeting AArch64.

## Overview

AIOS is an experimental OS exploring how far POSIX compliance and Unix
design principles can be carried on a formally verified microkernel.
Development follows strict Unix philosophy: real fork+exec+waitpid
process launching, POSIX signals, pipelines, shell operators, and
a growing set of standard utilities.

External AI (Claude) is used as a development tool for code generation
and review. This project is also a study in AI-assisted systems programming.
The long-term goal is self-hosted development within AIOS itself.

**Current version:** v0.4.54

## What Works

- **seL4 microkernel** on AArch64/QEMU (Cortex-A53, 4-core SMP)
- **ext2 filesystem** with read/write, indirect blocks, multi-group allocation
- **VFS layer** with ext2 root mount and procfs at /proc
- **55+ POSIX syscalls** via musllibc shim (open, read, write, fork, exec, pipe, dup2, ...)
- **fork+exec+waitpid** process model with full ELF loading from disk
- **POSIX signals** (sigaction, kill, sigprocmask) with cooperative handler dispatch
- **Unix pipelines** (echo hello | cat | wc -c) with error recovery
- **Shell operators** (&&, ||, >, <) and environment variables
- **100+ programs** in /bin/ (79 sbase utilities + AIOS tools)
- **pthreads** (create, join, mutex) via manual TCB creation in child VSpaces
- **Auth server** with SHA-3-512 (Keccak) passwords, login/logout, su/passwd
- **Kernel log** ring buffer with /proc/log and /proc/uptime

## Architecture

AIOS runs as a single root task on bare seL4 (no Microkit). Server threads
handle IPC-based services; user processes get isolated VSpaces with
capability-mediated access to servers.

    User programs (mini_shell, sbase tools, test programs)
            |
       aios_posix.c  (POSIX shim: 55+ syscalls, signals, pthreads)
            |
       +------------+------------+------------+------------+
       |            |            |            |            |
    pipe_server  exec_server  fs_server  thread_srv  auth_server
     (pipes,     (ELF load,   (VFS       (pthreads)  (SHA-3-512,
      fork,       process      dispatch,               users,
      exec,       lifecycle)   ext2 I/O)               sessions)
      wait,
      signals)
            |
       +----------+----------+
       |          |          |
     ext2.c    procfs.c    vfs.c
       |
    virtio-blk driver
       |
    seL4 microkernel (AArch64, 4-core SMP)
       |
    QEMU virt / Raspberry Pi

## Development Environment

- **Host:** macOS (Apple Silicon M3 Max)
- **Cross-compiler:** aarch64-linux-gnu-gcc (Homebrew, GCC 15)
- **Build system:** CMake + Ninja
- **Scripts:** Python 3 (file edits, disk builder, sbase builder)
- **Target:** qemu-system-aarch64 (virt machine, Cortex-A53)

## Quick Start

Prerequisites: seL4 dependencies, AArch64 cross-compiler, QEMU, Python 3.

    # Build
    cd ~/Desktop/github_repos/AIOS
    rm -rf build-04 && mkdir build-04 && cd build-04
    cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
        -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
    ninja

    # Build sbase utilities
    cd ~/Desktop/github_repos/AIOS
    python3 scripts/build_sbase.py

    # Create disk image
    python3 scripts/mkdisk.py disk/disk_ext2.img 128 \
        --rootfs disk/rootfs \
        --install-elfs build-04/sbase \
        --install-elfs build-04/projects/aios/

    # Boot
    qemu-system-aarch64 \
        -machine virt,virtualization=on \
        -cpu cortex-a53 -smp 4 -m 2G \
        -nographic -serial mon:stdio \
        -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
        -device virtio-blk-device,drive=hd0 \
        -kernel build-04/images/aios_root-image-arm-qemu-arm-virt

## Hardware Targets

- **Development:** QEMU virt (AArch64) -- current platform
- **Primary:** Raspberry Pi 4/5 (BCM2711/BCM2712, AArch64)
- **Future:** x86-64

## Design Philosophy

- Pure POSIX: no alias tables, no prefix stripping, no magic
- Strict Unix philosophy: shell searches PATH, sends full path to exec
- Correctness over performance (research OS)
- Modular server architecture with small source files
- Everything is IPC: all syscalls route through capability-protected endpoints

## Documentation

- [AI_BRIEFING.md](docs/AI_BRIEFING.md) -- Project context for AI sessions
- [ARCHITECTURE.md](docs/ARCHITECTURE.md) -- System design
- [DESIGN_0.4.md](docs/DESIGN_0.4.md) -- 0.4.x design decisions
- [LEARNINGS.md](docs/LEARNINGS.md) -- Hard-won lessons from seL4 development

## Project Status

This is in an experimental/research phase. Collaborators welcome.

The 0.4.x line runs on bare seL4 (single root task, no Microkit).
Earlier branches (0.2.x, 0.3.x) explored Microkit-based designs.

## License

MIT License. See [LICENSE](LICENSE) file.

The sbase utilities are from suckless.org (MIT License).
