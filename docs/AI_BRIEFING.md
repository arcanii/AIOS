# AIOS AI Briefing Document

## Project Overview

**AIOS (Open Aries)** is a microkernel operating system built on seL4, targeting AArch64/QEMU.

- **Repository**: https://github.com/arcanii/AIOS
- **Branch**: main
- **Developer**: Bryan
- **Current Version**: v0.4.29

## Development Environment

- **Host**: macOS Apple Silicon M3 Max
- **Cross-compiler**: aarch64-linux-gnu-gcc (Homebrew, GCC 15)
- **Build system**: CMake + Ninja
- **Scripts**: Python 3 (preferred), Bash (for simple ops)
- **Disk tools**: Custom Python ext2 builder (scripts/ext2/builder.py)
- **QEMU**: qemu-system-aarch64

## Session Protocols

### Bump-Patch Protocol

ALWAYS bump-patch at the start of each new work phase. This sets the version for the work about to be done.

    ./scripts/bump-patch.sh
    ./scripts/version.sh

- Commit the previous version FIRST, then bump for the next phase
- Version format: v0.4.XX (staying on 0.4.x throughout this development cycle)
- Never use bump-minor unless explicitly directed
- Build number auto-increments on each ninja build

### Script Naming Protocol

When presenting multiple scripts to the user, ALWAYS label them:

- **Script A** — description
- **Script B** — description
- **Script C** — description

This allows the user to report "Script B failed" without ambiguity.

### Code Change Protocol

- Use Python for file edits (heredoc quoting is unreliable in zsh with special chars)
- Avoid sed for multi-line edits or edits with quotes/slashes
- Always verify changes applied: grep for expected content after edit
- Full rebuild required when CPIO contents change (rm -rf build-04)
- Incremental ninja sufficient for aios_root.c or aios_posix.c changes

### Commit Protocol

- Commit at each milestone with version tag in message
- Format: "v0.4.XX: short description\n\ndetails"
- git push origin main after each commit

### No Hacks Protocol

- Pure POSIX — no alias tables, no prefix stripping, no magic
- Shell searches $PATH, sends full path to exec_thread
- exec_thread loads exactly what it's given
- mkdisk installs files with original names
- Temporary workarounds must be documented and removed when proper solution available

## Build & Boot Commands

### Full Rebuild

    cd ~/Desktop/github_repos/AIOS
    rm -rf build-04 && mkdir build-04 && cd build-04
    cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
        -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
    ninja

