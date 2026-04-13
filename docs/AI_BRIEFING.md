# AIOS AI Briefing Document

## Project Overview

**AIOS (Open Aries)** is a microkernel operating system built on seL4, targeting AArch64/QEMU.

* **Repository**: https://github.com/arcanii/AIOS
* **Branch**: main
* **Developer**: Bryan
* **Current Version**: v0.4.90

## Development Environment

* **Host**: macOS Apple Silicon M3 Max
* **Cross-compiler**: aarch64-linux-gnu-gcc (Homebrew, GCC 15)
* **Build system**: CMake + Ninja
* **Cross-compile wrapper**: scripts/aios-cc (passes EXTRA flags to compile + link)
* **Scripts**: Python 3 (preferred), Bash (for simple ops)
* **Disk tools**: Custom Python ext2 builder (scripts/ext2/builder.py)
* **QEMU**: qemu-system-aarch64

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
* Avoid sed for multi-line edits or edits with quotes/slashes
* Always verify changes applied: grep for expected content after edit
* Full rebuild required when CPIO contents change (rm -rf build-04)
* Incremental ninja sufficient for aios_root.c or aios_posix.c changes
* Do not include single-quote apostrophes in C comments (breaks zsh copy-paste)

### Commit Protocol

* Commit at each milestone with version tag in message. Each patch should contain 5 or more new features.
* Format: "v0.4.XX: short description\n\ndetails"
* git push origin main after each commit

### No Hacks Protocol

* Pure POSIX -- no alias tables, no prefix stripping, no magic.
* Strict Unix philosophy to do things the right way.
* Shell searches $PATH, sends full path to exec_thread
* exec_thread loads exactly what it is given
* mkdisk installs files with original names
* Temporary workarounds must be documented and removed when proper solution available

## Build & Boot Commands

### Full Rebuild (kernel + root task + all programs)

```
cd ~/Desktop/github_repos/AIOS
rm -rf build-04 && mkdir build-04 && cd build-04
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
ninja
```

### Incremental Rebuild (after source changes)

```
cd ~/Desktop/github_repos/AIOS/build-04
ninja
```

Note: ninja only recompiles changed files. This is sufficient for changes to
aios_root.c, pipe_server.c, posix_file.c, mini_shell.c, etc. A full rebuild
(rm -rf build-04) is only needed when CPIO contents change (tty_server, auth_server).

### Rebuild sbase (parallel builder)

```
python3 scripts/build_sbase.py [--clean] [--jobs N]
```

Builds 99 sbase tools into build-04/sbase/. Required after full rebuild
(rm -rf build-04 deletes the sbase binaries).

### Rebuild Disk Image (CRITICAL)

```
python3 scripts/mkdisk.py disk/disk_ext2.img \
    --rootfs disk/rootfs \
    --install-elfs build-04/sbase \
    --aios-elfs build-04/projects/aios/
```

**IMPORTANT**: mini_shell and all user programs are loaded from the ext2 disk
image, NOT from the CPIO archive. Only tty_server and auth_server are in CPIO
(they start before the filesystem is mounted). After any change to mini_shell.c,
getty.c, or any program in src/apps/, you MUST rebuild the disk image for
changes to take effect in QEMU. Failing to do this means QEMU boots with
stale binaries.

The typical edit-test cycle is:
1. Edit source file
2. `cd build-04 && ninja` (incremental build)
3. `cd .. && python3 scripts/mkdisk.py disk/disk_ext2.img --rootfs disk/rootfs --install-elfs build-04/sbase --aios-elfs build-04/projects/aios/`
4. Boot QEMU

If only aios_root.c or server thread code changed (pipe_server.c, exec_server.c,
fs_server.c, thread_server.c, fork.c, reap.c), step 3 can be skipped -- these
are part of the root task binary loaded directly by the ELF-loader.

### Boot QEMU

```
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

The second drive (log_ext2.img) is optional. If absent, boot prints
"No log drive (optional)" and continues without file logging. The boot
code identifies drives by ext2 volume label -- "aios-log" is the log
drive, any unlabeled ext2 is the system disk. Drive order does not matter.

### Build RPi4 Target

```
mkdir build-rpi4 && cd build-rpi4
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- \
    -DAIOS_SETTINGS=settings-rpi4.cmake ..
ninja
```

### Create RPi4 SD Card Image

```
python3 scripts/mksdcard.py [--mem 4096] [--output disk/sdcard-rpi4.img]
```

### RPi4 Hardware Boot (no serial adapter)

No USB-to-serial adapter available. All debugging via HDMI framebuffer.

**HDMI diagnostic stub**: A 4KB C program prepended to the seL4 binary at
kernel_address - 0x1000. It runs before seL4, sets up VideoCore mailbox
framebuffer (1024x768), displays a CRC32 build ID + EL level + DTB address,
then jumps to seL4 at offset +0x1000. The seL4 elfloader has been patched
to write colored progress bars to the same framebuffer at known address 0x1000.

**SD card flashing -- two methods**:

Method 1: `mksdcard.py` (full image with MBR + FAT32 boot + ext2 system):
```
python3 scripts/mksdcard.py
diskutil unmountDisk /dev/diskN
sudo dd if=disk/sdcard-rpi4.img of=/dev/rdiskN bs=1m
diskutil eject /dev/diskN
```
Used for final SD card images with ext2 system partition. Slower (193MB).

Method 2: Manual FAT32 (debug iteration, no ext2):
```
diskutil eraseDisk FAT32 AIOSBOOT MBRFormat /dev/diskN
cp disk/rpi4-firmware/start4.elf /Volumes/AIOSBOOT/start_cd.elf
cp disk/rpi4-firmware/fixup4.dat /Volumes/AIOSBOOT/fixup_cd.dat
cp disk/rpi4-firmware/start4.elf /Volumes/AIOSBOOT/start4.elf
cp disk/rpi4-firmware/fixup4.dat /Volumes/AIOSBOOT/fixup4.dat
cp disk/rpi4-firmware/bcm2711-rpi-4-b.dtb /Volumes/AIOSBOOT/
cp /tmp/config.txt /Volumes/AIOSBOOT/
cp /tmp/kernel8.img /Volumes/AIOSBOOT/
diskutil eject /dev/diskN
```
Used for all boot debugging. Fast (copies ~7MB). Single FAT32 partition,
whole card. No ext2 (not needed until SD card driver works).

**Build and flash cycle (during debug)**:
```
# 1. Rebuild RPi4 target
cd build-rpi4 && ninja && cd ..

# 2. Combine stub + seL4 binary (python writes CRC32 build ID at 0xFFC)
python3 <combiner script>   # prints build ID to verify on HDMI

