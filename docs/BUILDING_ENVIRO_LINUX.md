# AIOS Linux Build Environment Setup

Step-by-step guide for building AIOS on Linux (Ubuntu/Debian, x86_64).
Tested on Ubuntu 25.10 (kernel 6.17), GCC 15.2, Python 3.13.

## Quick Start

```bash
cd ~/Source/github/AIOS
python3 scripts/setup-linux.py    # installs deps, clones repos, patches, builds
```

Or follow the manual steps below.

## 1. System Packages

```bash
sudo apt install \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    cmake ninja-build python3 python3-venv \
    qemu-system-arm \
    device-tree-compiler libxml2-utils texinfo \
    git
```

Verify:

```bash
aarch64-linux-gnu-gcc --version   # >= 14.x
cmake --version                   # >= 3.20
ninja --version                   # >= 1.10
qemu-system-aarch64 --version     # >= 8.x
```

## 2. Python Virtual Environment

seL4 build scripts require several Python packages. Use a venv to
avoid polluting the system Python:

```bash
cd ~/Source/github/AIOS
python3 -m venv .venv
.venv/bin/pip install pyfdt pyyaml jinja2 jsonschema lxml ply pyelftools libarchive-c
```

All build commands must use this venv. Prefix with:

```bash
PATH=~/Source/github/AIOS/.venv/bin:$PATH
```

Or activate it:

```bash
source ~/Source/github/AIOS/.venv/bin/activate
```

## 3. Clone Repository

```bash
mkdir -p ~/Source/github && cd ~/Source/github
git clone https://github.com/arcanii/AIOS.git
cd AIOS
```

## 4. Set Up seL4 Dependencies

```bash
cd ~/Source/github/AIOS
mkdir -p deps

git clone https://github.com/seL4/seL4.git deps/seL4-kernel
ln -sf seL4-kernel deps/kernel

git clone https://github.com/seL4/musllibc.git deps/musllibc
git clone https://github.com/seL4/seL4_libs.git deps/seL4_libs
git clone https://github.com/seL4/seL4_tools.git deps/seL4_tools
git clone https://github.com/seL4/sel4runtime.git deps/sel4runtime
git clone https://github.com/seL4/util_libs.git deps/util_libs
```

Create build system symlink (note: two levels up from tools/seL4/):

```bash
mkdir -p tools/seL4
ln -sf ../../deps/seL4_tools/cmake-tool tools/seL4/cmake-tool
```

Verify:

```bash
ls tools/seL4/cmake-tool/all.cmake   # must exist
```

## 5. Apply Required Patches

### musllibc visibility fix (required for GCC 15)

GCC 15 changed default symbol visibility. Two files need patching:

```bash
# deps/musllibc/src/internal/vis.h
# Change: __attribute__((visibility("protected")))
# To:     __attribute__((visibility("default")))

# deps/musllibc/src/internal/stdio_impl.h
# Same change: protected -> default
```

### libplatsupport warning suppression

```bash
# deps/util_libs/libplatsupport/src/common.c
# Add at top: #pragma GCC diagnostic ignored "-Wformat"
```

## 6. Clone External Tools

### sbase (99 Unix utilities)

```bash
cd ~/Source/github
git clone https://git.suckless.org/sbase
```

### dash (POSIX login shell)

```bash
cd ~/Source/github
git clone https://github.com/tklauser/dash.git
cd dash/src

# Generate required headers (order matters)
sh mktokens
cc -o mksyntax mksyntax.c && ./mksyntax
cc -o mknodes mknodes.c && ./mknodes nodetypes nodes.c.pat
cc -o mkinit mkinit.c && ./mkinit *.c
cc -o mksignames mksignames.c && ./mksignames

# Generate builtins.def from builtins.def.in
# Must preprocess to remove #if JOBS / #ifndef SMALL blocks
# Use: python3 ~/Source/github/AIOS/scripts/gen-builtins-def.py
python3 ~/Source/github/AIOS/scripts/gen-builtins-def.py

# Then generate builtins.c and builtins.h
sh mkbuiltins builtins.def
```

### dash config.h

Create `~/Source/github/dash/src/config.h`:

```c
/* config.h -- AIOS/musl/AArch64 configuration for dash */
#ifndef DASH_CONFIG_H
#define DASH_CONFIG_H

#define JOBS 0
#define SMALL 1
#define GLOB_BROKEN 1
#define _GNU_SOURCE 1
#define BSD 1

#define HAVE_STRTOD 1
#define HAVE_ISALPHA 1
#define HAVE_MEMPCPY 1
#define HAVE_PATHS_H 1
#define HAVE_STRSIGNAL 1
#define HAVE_KILLPG 1
#define HAVE_SYSCONF 1
#define HAVE_WAITPID 1

/* musl AArch64 uses stat, not stat64 */
#define stat64 stat
#define fstat64 fstat
#define lstat64 lstat
#define open64 open
#define lseek64 lseek
#define ftruncate64 ftruncate

#define SHELLPATH "/bin/dash"
#define bsd_signal signal

#endif
```

