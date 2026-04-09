# AIOS (Open Aries)

A research microkernel operating system built on seL4

Architectures / Hardware Supported
- :white_check_mark: AArch64 (QEMU)
- :white_medium_square: AArch64 (Raspberry Pi 5)
- :white_medium_square: X86-64 

## Latest Achievements
- shell is using DASH (https://github.com/tklauser/dash)
- C compiler is TinyCC (https://github.com/TinyCC/tinycc)

## Overview

AIOS is an experimental OS exploring how far POSIX compliance and Unix
design principles can be carried on a formally verified microkernel.
Development follows strict Unix philosophy: real fork+exec+waitpid
process launching, POSIX signals, pipelines, shell operators, and
a growing set of standard utilities.

External AI (Claude) is used as a development tool for code generation
and review. This project is also a study in AI-assisted systems programming.
The long-term goal is self-hosted development within AIOS itself.

**Current version:** v0.4.73

## What Works

- **seL4 microkernel** on AArch64/QEMU (Cortex-A53, 4-core SMP)
- **ext2 filesystem** with read/write, indirect blocks, multi-group allocation
- **VFS layer** with ext2 root mount and procfs at /proc
- **70+ POSIX syscalls** via musllibc shim (open, read, write, fork, exec, pipe, dup2, statx, ...)
- **fork+exec+waitpid** process model with full ELF loading from disk
- **POSIX signals** (sigaction, kill, sigprocmask) with cooperative handler dispatch
- **Unix pipelines** (echo hello | cat | wc -c) with error recovery
- **Shell operators** (&&, ||, >, >>, <) and environment variables
- **dash login shell** -- POSIX shell as primary login shell via /etc/passwd pw_shell
- **131 programs** -- 99 sbase utilities in /bin/, 28 AIOS programs in /bin/aios/, dash, tcc, hello_test
- **tcc compiler** -- cross-compiled TinyCC runs on AIOS, compile-and-run working
- **Dual-drive support** -- system disk + log disk, identified by ext2 volume label
- **File-based logging** -- boot entries with timestamps persisted to /log/aios.log
- **Ctrl-C (SIGINT)** -- two-stage signal delivery: SIGINT first, force-kill on second ^C
- **Shared-memory pipes** with mapped frame buffers (client-side enabled)
- **pthreads** (create, join, mutex) via manual TCB creation in child VSpaces
- **Auth server** with SHA-3-512 (Keccak) passwords, login/logout, su/passwd
- **Kernel log** ring buffer + serial echo + file log (/log/aios.log)
- **POSIX core 55/55 (100%)** -- all core POSIX interfaces implemented
- **PSCI shutdown** -- clean power-off via /bin/aios/shutdown
- **getconf** -- sysconf, confstr, pathconf, limits (99/99 sbase tools)
- **VKA allocator audit** -- per-subsystem resource tracking via debug command

## Architecture

AIOS runs as a single root task on bare seL4 (no Microkit). Server threads
handle IPC-based services; user processes get isolated VSpaces with
capability-mediated access to servers.

    User programs (dash, sbase tools, test programs)
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

### dash (login shell -- required)

```bash
cd ..
git clone https://github.com/tklauser/dash.git
cd dash/src

# Generate required headers on macOS/Linux host
cc -o mksyntax mksyntax.c && ./mksyntax
cc -o mknodes mknodes.c && ./mknodes nodetypes nodes.c.pat
cc -o mkinit mkinit.c && ./mkinit *.c
cc -DSMALL -DJOBS=0 -o mkbuiltins mkbuiltins.c 2>/dev/null || \
  sh mkbuiltins shell.h builtins.def
cc -o mksignames mksignames.c && ./mksignames

# Create config.h for AIOS (see docs/DASH_PORT.md for full details)
# Key defines: JOBS=0, SMALL=1, GLOB_BROKEN=1, stat64->stat mappings
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

### 3. Build dash shell

```bash
cd AIOS
DASH=~/Desktop/github_repos/dash/src
./scripts/aios-cc \
    $DASH/main.c $DASH/eval.c $DASH/parser.c $DASH/expand.c \
    $DASH/exec.c $DASH/jobs.c $DASH/trap.c $DASH/redir.c \
    $DASH/input.c $DASH/output.c $DASH/var.c $DASH/cd.c \
    $DASH/error.c $DASH/options.c $DASH/memalloc.c \
    $DASH/mystring.c $DASH/syntax.c $DASH/nodes.c \
    $DASH/builtins.c $DASH/init.c $DASH/show.c \
    $DASH/arith_yacc.c $DASH/arith_yylex.c \
    $DASH/miscbltin.c $DASH/system.c \
    $DASH/alias.c $DASH/histedit.c $DASH/mail.c $DASH/signames.c \
    $DASH/bltin/test.c $DASH/bltin/printf.c $DASH/bltin/times.c \
    -I $DASH -include $DASH/config.h -DSHELL -DSMALL -DGLOB_BROKEN \
    -o build-04/sbase/dash
```

Dash is the primary login shell. It links against `libaios_posix.a` -- if you
change any `src/lib/posix_*.c` file, you must rebuild dash (ninja does not
rebuild it automatically).

### 4. Create disk image

```bash
python3 scripts/mkdisk.py disk/disk_ext2.img \
    --rootfs disk/rootfs \
    --install-elfs build-04/sbase \
    --aios-elfs build-04/projects/aios/
```

Creates a 128MB ext2 image with all programs installed.

### 4b. Create log disk (optional, enables file logging)

```bash
MKE2FS=/opt/homebrew/opt/e2fsprogs/sbin/mke2fs  # macOS
dd if=/dev/zero of=disk/log_ext2.img bs=1M count=16
$MKE2FS -b 1024 -I 128 -t ext2 -L "aios-log" disk/log_ext2.img
```

Creates a 16MB ext2 log disk. At boot, AIOS identifies it by volume label
("aios-log") and mounts at /log. Boot entries are written to /log/aios.log.

**Important:** The disk image must be rebuilt whenever programs in `src/apps/`
change (mini_shell, getty, etc.), since these are loaded from disk at runtime.
Only the root task and CPIO-embedded servers (tty_server, auth_server) are
loaded directly from the kernel image.

### 5. Boot

```bash
qemu-system-aarch64 \
    -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G \
    -nographic -serial mon:stdio \
    -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -drive file=disk/log_ext2.img,format=raw,if=none,id=hd1 \
    -device virtio-blk-device,drive=hd1 \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

The log drive is optional -- if absent, boot prints "No log drive" and
continues without file logging. Drive order does not matter (identified
by volume label).

Login as `root` (password: `root`). The login shell is dash (configured
in `/etc/passwd`).

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
- [DESIGN_NET.md](docs/DESIGN_NET.md) -- Networking subsystem design (virtio-net)
- [DESIGN_TCC.md](docs/DESIGN_TCC.md) -- tcc self-hosted compilation design
- [DESIGN_ZSH.md](docs/DESIGN_ZSH.md) -- zsh shell port design

## Project Status

This is in an experimental/research phase. Collaborators welcome.

The 0.4.x line runs on bare seL4 (single root task, no Microkit).
Earlier branches (0.2.x, 0.3.x) explored Microkit-based designs.

## License

MIT License. See [LICENSE](LICENSE) file.

The sbase utilities are from suckless.org (MIT License).