# 3. Flash with Method 2 above

# 4. Boot RPi4, verify build ID on HDMI matches terminal output
```

**What the HDMI shows**:
* Line 1: CRC32 build ID (green, must match terminal output)
* Line 2: EL level + DTB address
* Colored bars = elfloader progress (blue=sys_boot, cyan=load_images,
  magenta=DTB, white=load_elf). More bars = further progress.
* Bar colors appear BGR-swapped (0xFF8800 orange renders as blue)

**Key learnings (v0.4.91)**:
* EEPROM updated to 2026-02-23 (boots with D-cache enabled)
* Mini UART (0xFE215040) hangs CPU bus -- elfloader UART registration skipped
* printf in elfloader crashes on RPi4 -- disabled via `#define printf(...) ((void)0)`
* Bare-metal entry needs: stack setup, secondary core parking, D-cache disable
* Entry point from `readelf -h` (0x350f000), not LOAD PhysAddr (0x3500000)
* `kernel_address` in config.txt = entry_point - 0x1000 (stub size)
* Elfloader reaches load_images but crashes in memset to low phys addr
* **Likely fix**: disable MMU in stub (not just D-cache)

**U-Boot alternative** (interactive debugging):
* Build: `cd ~/Desktop/github_repos/u-boot && gmake CROSS_COMPILE=aarch64-linux-gnu- rpi_4_defconfig && gmake HOSTCFLAGS="-I$(brew --prefix openssl)/include" HOSTLDFLAGS="-L$(brew --prefix openssl)/lib" -j$(sysctl -n hw.ncpu)`
* config.txt: `kernel=u-boot.bin`
* USB keyboard works at U-Boot prompt on HDMI
* Commands: `fatload mmc 0 0x350f000 aios.bin` then `go 0x350f000`
* No `dcache`/`icache` commands in default config
* `md` command works for memory inspection
* EL check: `fatload mmc 0 0x3000 el_check.bin && go 0x3000 && md 0x2000 1`

**Elfloader deps/ debug patches** (temporary, must clean up):
* `elfloader-tool/src/arch-arm/sys_boot.c` -- diag_bar calls, diag_fb_debug.h
* `elfloader-tool/src/drivers/uart/bcm-uart.c` -- skip uart_set_out
* `elfloader-tool/src/common.c` -- printf disabled, DTB memmove skip, diag_bars
* `elfloader-tool/include/diag_fb_debug.h` -- new: shared inline bar renderer

### Cross-Compile External Programs

```
./scripts/aios-cc source.c -o output
```

### Build dash (cross-compile, primary login shell)

```
cd ~/Desktop/github_repos/AIOS
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

IMPORTANT: Rebuild dash after any change to src/lib/posix_*.c files.
ninja rebuilds libaios_posix.a but does NOT rebuild dash.

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
|   +-- aios_auth.c          # Auth server: SHA-3-512, user DB, sessions, permissions
|   +-- aios_log.c           # Kernel log: ring buffer + serial + file (/log/aios.log)
|   +-- ext2.c               # ext2 filesystem (read + write + indirect blocks)
|   +-- vfs.c                # Virtual filesystem switch (mount dispatch)
|   +-- procfs.c             # /proc: version, uptime, status, mounts, log, meminfo
|   +-- lib/
|   |   +-- aios_posix.c     # POSIX shim orchestrator (init, wrap_main, globals)
|   |   +-- posix_internal.h # Shared header: fd struct, NR defines, function decls
|   |   +-- posix_file.c     # File I/O: open, openat, read, write, close, lseek, /dev/null, O_APPEND
|   |   +-- posix_stat.c     # Stat: fstat, fstatat, statx, fchmod, fchown, readlinkat
|   |   +-- posix_dir.c      # Directories: getdents64, chdir, mkdirat, unlinkat
|   |   +-- posix_proc.c     # Process: exit, clone, execve, wait4, signals, setpgid
|   |   +-- posix_time.c     # Time: clock_gettime, gettimeofday, nanosleep, times
|   |   +-- posix_misc.c     # Misc: ioctl, fcntl, dup, dup3, pipe2, umask, mprotect
|   |   +-- posix_thread.c   # Threads: pthread wrappers, getpwuid, getgrnam
|   |   +-- vka_audit.c      # VKA allocation counter (per-subsystem instrumentation)
|   +-- boot/
|   |   +-- boot_fs_init.c   # Multi-device virtio-blk probe + ext2 mount (volume label)
|   |   +-- boot_log_init.c  # Second virtio-blk device: DMA, virtio init, /log mount
|   |   +-- boot_services.c  # Server threads + process spawning
|   |   +-- blk_io.c         # Block I/O (virtio-blk read/write, main + log devices)
|   |   +-- spawn_util.c     # spawn_with_args, spawn_simple
|   +-- servers/
|   |   +-- exec_server.c    # ELF loader, process spawn, foreground wait
|   |   +-- pipe_server.c    # Pipes, fork, exec, wait, exit, signals (central hub)
|   |   +-- fs_server.c      # VFS dispatch (LS, CAT, STAT, MKDIR, WRITE, UNLINK, APPEND)
|   |   +-- thread_server.c  # Thread create/join for child processes
|   +-- process/
|   |   +-- fork.c           # do_fork() implementation
|   |   +-- reap.c           # reap_forked_child, zombie management
|   +-- apps/                # AIOS programs + test_threads
|   +-- plat/
|   |   +-- plat.h              # Platform dispatcher (like arch.h)
|   |   +-- blk_hal.h           # Block device HAL interface
|   |   +-- net_hal.h           # Network device HAL interface
|   |   +-- display_hal.h       # Display device HAL interface
|   |   +-- qemu-virt/
|   |   |   +-- plat_virtio_probe.c # Shared MMIO mapping + device scan
|   |   |   +-- plat_virtio_probe.h # Probe interface (info struct + getters)
|   |   |   +-- blk_virtio.c    # Virtio-blk init + sector I/O (HAL impl)
|   |   |   +-- net_virtio.c    # Virtio-net init + TX + RX driver (HAL impl)
|   |   |   +-- display_ramfb.c # fw_cfg + ramfb framebuffer (HAL impl)
|   |   +-- rpi4/
|   |   |   +-- blk_emmc.c      # BCM2835 SDHCI stub (Phase 2)
|   |   |   +-- net_genet.c     # BCM54213 GENET stub (Phase 3)
|   |   |   +-- display_vc.c    # VideoCore mailbox stub (Phase 4)
|   |   |   +-- fb_test.c       # Bare-metal framebuffer test
+-- include/aios/
|   +-- version.h, build_number.h, ext2.h, vfs.h, procfs.h
|   +-- aios_auth.h          # Auth protocol: IPC labels, user/session types
|   +-- aios_log.h           # Log macros: AIOS_LOG_INFO/WARN/ERROR/DEBUG
|   +-- root_shared.h        # Shared types, limits, externs for all server threads
|   +-- aios_signal.h        # Signal state types
|   +-- vka_audit.h          # VKA allocation counter types and functions
+-- scripts/
|   +-- mkdisk.py            # Disk image CLI
|   +-- ext2/builder.py      # ext2 image builder (multi-group, indirect blocks)
|   +-- aios-cc              # Cross-compiler wrapper (per-PID temp dirs, --wrap flags)
|   +-- build_sbase.py       # Parallel sbase builder (16 jobs, progress bar)
|   +-- bump-patch.sh, bump-minor.sh, bump-build.sh, version.sh
|   +-- posix_audit.py       # POSIX compliance audit
+-- disk/
|   +-- disk_ext2.img        # 128MB ext2 system disk (gitignored)
|   +-- log_ext2.img         # 16MB ext2 log disk, label "aios-log" (gitignored)
|   +-- rootfs/              # Filesystem content overlay (/etc, /tmp, /root, /log)
+-- docs/
|   +-- AI_BRIEFING.md       # This file
|   +-- ARCHITECTURE.md, DESIGN_0.4.md, LEARNINGS.md
|   +-- DASH_PORT.md         # dash porting guide and syscall audit
|   +-- DESIGN_RPI.md        # Raspberry Pi port design (PAL, RPi4/RPi5)
|   +-- patches/             # Documented dep patches (musl-gcc15, platsupport)
+-- projects/aios/CMakeLists.txt  # Build config
+-- deps/                    # gitignored: seL4, musllibc, etc.
+-- build-04/                # gitignored: build output
|   +-- sbase/               # Cross-compiled sbase tools + dash
|   +-- projects/aios/       # AIOS programs
+-- settings.cmake           # seL4 kernel config (QEMU, default)
+-- settings-rpi4.cmake      # seL4 kernel config (RPi4 BCM2711)
```