### dash jobs.c patch

musl does not expose `wait3` through the BSD guard path reliably.
Replace the single call in `jobs.c`:

```
Line ~1179: change wait3(status, flags, NULL)
        to: waitpid(-1, status, flags)
```

## 7. Build

### Set AIOS root (all paths relative to this)

```bash
export AIOS_ROOT=~/Source/github/AIOS
export PATH=$AIOS_ROOT/.venv/bin:$PATH
```

### Step 1: Build kernel + root task

```bash
cd $AIOS_ROOT
rm -rf build-04 && mkdir build-04 && cd build-04
cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
ninja
```

### Step 2: Build sbase

```bash
cd $AIOS_ROOT
python3 scripts/build_sbase.py --sbase ~/Source/github/sbase
```

### Step 3: Build dash

```bash
cd $AIOS_ROOT
python3 scripts/build_dash.py
```

### Step 4: Create disk image

```bash
cd $AIOS_ROOT
python3 scripts/mkdisk.py disk/disk_ext2.img \
    --rootfs disk/rootfs \
    --install-elfs build-04/sbase \
    --aios-elfs build-04/projects/aios/
```

### Step 5: Create build_number.h (if missing)

```bash
echo "#define AIOS_BUILD_NUMBER 1" > $AIOS_ROOT/include/aios/build_number.h
```

## 8. Boot QEMU

```bash
cd $AIOS_ROOT
python3 scripts/qemu-boot.py           # basic boot
python3 scripts/qemu-boot.py --net     # with networking
python3 scripts/qemu-boot.py --display # with framebuffer
```

Login: `root` / `root`

Exit QEMU: `Ctrl-A X`

## 9. RPi4 Build

```bash
cd $AIOS_ROOT
rm -rf build-rpi4 && mkdir build-rpi4 && cd build-rpi4
cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- \
    -DAIOS_SETTINGS=settings-rpi4.cmake ..
ninja
```

### Create SD card image

```bash
cd $AIOS_ROOT
python3 scripts/mksdcard.py --output disk/sdcard-rpi4.img
```

### Flash SD card

```bash
lsblk                    # identify SD card device
sudo dd if=disk/sdcard-rpi4.img of=/dev/sdX bs=4M status=progress
sync
```

## 10. Validation Tests

After booting QEMU, run the test suite:

```
hwinfo          # hardware identification
test_hw         # hardware validation (17 tests)
test_fswrite    # ext2 write/read/unlink (13 tests)
test_smp        # SMP core count, parallel fork (4 tests)
test_pipe_rw    # pipe read/write/EOF (13 tests)
signal_test     # POSIX signals (16 tests)
```

## 11. Troubleshooting

| Problem | Solution |
|---------|----------|
| `pyfdt` not found | `$AIOS_ROOT/.venv/bin/pip install pyfdt` |
| `yaml` not found | `$AIOS_ROOT/.venv/bin/pip install pyyaml` |
| `lxml` not found | `$AIOS_ROOT/.venv/bin/pip install lxml` |
| `ply` not found | `$AIOS_ROOT/.venv/bin/pip install ply` |
| `pyelftools` not found | `$AIOS_ROOT/.venv/bin/pip install pyelftools` |
| `libarchive` not found | `$AIOS_ROOT/.venv/bin/pip install libarchive-c` |
| `xmllint` not found | `sudo apt install libxml2-utils` |
| `g++` cross not found | `sudo apt install g++-aarch64-linux-gnu` |
| `python3-venv` missing | `sudo apt install python3.13-venv` |
| `build_number.h` missing | `echo "#define AIOS_BUILD_NUMBER 1" > include/aios/build_number.h` |
| cmake-tool symlink broken | `ln -sf ../../deps/seL4_tools/cmake-tool tools/seL4/cmake-tool` |
| sbase path wrong | `python3 scripts/build_sbase.py --sbase ~/Source/github/sbase` |
| dash `wait3` error | Replace `wait3(status, flags, NULL)` with `waitpid(-1, status, flags)` in jobs.c |
| dash `killpg` error | Add `#define HAVE_KILLPG 1` to dash config.h |
| dash `builtins.h` corrupt | Regenerate: `python3 scripts/gen-builtins-def.py && sh mkbuiltins builtins.def` |
