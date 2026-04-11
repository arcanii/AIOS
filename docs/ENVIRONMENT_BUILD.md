# AIOS Build Environment Setup

Step-by-step guide to set up a development environment for AIOS from scratch.

Tested on macOS (Apple Silicon M3/M4) and Ubuntu 22.04+.

## 1. Host Prerequisites

### macOS (Homebrew)

```bash
# Cross-compiler for AArch64
brew install aarch64-unknown-linux-gnu

# Build system
brew install cmake ninja python3

# QEMU for AArch64 emulation
brew install qemu

# Required by seL4 kernel build system
brew install gnu-sed texinfo dtc libxml2

# Optional: log disk creation
brew install e2fsprogs

# Optional: splash image conversion
pip3 install Pillow
```

Add GNU tools to your shell profile (`~/.zshrc` or `~/.bashrc`):

```bash
export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
export PATH="/opt/homebrew/opt/texinfo/bin:$PATH"
```

Reload: `source ~/.zshrc`

### Linux (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install gcc-aarch64-linux-gnu cmake ninja-build python3 \
    qemu-system-arm device-tree-compiler libxml2-utils texinfo \
    e2fsprogs
```

### Verify Toolchain

```bash
aarch64-linux-gnu-gcc --version    # Should show GCC 14+ or 15+
cmake --version                     # 3.16+
ninja --version                     # 1.10+
qemu-system-aarch64 --version      # 8.0+
gsed --version 2>/dev/null || sed --version  # GNU sed required
```

## 2. Clone Repositories

All repos should live under the same parent directory (e.g., `~/Desktop/github_repos/`):

```bash
WORK=~/Desktop/github_repos
mkdir -p $WORK && cd $WORK

# Main AIOS repository
git clone https://github.com/arcanii/AIOS.git
cd AIOS
```

## 3. Set Up seL4 Dependencies

seL4 ecosystem libraries are gitignored. Clone them into `deps/`:

```bash
cd $WORK/AIOS
mkdir -p deps

# seL4 kernel
git clone https://github.com/seL4/seL4.git deps/seL4-kernel
ln -s seL4-kernel deps/kernel

# C library (musl, seL4 port)
git clone https://github.com/seL4/musllibc.git deps/musllibc

# seL4 core libraries
git clone https://github.com/seL4/seL4_libs.git deps/seL4_libs

# CMake build infrastructure
git clone https://github.com/seL4/seL4_tools.git deps/seL4_tools

# Runtime startup code
git clone https://github.com/seL4/sel4runtime.git deps/sel4runtime

# Utility libraries (libfdt, libelf, libcpio, libplatsupport, libutils)
git clone https://github.com/seL4/util_libs.git deps/util_libs

# Create required build-system symlink
mkdir -p tools
ln -sf ../deps/seL4_tools/cmake-tool tools/seL4
```

### Verify deps structure

```bash
ls deps/
# Expected: kernel@ musllibc/ seL4-kernel/ seL4_libs/ seL4_tools/
#           sel4runtime/ util_libs/

ls -l deps/kernel
# Expected: symlink -> seL4-kernel

ls tools/seL4
# Expected: symlink -> deps/seL4_tools/cmake-tool content
```

## 4. Apply Required Patches

GCC 15 changes default symbol visibility, breaking musllibc. These patches
are also documented in `docs/patches/`.

### musllibc visibility fix

```bash
cd $WORK/AIOS

# vis.h: protected -> default
sed -i 's/visibility push(protected)/visibility push(default)/' \
    deps/musllibc/src/internal/vis.h

# stdio_impl.h: protected -> default
sed -i 's/visibility("protected")/visibility("default")/' \
    deps/musllibc/src/internal/stdio_impl.h
```

On macOS, use `sed -i ''` instead of `sed -i` (or ensure GNU sed is in PATH).

### libsel4platsupport warning suppression

```bash
# Suppress noisy format warning in platsupport common.c
sed -i 's/seL4_DebugPutString.*Warning.*printf.*/\/\/ &/' \
    deps/seL4_libs/libsel4platsupport/src/common.c
```

### Verify patches

```bash
grep "visibility push" deps/musllibc/src/internal/vis.h
# Expected: __attribute__((visibility push(default)))