## Architecture

### Boot Sequence

1. ELF-loader loads kernel + root task
2. Root task initializes: allocator (4000 pages), SMP
3. Probe all virtio-blk devices, identify system disk by volume label
4. Init ext2 on system disk, VFS mounts: / (ext2) and /proc (procfs)
5. If log drive found (label "aios-log"): init second virtio-blk, mount /log
6. Auth init: load /etc/passwd (SHA-3-512), auto-login root
7. Starts fs_thread + exec_thread + thread_server + pipe_server
8. fs_thread creates /log/aios.log, flushes boot ring buffer to file
6. Spawns tty_server + auth_server (CPIO, isolated processes)
7. Spawns getty via exec_thread (fork+exec capable VSpace)
8. Getty presents login prompt, authenticates via auth IPC, reads pw_shell from
   /etc/passwd via getpwuid(), spawns configured shell (default: /bin/dash)

### Process Model

* tty_server: UART I/O via IPC (PUTC/GETC), runs in isolated VSpace
* fs_thread: VFS dispatch (LS, CAT, STAT, MKDIR, WRITE, UNLINK, UNAME)
* exec_thread: Reads ELF from /bin/, loads into new VSpace, manages lifecycle
* pipe_server: Central hub for pipes, fork, exec, wait, exit, signals
* thread_server: Creates child threads within existing processes
* dash: primary login shell (POSIX compliant, configured via /etc/passwd)
* mini_shell: legacy shell (still on disk as fallback, no longer default)
* User programs: isolated VSpaces, POSIX syscalls via IPC

### ELF Loading (from disk)

1. Shell searches $PATH, sends full path to exec_thread
2. exec_thread reads ELF via VFS (ext2 with indirect blocks)
3. elf_newFile() parses from 8MB static buffer
4. sel4utils_elf_load() maps segments into child VSpace
5. __wrap_main intercepts: extracts caps [ser, fs, thread, auth, pipe, CWD], sets CWD, calls real main
6. Process exits via VM fault -> exec_thread cleans up -> shell resumes

### POSIX Shim (modularized v0.4.58)

* __wrap_main: intercepts main(), strips cap args, inits shim
* argv from exec: [serial_ep, fs_ep, thread_ep, auth_ep, pipe_ep, CWD, progname, arg1, ...]
* CWD format: uid:gid:spipe:rpipe:/path (spipe/rpipe=99 if not piped)
* Programs see clean POSIX argv: [progname, arg1, ...]
* --wrap flags: main, pthread_create/join/exit/detach, pthread_mutex_*, getpwuid, getpwnam, getgrnam, getgrgid
* 70+ syscalls overridden via muslcsys_install_syscall()
* resolve_path() handles relative paths against CWD
* Environment variables set via environ pointer
* /dev/null handled directly in open/openat (no VFS round-trip)

### Pipe Architecture (v0.4.66)

Pipes use shared-memory frames allocated via vka_alloc_frame and mapped into
the root task VSpace. Each pipe_t has a shm_buf pointer (backed by a 4K mapped
frame) used as the ring buffer (PIPE_BUF_SIZE = 4096, matching page size).
Data flows through IPC message registers (MR-based path, up to 900 bytes per
call). Server-side SHM transfer infrastructure is in place (PIPE_MAP_SHM,
PIPE_WRITE_SHM, PIPE_READ_SHM ops with lazy xfer page allocation) but
client-side mapping enabled in v0.4.67 with cap copy tracking and proper
CNode_Delete/Revoke cleanup. Cleanup unmaps and frees all frames when all
refs are dropped.

### IPC Protocol Labels

```
SER_PUTC=1, SER_GETC=2, SER_KEY_PUSH=4
FS_LS=10, FS_CAT=11, FS_STAT=12, FS_PREAD=13
FS_MKDIR=14, FS_WRITE_FILE=15, FS_UNLINK=16, FS_UNAME=17, FS_RENAME=18, FS_APPEND=19, FS_PWRITE=20
EXEC_RUN=20, EXEC_NICE=21, EXEC_RUN_BG=24, EXEC_FORK=25, EXEC_WAIT=26
THREAD_CREATE=30, THREAD_JOIN=31
AUTH_LOGIN=40, AUTH_LOGOUT=41, AUTH_WHOAMI=42, AUTH_CHECK_FILE=43
AUTH_CHECK_KILL=44, AUTH_CHECK_PRIV=45, AUTH_USERADD=46
AUTH_PASSWD=47, AUTH_SU=48, AUTH_GROUPS=49, AUTH_USERMOD=50
AUTH_GET_USER=51, AUTH_LOAD_PASSWD=52
PIPE_CREATE=60, PIPE_WRITE=61, PIPE_READ=62, PIPE_CLOSE=63, PIPE_KILL=64
PIPE_FORK=65, PIPE_GETPID=66, PIPE_WAIT=67, PIPE_EXIT=68, PIPE_EXEC=69
PIPE_CLOSE_WRITE=70, PIPE_DEBUG=71, PIPE_EXEC_WAIT=72
PIPE_CLOSE_READ=73, PIPE_SET_IDENTITY=74
PIPE_SIGNAL=75, PIPE_SIG_FETCH=76, PIPE_SHUTDOWN=77
PIPE_MAP_SHM=78, PIPE_WRITE_SHM=79, PIPE_READ_SHM=80, PIPE_SET_PIPES=81
```

