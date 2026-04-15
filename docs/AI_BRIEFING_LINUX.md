# AIOS AI Briefing Document (Linux Host)

## Project Overview

**AIOS (Open Aries)** is a microkernel operating system built on seL4, targeting AArch64 (QEMU and Raspberry Pi 4).

* **Repository**: https://github.com/arcanii/AIOS
* **Branch**: rpi4 (RPi4 hardware feature branch, branched from main)
* **Developer**: Bryan
* **Current Version**: v0.4.96

## Development Environment

* **Host**: Linux x86_64 (Ubuntu, kernel 6.17)
* **Cross-compiler**: aarch64-linux-gnu-gcc (Ubuntu 15.2.0, system package)
* **Build system**: CMake 3.31 + Ninja 1.12
* **Cross-compile wrapper**: scripts/aios-cc (passes EXTRA flags to compile + link)
* **Scripts**: Python 3.13 (preferred), Bash (for simple ops)
* **Disk tools**: Custom Python ext2 builder (scripts/ext2/builder.py)
* **QEMU**: qemu-system-aarch64 10.1.0
* **SD card tools**: dd (Linux block device, e.g. /dev/sdX or /dev/mmcblkX)

### Key differences from macOS host

| Item | macOS (main branch) | Linux (rpi4 branch) |
| --- | --- | --- |
| Repo path | ~/Desktop/github_repos/AIOS | ~/Source/github/AIOS |
| Cross-compiler | Homebrew aarch64-linux-gnu-gcc | apt: gcc-aarch64-linux-gnu |
| SD card flash | diskutil + dd to /dev/rdiskN | dd to /dev/sdX or /dev/mmcblkX |
| SD card unmount | diskutil unmountDisk | umount or udisksctl |
| SD card eject | diskutil eject | eject or udisksctl power-off |
| CPU info | sysctl -n hw.ncpu | nproc |
| U-Boot build | gmake + Homebrew OpenSSL | make (system OpenSSL) |

## Session Protocols

### Bump-Patch Protocol

ALWAYS bump-patch at the start of each new work phase.

```
./scripts/bump-patch.sh
./scripts/version.sh
```

* Commit the previous version FIRST, then bump for the next phase
* Version format: v0.4.XX (staying on 0.4.x throughout this development cycle)
* Never use bump-minor unless explicitly directed
* Build number auto-increments on each ninja build

### Script Naming Protocol

When presenting multiple scripts to the user, ALWAYS label them:

* **Script A** -- description
* **Script B** -- description
* **Script C** -- description

This allows the user to report "Script B failed" without ambiguity.

### Code Change Protocol

* Use Python for file edits (heredoc quoting is unreliable in zsh with special chars)
* Use Claude Code Edit tool when available (preferred over scripts)
* Avoid sed for multi-line edits or edits with quotes/slashes
* Always verify changes applied: grep for expected content after edit
* Full rebuild required when CPIO contents change (rm -rf build dir)
* Incremental ninja sufficient for aios_root.c or aios_posix.c changes
* Do not include single-quote apostrophes in C comments (breaks zsh copy-paste)

### Commit Protocol

* Commit at each milestone with version tag in message
* Format: "v0.4.XX: short description\n\ndetails"
* Commits made via GitHub Desktop (not CLI git commit)
* git push origin rpi4 after each commit

### No Hacks Protocol

* Pure POSIX -- no alias tables, no prefix stripping, no magic
* Strict Unix philosophy to do things the right way
* Shell searches $PATH, sends full path to exec_thread
* exec_thread loads exactly what it is given
* mkdisk installs files with original names
* Temporary workarounds must be documented and removed when proper solution available

## Build & Boot Commands

### Full Rebuild (QEMU target)

```
cd ~/Source/github/AIOS
rm -rf build-04 && mkdir build-04 && cd build-04
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
ninja
```

### Full Rebuild (RPi4 target)

```
cd ~/Source/github/AIOS
rm -rf build-rpi4 && mkdir build-rpi4 && cd build-rpi4
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- \
    -DAIOS_SETTINGS=settings-rpi4.cmake ..
ninja
```

### Incremental Rebuild

```
cd ~/Source/github/AIOS/build-rpi4  # or build-04
ninja
```

### Rebuild sbase (parallel builder)

```
python3 scripts/build_sbase.py [--clean] [--jobs N]
```

### Rebuild Disk Image (CRITICAL)

```
python3 scripts/mkdisk.py disk/disk_ext2.img \
    --rootfs disk/rootfs \
    --install-elfs build-04/sbase \
    --aios-elfs build-04/projects/aios/
```

### Boot QEMU

```
qemu-system-aarch64 \
    -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G \
    -nographic -serial mon:stdio \
    -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

### Create RPi4 SD Card Image

```
python3 scripts/mksdcard.py [--mem 4096] [--output disk/sdcard-rpi4.img]
```

### Flash SD Card (Linux)

```
# Find SD card device
lsblk

# Flash full image
sudo dd if=disk/sdcard-rpi4.img of=/dev/sdX bs=4M status=progress
sync
```

### Cross-Compile External Programs

```
./scripts/aios-cc source.c -o output
```

## Which binaries live where

| Binary | Loaded from | Rebuild disk needed? |
| --- | --- | --- |
| aios_root (+ all server threads) | ELF-loader (kernel image) | No |
| tty_server, auth_server | CPIO inside kernel image | No (but full rebuild if changed) |
| mini_shell, getty, all apps | /bin/aios/ on ext2 disk | **Yes** |
| sbase tools (echo, cat, ...) | /bin/ on ext2 disk | **Yes** |
| dash | /bin/dash on ext2 disk | **Yes** |

## Repository Structure

```
AIOS/
+-- src/
|   +-- aios_root.c          # Root task: boot, drivers, exec/fs/thread/auth servers
|   +-- plat/
|   |   +-- rpi4/
|   |   |   +-- blk_emmc.c      # BCM2711 SDHCI read/write (CMD17+CMD24 PIO)
|   |   |   +-- net_genet.c     # BCM54213 GENET Ethernet driver
|   |   |   +-- display_vc.c    # VideoCore mailbox framebuffer
|   |   |   +-- diag_stub/      # Pre-seL4 diagnostic stub (HDMI + spin-table)
|   |   +-- qemu-virt/          # QEMU virtio drivers
+-- settings.cmake              # seL4 config (QEMU, 4-core SMP)
+-- settings-rpi4.cmake         # seL4 config (RPi4 BCM2711, 4-core SMP)
+-- docs/
|   +-- AI_BRIEFING.md          # Original briefing (macOS host)
|   +-- AI_BRIEFING_LINUX.md    # This file (Linux host)
|   +-- NEXT_RPI4.md            # RPi4 branch progress and next steps
```

See AI_BRIEFING.md for full architecture, IPC protocol labels, and process model details.
Those remain unchanged between hosts.