grep "visibility" deps/musllibc/src/internal/stdio_impl.h
# Expected: __attribute__((visibility("default")))
```

## 5. Clone External Tools

### sbase (99 Unix utilities)

```bash
cd $WORK
git clone https://git.suckless.org/sbase
```

### dash (POSIX login shell)

```bash
cd $WORK
git clone https://github.com/tklauser/dash.git
cd dash/src

# Generate required headers on the host system
cc -o mksyntax mksyntax.c && ./mksyntax
cc -o mknodes mknodes.c && ./mknodes nodetypes nodes.c.pat
cc -o mkinit mkinit.c && ./mkinit *.c
cc -DSMALL -DJOBS=0 -o mkbuiltins mkbuiltins.c 2>/dev/null || \
    sh mkbuiltins shell.h builtins.def
cc -o mksignames mksignames.c && ./mksignames
```

Verify that these files were generated:

```bash
ls syntax.h nodes.h nodes.c builtins.h builtins.c init.c signames.c
```

The `config.h` file must also exist. See `docs/DASH_PORT.md` for the full
config.h contents. Key defines: `JOBS=0`, `SMALL=1`, `GLOB_BROKEN=1`.

## 6. Full Build

### Step 1: Build kernel + root task + AIOS programs

```bash
cd $WORK/AIOS
rm -rf build-04 && mkdir build-04 && cd build-04
cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- \
    ..
ninja
```

This builds:
- seL4 kernel (AArch64, Cortex-A53, 4-core SMP)
- Root task with all server threads (pipe, exec, fs, thread, auth, net, display)
- CPIO-embedded servers (tty_server, auth_server)
- All AIOS user programs (29 programs in projects/aios/)
- Final kernel image: `build-04/images/aios_root-image-arm-qemu-arm-virt`

### Step 2: Build sbase utilities

```bash
cd $WORK/AIOS
python3 scripts/build_sbase.py --jobs 16
```

Cross-compiles 99 sbase tools into `build-04/sbase/` using the `scripts/aios-cc`
wrapper. The wrapper provides all seL4 headers, musl libc, and the AIOS POSIX shim.

### Step 3: Build dash shell

```bash
cd $WORK/AIOS
DASH=$WORK/dash/src
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

**Important:** Dash links against `libaios_posix.a`. Ninja does NOT rebuild
dash when posix_*.c files change. You must rebuild dash manually after any
POSIX shim modification.

### Step 4: Create system disk image

```bash
cd $WORK/AIOS
python3 scripts/mkdisk.py disk/disk_ext2.img \
    --rootfs disk/rootfs \
    --install-elfs build-04/sbase \
    --aios-elfs build-04/projects/aios/
```

Creates a 128MB ext2 image containing:
- 99 sbase tools in /bin/
- 29 AIOS programs in /bin/aios/
- dash in /bin/dash
- Rootfs overlay (/etc/passwd, /etc/motd, /tmp/, etc.)

### Step 5: Create log disk (optional)

```bash
dd if=/dev/zero of=disk/log_ext2.img bs=1M count=16

# macOS:
/opt/homebrew/opt/e2fsprogs/sbin/mke2fs -b 1024 -I 128 -t ext2 \
    -L "aios-log" disk/log_ext2.img

# Linux:
mke2fs -b 1024 -I 128 -t ext2 -L "aios-log" disk/log_ext2.img
```

The log disk is identified by volume label "aios-log" at boot. If absent,
AIOS prints "No log drive (optional)" and continues without file logging.

## 7. Boot QEMU

### Minimal boot (serial console only)

```bash
qemu-system-aarch64 \
    -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G \
    -nographic -serial mon:stdio \
    -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

### Full boot (serial + log disk + network + display)

```bash
qemu-system-aarch64 \
    -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G \
    -serial mon:stdio \
    -device ramfb \
    -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -drive file=disk/log_ext2.img,format=raw,if=none,id=hd1 \
    -device virtio-blk-device,drive=hd1 \
    -device virtio-net-device,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

Login: `root` / Password: `root`

The log drive, network adapter, and display are all optional. Omit any
`-device` line and AIOS boots without that subsystem.

### Exit QEMU

Press `Ctrl-A` then `X` to terminate QEMU from the serial console.

## 8. Development Cycle

### Incremental rebuild (server/kernel changes only)

After editing root task code (`aios_root.c`, `pipe_server.c`, `fork.c`,
`exec_server.c`, `fs_server.c`, `procfs.c`, `ext2.c`, posix_*.c, etc.):

```bash
cd build-04 && ninja
```