### Implemented Syscalls (86+)

File I/O: open, openat (O_CREAT, O_APPEND), read, readv, write, writev, close, lseek, pread64, pwrite64, ftruncate (FS_APPEND for server-side append)
Directories: getdents64, chdir, mkdirat, unlinkat
Stat: fstat, fstatat, statx, fchmod, fchmodat, fchown, fchownat
Links: linkat (ENOSYS), symlinkat (ENOSYS), readlinkat (/proc/self/exe supported)
Identity: getpid, getppid, getuid, geteuid, getgid, getegid, setuid, setgid
Process groups: setsid, getpgid, setpgid
System: uname (kernel IPC), getcwd, ioctl (TIOCGWINSZ, TIOCGPGRP, TIOCSPGRP), fcntl (F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD/SETFD, F_GETFL/SETFL), dup, dup3
Access: access, faccessat, umask, utimensat
Time: clock_gettime, gettimeofday, nanosleep, clock_nanosleep, times
Process: exit, exit_group, clone (fork), execve, wait4
Pipes: pipe2
Signals: rt_sigaction, rt_sigprocmask, rt_sigpending, rt_sigreturn, sigaltstack, kill, tgkill
Memory: mmap, munmap, brk, madvise, mprotect (stub)
Rename: renameat, renameat2
Linux compat: ppoll, pselect6, getrandom (splitmix64), prlimit64, prctl, getrlimit, setrlimit, sysinfo, getrusage, membarrier, futex (WAIT/WAKE stub)
Network: socket, bind, listen, accept/accept4, sendto, recvfrom, connect (stub), setsockopt (stub), getsockopt (stub), getsockname (stub), getpeername (stub), shutdown (stub). Socket fds support read()/write() via NET_RECVFROM/NET_SENDTO routing.

### fd Table

* AIOS_FD_BASE=10, AIOS_MAX_FDS=32
* aios_fd_t: active, is_dir, is_pipe, pipe_id, pipe_read, is_devnull, is_append, is_nonblock, is_tty, is_socket, socket_id, shm_vaddr, path[128], data[4096], size, pos
* fds 0-2 handled specially (stdin/stdout/stderr via serial or pipe redirect)
* /dev/null: is_devnull=1, write returns count, read returns 0
* /dev/urandom, /dev/random: is_devnull=1, read returns splitmix64 PRNG bytes
* /dev/zero: is_devnull=1, read returns zero bytes

### VKA Allocator Audit (v0.4.65)

Per-subsystem allocation counters track frames, endpoints, TCBs, cslots, and
untypeds across boot, fork, exec, thread, and pipe subsystems. The audit table
is dumped via the shell `debug` command (PIPE_DEBUG IPC).

Measured allocations (typical boot + login + commands):
* boot: 7 endpoints, 1 untyped (4 pages DMA) = 4 pages total
* fork: ~1200+ cslots per fork (cap duplication), 0 new frames
* exec: 4 cslots per exec (capability minting)
* pipe: 1 frame per pipe (shared-memory buffer)
* Pool: 4000 pages, ~3994 remaining after boot + login

The 4000-page pool is heavily oversized. Fork cost is dominated by cslot
allocation (capability duplication for VSpace pages), not frame allocation.

## External Tools

### sbase (suckless base utilities)

* Source: ~/Desktop/github_repos/sbase
* 99 tools: ls, cat, head, wc, sort, grep, sed, find, cp, rm, mkdir, etc.
* Cross-compiled via ./scripts/aios-cc with libutil + libutf
* Note: cp.c and rm.c need special handling (libutil has cp.c/rm.c too)

### dash (Debian Almquist Shell) -- PRIMARY LOGIN SHELL (v0.4.68)

* Source: ~/Desktop/github_repos/dash (cloned from github.com/tklauser/dash)
* 468KB statically linked AArch64 ELF
* Generated headers: token.h, syntax.h, nodes.h/c, builtins.h/c, init.c, signames.c, config.h
* Config: JOBS=0, SMALL=1, GLOB_BROKEN=1, _GNU_SOURCE
* Builtins.def: stripped C license comment block (mkbuiltins is not a C preprocessor), JOBS/SMALL conditionals removed
* Source files: main eval parser expand exec jobs trap redir input output var cd
  error options memalloc mystring syntax nodes builtins init show arith_yacc
  arith_yylex miscbltin system alias histedit mail signames + bltin/test bltin/printf bltin/times
* Cross-compiled with: aios-cc -I $DASH -include $DASH/config.h -DSHELL -DSMALL -DGLOB_BROKEN
* IMPORTANT: dash must be rebuilt after libaios_posix.a changes (ninja does not rebuild dash)
* Status: FULLY WORKING as primary login shell.

### zsh (Z Shell) -- SCRIPT MODE WORKING (v0.4.88)

* Source: ~/Desktop/github_repos/zsh (upstream zsh 5.9.0.3-test)
* 1,189KB statically linked AArch64 ELF
* Build: autoconf + configure (--disable-zle --disable-dynamic --disable-gdbm --disable-pcre --disable-cap --without-tcsetpgrp) -> make -> aios-cc re-link
* Static modules: zsh/main, rlimits, sched, datetime, langinfo, parameter, random
* Disabled: ZLE, completion, termcap, ksh93, hlgroup, zutil (ZLE dependency cascade)
* Stubs: libfakecurses.a (termcap), Src/termcap.h (declarations), getrandom (reads /dev/urandom)
* CFLAGS: -Wno-implicit-function-declaration -Wno-int-conversion (GCC 15 termcap compat)
* Status: script mode working. echo, variables, for loops, arithmetic verified.
  - Interactive mode with echo and backspace (tty_echo=1, TTY_READ cooked input)
  - -c mode, script files, pipelines, semicolons
  - Variable expansion, for loops, arithmetic, if/then/fi
  - Getty reads /etc/passwd pw_shell field to determine login shell