### Rebuild sbase (after aios_posix changes)

    cd ~/Desktop/github_repos/AIOS
    SBASE=~/Desktop/github_repos/sbase
    mkdir -p build-04/sbase
    for cmd in true false; do
        ./scripts/aios-cc $SBASE/${cmd}.c -o build-04/sbase/${cmd}
    done
    for cmd in echo yes basename dirname hostname logname whoami; do
        ./scripts/aios-cc $SBASE/${cmd}.c $SBASE/libutil/*.c -I $SBASE -o build-04/sbase/${cmd}
    done
    for cmd in cat head wc sort tee sleep link unlink tty printenv \
               pwd env uname date cal cksum comm cmp cut expand \
               fold nl paste rev seq strings tail tr ls grep \
               chmod chown dd du find join kill ln mkdir mkfifo \
               mktemp mv nice nohup od printf readlink rmdir \
               sed split sync touch tsort unexpand uniq which xargs \
               test expr chgrp chroot dc logger md5sum sha256sum \
               sha512sum sponge pathchk; do
        ./scripts/aios-cc $SBASE/${cmd}.c $SBASE/libutil/*.c $SBASE/libutf/*.c \
            -I $SBASE -o build-04/sbase/${cmd} 2>/dev/null
    done
    ./scripts/aios-cc $SBASE/cp.c $SBASE/libutil/*.c $SBASE/libutf/*.c -I $SBASE -o build-04/sbase/cp
    ./scripts/aios-cc $SBASE/rm.c $SBASE/libutil/*.c $SBASE/libutf/*.c -I $SBASE -o build-04/sbase/rm

### Install to Disk

    python3 scripts/mkdisk.py disk/disk_ext2.img 128 \
        --rootfs disk/rootfs \
        --install-elfs build-04/sbase \
        --install-elfs build-04/projects/aios/

### Boot QEMU

    qemu-system-aarch64 \
        -machine virt,virtualization=on \
        -cpu cortex-a53 -smp 4 -m 2G \
        -nographic -serial mon:stdio \
        -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
        -device virtio-blk-device,drive=hd0 \
        -kernel build-04/images/aios_root-image-arm-qemu-arm-virt

### Cross-Compile External Programs

    ./scripts/aios-cc source.c -o output

## Repository Structure

    AIOS/
    ├── src/
    │   ├── aios_root.c          # Root task: boot, drivers, exec_thread, fs_thread
    │   ├── ext2.c               # ext2 filesystem (read + write + indirect blocks)
    │   ├── vfs.c                # Virtual filesystem switch (mount dispatch)
    │   ├── procfs.c             # /proc virtual filesystem
    │   ├── lib/
    │   │   ├── aios_posix.c     # POSIX syscall shim (40+ syscalls)
    │   │   └── aios_posix.h     # POSIX shim header + AIOS_INIT macro
    │   └── apps/                # AIOS-specific programs (9 remaining)
    ├── include/aios/
    │   ├── version.h, build_number.h, ext2.h, vfs.h, procfs.h
    ├── scripts/
    │   ├── mkdisk.py            # Disk image CLI
    │   ├── ext2/builder.py      # ext2 image builder (multi-group, indirect blocks)
    │   ├── aios-cc              # Cross-compiler wrapper
    │   ├── bump-patch.sh, bump-minor.sh, bump-build.sh, version.sh
    │   └── posix_audit.py       # POSIX compliance audit
    ├── disk/
    │   ├── disk_ext2.img        # 128MB ext2 disk image (gitignored)
    │   └── rootfs/              # Filesystem content overlay
    ├── docs/
    │   ├── AI_BRIEFING.md       # This file
    │   ├── ARCHITECTURE.md, DESIGN_0.4.md, LEARNINGS.md
    │   └── patches/             # Documented dep patches
    ├── projects/aios/CMakeLists.txt  # Build config
    ├── deps/                    # gitignored: seL4, musllibc, etc.
    ├── build-04/                # gitignored: build output
    │   ├── sbase/               # Cross-compiled sbase tools
    │   └── projects/aios/       # AIOS programs
    └── settings.cmake           # seL4 kernel config

## Architecture

### Boot Sequence

1. ELF-loader loads kernel + root task
2. Root task initializes: allocator, SMP, virtio-blk, ext2, VFS
3. VFS mounts: / (ext2) and /proc (procfs)
4. Spawns serial_server + mini_shell from CPIO
5. Starts fs_thread + exec_thread

### Process Model

- serial_server: UART I/O via IPC (PUTC/GETC)
- fs_thread: VFS dispatch (LS, CAT, STAT, MKDIR, WRITE, UNLINK, UNAME)
- exec_thread: Reads ELF from /bin/, loads into new VSpace, manages lifecycle
- mini_shell: PATH search, CWD, environment variables, exec launcher
- User programs: isolated VSpaces, POSIX syscalls via IPC

### ELF Loading (from disk)

1. Shell searches $PATH, sends full path to exec_thread
2. exec_thread reads ELF via VFS (ext2 with indirect blocks)
3. elf_newFile() parses from 1MB static buffer (TODO: dynamic)
4. sel4utils_elf_load() maps segments into child VSpace
5. __wrap_main intercepts: extracts caps [ser, fs, CWD], sets CWD, calls real main
6. Process exits via VM fault -> exec_thread cleans up -> shell resumes

### POSIX Shim (aios_posix.c)

- __wrap_main: intercepts main(), strips cap args, inits shim
- argv from exec: [serial_ep, fs_ep, CWD, progname, arg1, ...]
- Programs see clean POSIX argv: [progname, arg1, ...]
- 40+ syscalls overridden via muslcsys_install_syscall()
- resolve_path() handles relative paths against CWD
- Environment variables set via environ pointer

### IPC Protocol Labels

    SER_PUTC=1, SER_GETC=2
    FS_LS=10, FS_CAT=11, FS_STAT=12
    FS_MKDIR=14, FS_WRITE_FILE=15, FS_UNLINK=16, FS_UNAME=17
    EXEC_RUN=20

### Implemented Syscalls (40+)

File I/O: open, openat (O_CREAT), read, readv, write, writev, close, lseek
Directories: getdents64, chdir, mkdirat, unlinkat
Stat: fstat, fstatat
Identity: getpid, getppid, getuid, geteuid, getgid, getegid
System: uname (kernel IPC), getcwd, ioctl, fcntl, dup, dup3
Access: access, faccessat, umask, utimensat
Time: clock_gettime, gettimeofday, nanosleep (ARM generic timer)
Process: exit, exit_group (VM fault for clean recovery)
Memory: mmap, munmap, brk, madvise (from muslcsys)

## External Tools

### sbase (suckless base utilities)

- Source: ~/Desktop/github_repos/sbase
- 79 tools: ls, cat, head, wc, sort, grep, sed, find, cp, rm, mkdir, etc.
- Cross-compiled via ./scripts/aios-cc with libutil + libutf
- Note: cp.c and rm.c need special handling (libutil has cp.c/rm.c too)

### Programs on Disk

- 94 total programs in /bin/
- 79 sbase Unix tools
- 9 AIOS-specific programs
- Boot services in CPIO: serial_server, mini_shell

## Known Issues / Gotchas

- GCC 15 + musllibc: Patch vis.h and stdio_impl.h (protected->default visibility)
- platsupport Warning: Patched common.c (docs/patches/)
- GNU sed on macOS: PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
- texinfo: PATH="/opt/homebrew/opt/texinfo/bin:$PATH"
- CPIO caching: ninja doesn't detect child ELF changes, need full rebuild
- ELF buffer: Static 1MB (TODO: dynamic for v0.5.x)
- DMA: Must use single untyped Retype for contiguous pages
- Priority: All processes at 200 (different = deadlock)
- ext2: Never use packed structs on AArch64 (use rd16/rd32)
- aios-cc: Uses tr "/" "_" for object names (avoids cp.c vs libutil/cp.c collision)
- Process exit: Overridden to trigger VM fault (not seL4_DebugHalt)
- Multi-group ext2: Block allocation scans all groups; inode allocation group 0 only

## Pending Items

1. Shell: piping, redirection, tab completion
2. POSIX: more syscalls (symlink, chmod, chown, pipe, dup2)
3. Networking: virtio-net + TCP/IP
4. Dynamic ELF buffer: vka_alloc_frame for >1MB binaries
5. Process niceness: runtime seL4_TCB_SetPriority
6. ext2 write improvements: multi-block file write, triple indirect

## Version History (0.4.x)

| Version | Feature |
|---------|---------|
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