No disk rebuild needed -- these are part of the kernel image loaded by ELF-loader.

### Rebuild after app changes

After editing user programs (`src/apps/mini_shell.c`, `src/apps/getty.c`, etc.):

```bash
cd build-04 && ninja && cd ..
python3 scripts/mkdisk.py disk/disk_ext2.img \
    --rootfs disk/rootfs \
    --install-elfs build-04/sbase \
    --aios-elfs build-04/projects/aios/
```

Apps are loaded from the ext2 disk, not the kernel image.

### Rebuild after POSIX shim changes

After editing `src/lib/posix_*.c` or `src/lib/aios_posix.c`:

```bash
cd build-04 && ninja && cd ..

# Rebuild sbase (links libaios_posix.a)
python3 scripts/build_sbase.py --jobs 16

# Rebuild dash (links libaios_posix.a, ninja does NOT track this)
DASH=$WORK/dash/src
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

# Rebuild disk
python3 scripts/mkdisk.py disk/disk_ext2.img \
    --rootfs disk/rootfs \
    --install-elfs build-04/sbase \
    --aios-elfs build-04/projects/aios/
```

### Full rebuild from scratch

Required when CPIO contents change (tty_server, auth_server) or after
major build system changes:

```bash
cd $WORK/AIOS
rm -rf build-04 && mkdir build-04 && cd build-04
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
ninja
cd ..
python3 scripts/build_sbase.py --jobs 16
# ... rebuild dash + disk as above
```

Note: `rm -rf build-04` also deletes `build-04/sbase/` so sbase and dash
must always be rebuilt after a full rebuild.

### Rebuild order when everything changes

1. `cd build-04 && ninja` (kernel + root task + CPIO)
2. `python3 scripts/build_sbase.py` (99 sbase tools)
3. Rebuild dash (manual aios-cc command)
4. `python3 scripts/mkdisk.py ...` (disk image with all programs)

Always: ninja first, sbase second, dash third, disk last.

## 9. Cross-Compiling Custom Programs

The `scripts/aios-cc` wrapper compiles standalone C programs for AIOS:

```bash
# Simple program
./scripts/aios-cc myprogram.c -o build-04/sbase/myprogram

# Compile only (produce .o)
./scripts/aios-cc -c myprogram.c -o myprogram.o

# Multiple source files
./scripts/aios-cc file1.c file2.c -o build-04/sbase/myprogram
```

The wrapper handles all seL4 include paths, CRT objects, musl libc,
and the AIOS POSIX shim (`libaios_posix.a`). Output is a statically
linked AArch64 ELF.

## 10. Directory Layout

```
AIOS/
  src/                    Source code (kernel, servers, POSIX shim, apps)
  include/aios/           AIOS headers
  scripts/                Build and utility scripts
  disk/                   Disk images and rootfs overlay
  docs/                   Documentation
  projects/aios/          CMakeLists.txt (build configuration)
  settings.cmake          seL4 kernel config
  deps/                   seL4 dependencies (gitignored, see Step 3)
  tools/seL4/             Symlink to deps/seL4_tools/cmake-tool
  build-04/               Build output (gitignored)
    images/               Kernel image
    sbase/                Cross-compiled sbase tools + dash
    projects/aios/        Cross-compiled AIOS programs
```

## 11. Troubleshooting

| Problem | Solution |
| --- | --- |
| `sed: invalid option -- 'i'` | GNU sed not in PATH. Add `/opt/homebrew/opt/gnu-sed/libexec/gnubin` to PATH |
| `makeinfo: not found` | Install texinfo and add `/opt/homebrew/opt/texinfo/bin` to PATH |
| `visibility("protected")` errors | Apply musllibc patches (Step 4) |
| `NULL redefined` warning in dash | Harmless (dash and musl define NULL differently) |
| `elfloader has LOAD segment with RWX` | Harmless linker warning from ELF-loader |
| Stale binaries after POSIX changes | Rebuild dash manually (ninja does not track it) |
| `rm -rf build-04` loses sbase | Rebuild sbase after any full rebuild |
| QEMU: no display output | Add `-device ramfb` to QEMU command |
| QEMU: no network | Add `-device virtio-net-device` with netdev |
| Boot says "No log drive" | Create log disk (Step 5) or ignore (optional) |
| `Attempted to invoke null cap` | Usually a process using a stale capability. Full rebuild may help |