* v0.4.66: aios-cc EXTRA flags, split_args_qa quote-aware arg splitting
* v0.4.67: PIPE_SET_PIPES, stdin blocking, quote-aware pipe detection
* v0.4.68: tty_echo=1 default, TTY_READ for cooked stdin, RAW/COOKED mode
  switching, getty spawns dash via /etc/passwd, dash is primary shell

### mbedTLS v3.6.3 -- CRYPTO LIBRARY (v0.4.82)

* Source: ~/Desktop/github_repos/mbedtls (cloned from github.com/Mbed-TLS/mbedtls, tag v3.6.3)
* Config: bare-metal AArch64 (disabled FS_IO, NET_C, TIMING_C, x86 accel, PSA file storage; enabled ENTROPY_HARDWARE_ALT)
* Build: 81 crypto objects compiled with aios-cc, archived into build-04/libmbedcrypto.a (1181 KB)
* GCC fix: `-isystem $(gcc -print-file-name=include)` restores arm_neon.h under -nostdinc
* Entropy: /dev/urandom callback (splitmix64 + ARM CNTPCT_EL0)
* Verified: AES-256-GCM, SHA-256, ECDSA-P256, x25519 ECDH, CTR-DRBG -- all 5/5 PASS on AIOS

### SSH server (sshd) -- INTERACTIVE SHELL WORKING (v0.4.84)

* Source: src/ssh/ (sshd_main.c, ssh_transport.c, ssh_kex.c, ssh_crypto.c, ssh_session.h)
* Built with: aios-cc + libmbedcrypto.a + -DMBEDTLS_ALLOW_PRIVATE_ACCESS
* Source: src/ssh/ (sshd_main.c, ssh_transport.c, ssh_kex.c, ssh_crypto.c, ssh_encrypt.c, ssh_auth.c, ssh_channel.c, aios_entropy.c)
* Port: 2222 (development), single-connection sequential model
* Algorithms: curve25519-sha256 (kex), ecdsa-sha2-nistp256 (hostkey), aes256-ctr (cipher), hmac-sha2-256 (mac)
* Strict KEX: kex-strict-s-v00@openssh.com (Terrapin countermeasure)
* Status: full SSH session working (version exchange, KEXINIT, ECDH, NEWKEYS, encrypted transport, password auth, session channel, interactive shell relay)
* Verified with: OpenSSH 10.2 (Homebrew/OpenSSL 3.6.1)
* sshd ignores SIGINT (survives first Ctrl-C, second Ctrl-C force-kills via SIGKILL)
* Key learnings:
  - ECDSA signs SHA-256(H), not H directly (OpenSSH ssh_ecdsa_sign hashes before ECDSA_do_sign)
  - x25519 shared secret: use raw LE bytes as mpint data (OpenSSH sshbuf_put_bignum2_bytes convention)
  - SSH packet padding: total of (packet_length || padding_length || payload || padding) must be multiple of 8

### Programs on Disk

* 99 sbase Unix tools + sshd in /bin/
* 30 AIOS programs in /bin/aios/
* dash in /bin/dash (primary login shell)
* zsh in /bin/zsh (script mode, Phase 1 -- arrays, loops, arithmetic, extended globbing)
* tcc in /bin/tcc (compiler, tcc -c and tcc -o work, compile-and-run works)
* SDK in /usr/ (211 musl headers, augmented libc.a with AIOS runtime, libtcc1.a, custom CRT, tcc headers)
* Disk image: 256MB (was 128MB)
* Total: 135 programs

## Known Issues / Gotchas

* GCC 15 + musllibc: Patch vis.h and stdio_impl.h (protected->default visibility)
* platsupport Warning: Patched common.c (docs/patches/)
* GNU sed on macOS: PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
* texinfo: PATH="/opt/homebrew/opt/texinfo/bin:$PATH"
* CPIO caching: ninja does not detect child ELF changes, need full rebuild
* ELF buffer: Static 8MB (increased from 1MB in v0.4.69, dynamic for v0.5.x)
* DMA: Must use single untyped Retype for contiguous pages
* Priority: All processes at 200 (different = deadlock)
* Ctrl-C (SIGINT): two-stage delivery (v0.4.73, improved v0.4.87). First ^C sends SIGINT via PIPE_SIGNAL (wakes blocked PIPE_READ with EINTR). Second ^C force-kills via PIPE_SIGNAL(SIGKILL). fg_pid tracked via PIPE_EXEC, promoted to parent on child death. sshd ignores SIGINT (survives first ^C). SSH Ctrl-C via kill(0,2) in sshd relay.
* Socket cleanup: NET_CLEANUP_PID (v0.4.86) frees sockets by owner_pid on process death. Connection sockets inherit owner_pid from listen socket (v0.4.87).
* ext2: Never use packed structs on AArch64 (use rd16/rd32)
* aios-cc: Uses tr "/" "_" for object names (avoids cp.c vs libutil/cp.c collision)
* Process exit: Overridden to trigger VM fault (not seL4_DebugHalt)
* Multi-group ext2: Block allocation scans all groups; inode allocation group 0 only
* dash builtins.def: Must strip C-style comments before mkbuiltins (it is a shell script not C)
* Disk image is gitignored: must rebuild after rm -rf build-04 (sbase binaries lost)
* tty_echo: ON by default (v0.4.68). mini_shell/getty switch to RAW mode during line editing, COOKED for normal programs. Both buffers flushed on mode switch.
* Dash rebuild: dash links against libaios_posix.a but is not rebuilt by ninja. Must manually rebuild via aios-cc after any POSIX shim change.
* sshd rebuild: sshd also links libaios_posix.a via aios-cc. Must rebuild after posix_*.c or aios_posix.c changes. Failing to rebuild causes stale close()/exit() behavior.
* Virtio IRQ: Each virtio device needs its own seL4 IRQ handler (IRQ = 48 + slot). Never busy-poll with seL4_NBRecv in QEMU guest -- starves SLIRP event loop. Use blocking seL4_Recv with bound notification.
* Socket fd read/write: routed through posix_file.c (is_socket check) to net_server via NET_RECVFROM/NET_SENDTO IPC. Max 900 bytes per MR-based transfer.
* Morecore limit: 16MB exhausts VKA page frames (4096 BSS pages per process). 8MB is safe max with 8000-page pool. Increasing further requires demand paging or larger pool.
* zsh rebuild: zsh uses configure + make for headers, then aios-cc re-link. Requires libfakecurses.a stub and getrandom stub.

## Pending Items

