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

**Current version:** v0.4.65

## What Works

- **seL4 microkernel** on AArch64/QEMU (Cortex-A53, 4-core SMP)
- **ext2 filesystem** with read/write, indirect blocks, multi-group allocation
- **VFS layer** with ext2 root mount and procfs at /proc
- **70+ POSIX syscalls** via musllibc shim (open, read, write, fork, exec, pipe, dup2, statx, ...)
- **fork+exec+waitpid** process model with full ELF loading from disk
- **POSIX signals** (sigaction, kill, sigprocmask) with cooperative handler dispatch
- **Unix pipelines** (echo hello | cat | wc -c) with error recovery
- **Shell operators** (&&, ||, >, >>, <) and environment variables
- **128 programs** -- 99 sbase utilities in /bin/, 28 AIOS programs in /bin/aios/, dash in /bin/dash
- **Shared-memory pipes** with mapped frame buffers
- **pthreads** (create, join, mutex) via manual TCB creation in child VSpaces
- **Auth server** with SHA-3-512 (Keccak) passwords, login/logout, su/passwd
- **Kernel log** ring buffer with /proc/log and /proc/uptime
- **POSIX core 55/55 (100%)** -- all core POSIX interfaces implemented
- **PSCI shutdown** -- clean power-off via /bin/aios/shutdown
- **getconf** -- sysconf, confstr, pathconf, limits (99/99 sbase tools)
- **VKA allocator audit** -- per-subsystem resource tracking via debug command

## Architecture

AIOS runs as a single root task on bare seL4 (no Microkit). Server threads
handle IPC-based services; user processes get isolated VSpaces with
capability-mediated access to servers.

    User programs (mini_shell, sbase tools, test programs)
            |
       aios_posix.c  (POSIX shim: 70+ syscalls, signals, pthreads, statx)
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

## Prerequisites

### macOS (Apple Silicon)

Install these via Homebrew:

```bash
# Cross-compiler toolchain
brew install aarch64-unknown-linux-gnu

# Build tools
brew install cmake ninja python3

# QEMU for AArch64 emulation
brew install qemu

# Required by seL4 build system
brew install gnu-sed texinfo dtc libxml2

# Add GNU tools to PATH (required for seL4 kernel build)
export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
export PATH="/opt/homebrew/opt/texinfo/bin:$PATH"
```

### Linux (Ubuntu/Debian)

```bash
sudo apt install gcc-aarch64-linux-gnu cmake ninja-build python3 \
    qemu-system-arm device-tree-compiler libxml2-utils texinfo
```

## Setting Up Dependencies

The seL4 ecosystem libraries are required but gitignored. Clone them into `deps/`:

```bash
cd AIOS
mkdir -p deps

# seL4 kernel (symlinked as deps/kernel)
git clone https://github.com/seL4/seL4.git deps/seL4-kernel
ln -s seL4-kernel deps/kernel

# seL4 libraries
git clone https://github.com/seL4/musllibc.git deps/musllibc
git clone https://github.com/seL4/seL4_libs.git deps/seL4_libs
git clone https://github.com/seL4/seL4_tools.git deps/seL4_tools
git clone https://github.com/seL4/sel4runtime.git deps/sel4runtime
git clone https://github.com/seL4/util_libs.git deps/util_libs

# Symlink for build system
ln -sf deps/seL4_tools/cmake-tool tools/seL4
```

### Required Patches (GCC 15)

GCC 15 changes default symbol visibility, breaking musllibc. Apply these patches
after cloning deps (documented in `docs/patches/`):

**deps/musllibc** -- `vis.h` and `stdio_impl.h`: change `__attribute__((visibility("protected")))`
to `__attribute__((visibility("default")))`.

**deps/util_libs** -- `libplatsupport/src/common.c`: suppress format warning
(documented in `docs/patches/platsupport-warning.md`).

### sbase (Unix utilities)

```bash
# Clone sbase alongside AIOS (same parent directory)
cd ..
git clone https://git.suckless.org/sbase
cd AIOS
```

### dash (optional -- shell port)

```bash
cd ..
git clone https://github.com/tklauser/dash.git
cd dash/src
# Generate required headers (see docs/DASH_PORT.md for details)
```

## Building

### 1. Build kernel + root task + all AIOS programs

```bash
cd AIOS
rm -rf build-04 && mkdir build-04 && cd build-04
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
ninja
```

### 2. Build sbase utilities

```bash
cd AIOS
python3 scripts/build_sbase.py
```

This cross-compiles 99 sbase tools into `build-04/sbase/` using `scripts/aios-cc`.

### 3. Create disk image

```bash
python3 scripts/mkdisk.py disk/disk_ext2.img \
    --rootfs disk/rootfs \
    --install-elfs build-04/sbase \
    --aios-elfs build-04/projects/aios/
```

Creates a 128MB ext2 image with all programs installed.

**Important:** The disk image must be rebuilt whenever programs in `src/apps/`
change (mini_shell, getty, etc.), since these are loaded from disk at runtime.
Only the root task and CPIO-embedded servers (tty_server, auth_server) are
loaded directly from the kernel image.

### 4. Boot

```bash
qemu-system-aarch64 \
    -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G \
    -nographic -serial mon:stdio \
    -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

Login as `root` (password: `root`).

### Incremental Development Cycle

For most source changes, a full rebuild is not needed:

```bash
# After editing server code (pipe_server.c, fork.c, etc.):
cd build-04 && ninja
# These are part of aios_root -- no disk rebuild needed

# After editing app code (mini_shell.c, getty.c, etc.):
cd build-04 && ninja && cd ..
python3 scripts/mkdisk.py disk/disk_ext2.img \
    --rootfs disk/rootfs \
    --install-elfs build-04/sbase \
    --aios-elfs build-04/projects/aios/
# Disk rebuild IS needed -- apps load from ext2
```

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
- [DASH_PORT.md](docs/DASH_PORT.md) -- dash shell porting guide

## Project Status

This is in an experimental/research phase. Collaborators welcome.

The 0.4.x line runs on bare seL4 (single root task, no Microkit).
Earlier branches (0.2.x, 0.3.x) explored Microkit-based designs.

## License

MIT License. See [LICENSE](LICENSE) file.

The sbase utilities are from suckless.org (MIT License).