1. ~~Ctrl-C signal delivery~~ FIXED in v0.4.73: two-stage SIGINT, fg_pid tracking for fork+exec, signal check in TTY_READ loop
2. virtio-blk stale reads: partially mitigated by dmb sy barrier (v0.4.73), needs stress testing. See docs/NEXT_20260409a.md
3. ~~tcc program TLS/IPC~~ FIXED in v0.4.72
4. ~~Networking~~ DONE in v0.4.74: virtio-net driver, ARP/ICMP, UDP sockets, TCP + HTTP server (M1-M4)
5. ~~Display server~~ DONE in v0.4.75: ramfb framebuffer, font rendering, display IPC, fbshow, UART IRQ
6. ~~TCC compilation~~ VERIFIED in v0.4.76: single-file, multi-file, multi-header, libc.a linking
7. ~~UART IRQ freeze~~ FIXED in v0.4.76: QEMU PL011 ICR race, RXFE check before seL4_Wait
8. ~~Large file open~~ FIXED in v0.4.76: fetch_stat for real size, FS_PREAD path works
9. ~~Stability fixes~~ DONE in v0.4.76: Mint/Copy/Register error checks, fs_ls_total race
10. ~~src/arch isolation~~ DONE in v0.4.76: arch.h dispatcher, aarch64 + x86_64 barrier/page headers
11. ~~DTB hardware discovery~~ DONE in v0.4.77: libfdt parsing, dynamic UART/virtio/fw_cfg addresses
12. ~~procfs reads broken~~ FIXED in v0.4.77: openat zero-size stat fallback, ls /proc directory mode
13. ~~zsh Phase 1~~ DONE in v0.4.88: script mode (1.19MB binary), echo/variables/loops/arithmetic verified
38. zsh Phase 2: interactive mode with ZLE (requires ZLE enable + termios integration)
12. Allocator right-sizing: 8000 pages (increased for 8MB morecore), monitor with debug command
13. Dash improvements: tab completion, history, PS1, job control
14. TTY improvements: process-aware echo, virtual terminals
15. ~~ext2 free blocks on unlink~~ DONE in v0.4.78: ext2_free_block/inode, BGDT count updates
16. ext2 write improvements: triple indirect
17. ~~O_NONBLOCK pipes~~ DONE in v0.4.79: pipe2 flags, fcntl F_GETFL/F_SETFL, server EAGAIN
18. ~~futex stub~~ DONE in v0.4.79: WAIT/WAKE for musl pthreads
19. ~~close() pipe EOF~~ FIXED in v0.4.79: last-write-ref PIPE_CLOSE_WRITE (builtin|external freeze)
20. ~~Zombie overflow~~ FIXED in v0.4.79: evicts oldest entry with warning
21. ~~Fork ReadRegisters leak~~ FIXED in v0.4.79: destroys child process + frees fault EP
22. /proc/self/fd listing: external command opendir does not route through client intercept (syscall dispatch investigation)
23. File redirects across exec: cmd > file lost after fork+exec (server-side fd table needed)
24. reap_check: cannot use seL4_NBRecv on minted fault EPs (steals from shared pipe_ep). Need different orphan detection approach.
25. ~~TCP stream sockets~~ DONE in v0.4.81: read()/write() on socket fds, 4KB circular RX buffer, dedicated virtio-net IRQ
26. ~~Virtio-net IRQ~~ FIXED in v0.4.81: dedicated seL4 IRQ handler, was piggybacking on UART
27. ~~SSH server (Phase 1-2)~~ DONE in v0.4.82: mbedTLS cross-compile, ECDSA-P256 host key, x25519 ECDH, KEXINIT, NEWKEYS verified with OpenSSH 10.3
28. TCP improvements: retransmission, window advertisement, connect() implementation, SHM data path
29. ~~SSH Phase 3~~ DONE in v0.4.83: AES-256-CTR + HMAC-SHA-256 encrypted transport
30. ~~SSH Phase 4~~ DONE in v0.4.83: password auth via auth_server IPC
31. ~~SSH Phase 5~~ DONE in v0.4.84: session channel, pty-req, shell spawning, data relay
32. ~~Pipe subsystem~~ FIXED in v0.4.85: ignore_next_fault bug, PIPE_READ_SHM -2, conditional read_closed, PIPE_DUP_REFS
33. ~~Ctrl-C regression~~ FIXED in v0.4.85: root-side fg_sigint_sent, force-kill via PIPE_SIGNAL, PIPE_WAIT signal interrupt
34. ~~Socket cleanup on process kill~~ DONE in v0.4.86: NET_CLEANUP_PID with owner_pid tracking
35. ~~SSH Ctrl-C to child commands~~ DONE in v0.4.87: PIPE_SIGNAL wakes blocked PIPE_READ, kill(0,sig)->fg_pid, EINTR dispatch
36. TCP improvements: retransmission timer, keepalive, SHM data path, TIME_WAIT
37. SSH improvements: host key persistence, window size, concurrent sessions
38. ~~PAL Phase 0~~ DONE in v0.4.89: 3 HAL interfaces, 3 platform drivers (blk+net+display), 14 globals privatized
39. ~~PAL cleanup (Step 7)~~ DONE in v0.4.90: shared plat_virtio_probe, bridge globals removed, 3 dead files deleted
40. ~~RPi4 Phase 1~~ DONE in v0.4.90: multi-target build, stub drivers, DTB parsers, mksdcard.py, hardware verified (HDMI framebuffer)
41. RPi4 Phase 2: BCM2835 SDHCI SD card driver (ext2 from disk, login on RPi4)
42. RPi4 Phase 3: BCM54213 GENET Ethernet driver (SSH into RPi4)
43. RPi4 Phase 4: VideoCore mailbox framebuffer (integrated display server)

## Version History (0.4.x)

| Version | Feature |
| --- | --- |
| v0.4.5 | SMP verified (4 cores) |
| v0.4.6 | virtio-blk driver |
| v0.4.7-8 | ext2 filesystem + navigation |
| v0.4.9 | Exec from shell |
| v0.4.10-11 | POSIX shim (printf, open/read/write/close) |
| v0.4.12-13 | POSIX programs + stat + silent exec |
| v0.4.14 | Unix-like shell with aliases |
| v0.4.15 | VFS + procfs + /proc |
| v0.4.16 | Process lifecycle tracking |
| v0.4.17-18 | 34 syscalls + getdents64 |
| v0.4.19 | ext2 write support |
| v0.4.20-21 | Auto-init + 30 programs + miniShell |
| v0.4.22-24 | ELF-from-disk + PATH search + env vars |
| v0.4.25 | aios-cc cross-compiler + kernel uname |
| v0.4.26 | 37 sbase tools + __wrap_main |
| v0.4.27 | sbase ls + chdir + fstatat fix |
| v0.4.28 | Crash recovery (exit via VM fault) |
| v0.4.29 | CWD propagation + 79 sbase tools + path resolution |
| v0.4.30 | pthreads: create, join, mutex (manual TCB in child VSpaces) |
| v0.4.31 | AIOS_LOG: 16KB ring buffer + serial echo + /proc/log + /proc/uptime |
| v0.4.32 | Auth server: SHA-3-512 (Keccak), user DB, sessions, /etc/passwd |
| v0.4.33 | Login prompt: password masking, 3 retries, logout |
| v0.4.34 | uid/gid propagation + getpwuid via auth IPC |
| v0.4.43 | su/passwd + file permissions + line editor + parallel sbase builder |
| v0.4.56 | ext2 block cache + POSIX overwrite + writev/readv pipes |
| v0.4.57 | pthreads in child processes + chdir validation |
| v0.4.58 | POSIX shim modularization (8 source files) |
| v0.4.59 | ext2 cache, getty, Signal Phase 3, rm fix, pseudo-inodes |
| v0.4.60 | Signal Phase 4 (EINTR), getconf (99/99 sbase) |
| v0.4.61 | Core POSIX 55/55, statx fix, PSCI shutdown, /bin/aios |
| v0.4.62 | Extended POSIX 81/81 (100%), posix_verify V3 98/98 |
| v0.4.63 | sbase integration test, utimensat fix |
| v0.4.64 | Allocator 4000pg, /dev/null, dup2 fix, ioctl TIOCGWINSZ, fcntl F_DUPFD, setpgid, pip_dest leak fix, dash port (compiled + linked, first boot) |
| v0.4.65 | Shared-memory pipes, VKA allocator audit, O_APPEND (>> redirect), pipe cleanup fix |
| v0.4.66 | Dash stdout working, FS_APPEND IPC op, shell >> via FS_APPEND, SHM pipe phase 2 server-side, PIPE_BUF_SIZE fix, aios-cc EXTRA flags fix, split_args_qa |
| v0.4.67 | SHM pipes client enabled, dash pipelines + semicolons + interactive, PIPE_SET_PIPES, stdin blocking, quote-aware pipe detection, cap copy cleanup |
| v0.4.68 | Dash as primary login shell, tty_echo=1, TTY_READ cooked stdin, RAW/COOKED mode switching, getty reads pw_shell from /etc/passwd, buffer isolation on mode switch, macOS sed fix |
| v0.4.69 | ELF buffer 8MB, morecore 4MB, termios TCGETS/TCSETS, bump scripts sed fix, DESIGN_TCC.md, DESIGN_ZSH.md |
| v0.4.70 | tcc cross-compiled and running, tcc -c produces .o files, FS_PREAD (large file reads), FS_PWRITE (positioned writes), writev for regular files, tcc SDK on disk (211 headers + libc.a), ext2 block allocation for writes |
| v0.4.71 | tcc -o linking (custom CRT, augmented libc, TLS LE relocs, MRI merge), small file truncation fix, munmap reclaim, build_apps.py. Blocked: virtio-blk stale reads + tcc program TLS/IPC |
| v0.4.72 | tcc compile-and-run working (3 fixes: __sysinfo init, muslcsys_init_muslc, arm64 direct ADRP). Shell > and >> redirect for builtins. Empty file open fix. See docs/NEXT_20260409b.md |
| v0.4.74 | Networking: virtio-net driver (128KB DMA, two-thread RX/TX), ARP/ICMP/UDP/TCP protocol stack, POSIX socket API, HTTP server user program. See docs/NEXT_20260410b.md |
| v0.4.73 | Second virtio-blk drive (log disk), ext2 multi-device cache (dev_id), volume label disk probe, file-based logging (/log/aios.log), ext2 inode_size from superblock, alloc_block group_start fix, post-completion dmb sy barrier, create_file 0755 permissions. See docs/NEXT_20260410a.md |
| v0.4.75 | Display server: ramfb framebuffer (1024x768), 8x8 bitmap font, splash images, display server IPC (5 commands), fbshow user program, disp_ep capability propagation, UART IRQ notification (seL4_Wait replaces polling). See docs/NEXT_20260411a.md |
| v0.4.76 | TCC compilation verified (single/multi-file, libc.a linking). UART IRQ freeze fix (QEMU PL011 ICR race). Large file open fix (fetch_stat for real size). Stability: seL4 cap error checks, fs_ls_total race. Architecture isolation: src/arch/ layer with aarch64 + x86_64 barrier/page macros (34 inline asm replaced). See docs/NEXT_20260411b.md |
| v0.4.77 | DTB hardware discovery (libfdt): UART, virtio, fw_cfg, CPU, memory from device tree. Hardcoded MMIO addresses replaced with DTB values. Dynamic /proc/version. Procfs zero-size open fix. ls /proc directory listing fix. See docs/NEXT_20260411c.md |
| v0.4.78 | Linux compat layer (10 syscalls: getrandom, ppoll, prlimit64, sysinfo, etc.). ext2 block/inode freeing on unlink with BGDT count updates. /dev/urandom, /dev/random, /dev/zero virtual devices. /proc/cpuinfo, /proc/stat, /proc/loadavg. readlinkat /proc/self/exe. Bumped MAX_ZOMBIES=16, MAX_PIPES=16, MAX_WAIT_PENDING=8. See docs/NEXT_20260411d.md |
| v0.4.79 | O_NONBLOCK for pipes (pipe2 flags, fcntl F_GETFL/F_SETFL, server EAGAIN). futex stub (WAIT/WAKE). /proc/self/fd stat+readlinkat. close() EOF fix (last-write-ref PIPE_CLOSE_WRITE). Zombie overflow guard. Fork ReadRegisters cleanup. procfs /self directory. See docs/NEXT_20260411e.md |
| v0.4.80 | Dynamic config: shared key=value parser (config.h, config_parser.c), boot_load_config reads /etc/hostname + /etc/network.conf + /etc/environment. Runtime network IP/gateway/mask (net_cfg_ip arrays replace NET_IP macros). Dynamic environment from disk. auth_server /bin/sh -> /bin/dash. uname fallback updated. /proc/mounts shows log drive. USER= from login uid. Test scripts at /bin/tests/. See docs/NEXT_20260411f.md |
| v0.4.81 | TCP stream sockets: read()/write() on socket fds via NET_RECVFROM/NET_SENDTO IPC. 4KB circular TCP RX buffer (ring with rx_head/rx_tail/rx_eof). Dedicated virtio-net IRQ handler (was piggybacking on UART). Event-driven net_server (blocking seL4_Recv replaces busy-poll). Socket stubs: connect, getsockname, getpeername, getsockopt. echo_tcp test app. Getty version banner. See docs/NEXT_20260411g.md |
| v0.4.82 | mbedTLS 3.6.3 cross-compile (81 objects, libmbedcrypto.a). SSH Phase 1-2: ECDSA-P256 host key, x25519 ECDH, KEXINIT, NEWKEYS. See docs/NEXT_20260412a.md |
| v0.4.83 | SSH Phase 3: AES-256-CTR + HMAC-SHA-256 encrypted transport. Phase 4: password auth via auth_server IPC. TCP partial read fix. Strict KEX seq reset. See docs/NEXT_20260412a.md |
| v0.4.84 | SSH Phase 5: session channel, shell spawn, interactive relay. O_NONBLOCK TCP sockets. PIPE_READ_SHM false-EOF fix. PIPE_EXEC ref counting + TCB fault suppression. See docs/NEXT_20260412a.md |
| v0.4.85 | Pipe subsystem fixes: ignore_next_fault bug (v0.4.84), PIPE_READ_SHM -2, conditional read_closed, PIPE_DUP_REFS handler. Ctrl-C: root-side fg_sigint_sent, force-kill via PIPE_SIGNAL(SIGKILL), PIPE_WAIT signal interrupt. sshd SIGINT ignore. Net debug printfs removed, recvfrom O_NONBLOCK. See docs/NEXT_20260412b.md |
| v0.4.86 | TCP connect() (3-way handshake, SYN_SENT state), dynamic window, seq validation, socket PID tracking + NET_CLEANUP_PID, deferred net cleanup (seL4 reply cap fix), fg parent promotion, sshd restart fix. See docs/NEXT_20260412c.md |
| v0.4.87 | Signal delivery to blocked PIPE_READ (EINTR wakeup), SSH Ctrl-C via kill(0,2)+fg_pid, SSH exit (pipe EOF via server-side close), TCP retransmit tolerance (overlapping segment handling), exit path pipe close fixes. See docs/NEXT_20260412d.md |
| v0.4.88 | ZSH Phase 1 (script mode, 1.19MB binary, 7 modules), morecore 4->8MB, allocator pool 4000->8000 pages, TCC + SDK rebuild with augmented libc, aios_entropy.c for mbedTLS, disk 128->256MB. See docs/NEXT_20260413a.md |
| v0.4.89 | Platform Abstraction Layer Phase 0 complete: DESIGN_RPI.md, src/plat/ (3 HAL interfaces), blk_virtio.c + net_virtio.c + display_ramfb.c (all drivers behind HAL), 14 globals privatized, boot files reduced 894->316 lines, PLAT_QEMU_VIRT build define, cortex-a72 verified. See docs/NEXT_20260413a.md |
| v0.4.90 | PAL Step 7 cleanup: shared plat_virtio_probe (MMIO scan), 5 bridge globals removed, 3 dead files deleted (-321 net). RPi4 Phase 1: multi-target build (AIOS_SETTINGS, PLAT_RPI4), stub drivers (blk/net/display), DTB parsers (emmc/genet/vc_mbox), mksdcard.py SD card builder, RPi4 hardware verified (boots AArch64, HDMI framebuffer via VideoCore mailbox). See docs/NEXT_20260413a.md |

## Architecture After v0.4.76

```
src/arch/
  arch.h                     -- architecture dispatcher
  aarch64/barriers.h          -- dmb, dsb, isb macros
  aarch64/page.h              -- ARCH_PAGE_OBJECT, arch_page_get_address
  x86_64/barriers.h           -- mfence stubs
  x86_64/page.h               -- x86 page stubs
```

## Architecture After v0.4.74

```
src/aios_root.c           ~200 lines
src/lib/aios_posix.c      ~580 lines
src/lib/posix_internal.h   ~272 lines (+ shm_vaddr)
src/lib/posix_file.c       ~647 lines (+ SHM pipe client, stdin blocking)
src/lib/posix_stat.c       ~318 lines
src/lib/posix_dir.c        ~188 lines
src/lib/posix_proc.c       ~325 lines
src/lib/posix_time.c       ~121 lines
src/lib/posix_misc.c       ~250 lines (+ PIPE_SET_PIPES from dup2)
src/lib/posix_thread.c     ~195 lines
src/lib/posix_net.c        ~170 lines
src/lib/vka_audit.c        ~65 lines
src/vfs.c                  ~170 lines
src/procfs.c               ~286 lines
src/servers/exec_server.c  ~388 lines
src/servers/pipe_server.c  ~1105 lines (+ SHM client, cap cleanup, PIPE_SET_PIPES)
src/servers/fs_server.c    ~343 lines (+ FS_APPEND)
src/servers/thread_server.c ~227 lines
src/servers/net_driver.c   ~95 lines  (+ virtio ISR/IRQ ACK)
src/servers/net_server.c   ~480 lines (+ TCP circular RX buffer, blocking Recv)
src/net/net_stack.c        ~350 lines
src/net/net_tcp.c          ~100 lines
src/boot/boot_services.c   ~117 lines
src/boot/boot_net_init.c   ~140 lines
src/process/fork.c         ~495 lines
src/process/reap.c         ~130 lines
src/apps/mini_shell.c      ~1312 lines (+ quote-aware pipe/chain detection)
include/aios/root_shared.h ~227 lines (+ PIPE_SET_PIPES, xfer_copies)
include/aios/vka_audit.h   ~38 lines
include/aios/net.h         ~210 lines
```

## Test Results (v0.4.76)

```
posix_verify V3: 98/98 PASS
signal_test: 20/20 PASS
POSIX audit: 81/81 (100%)
dash as login shell: PASS
Second drive mount: PASS (volume label probe, either QEMU order)
ls /log: PASS (log ext2 directory listing)
cat /log/aios.log: PASS (boot entries with timestamps)
cd /log: PASS (cross-mount chdir)
echo hello > /tmp/t.txt: PASS
echo line2 >> /tmp/t.txt: PASS
tcc -o single file (stdio.h): PASS (hello from tcc)
tcc -o multi-header (stdio+string+stdlib): PASS (sum=6 len=5)
tcc -c + multi-file link: PASS (add=7, mul=30)
fork_test: PASS
pipe: echo hello | cat -> hello (PASS)
Ctrl-C on tail -f: PASS (clean exit back to shell prompt)
virtio-net detection: PASS
ARP: PASS (gratuitous ARP + request/reply)
ICMP ping: PASS (echo request/reply)
UDP socket: PASS (sendto/recvfrom)
TCP handshake: PASS (SYN/SYN-ACK/ACK)
HTTP curl: PASS (curl localhost:8080 returns response)
Programs on disk: 132+ (99 sbase + 28 aios + dash + tcc + hello_test + httpd)
Drives: 2 (128MB system + 16MB log)
RAM: 476 MB
```
