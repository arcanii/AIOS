# AIOS — Open Aries

A microkernel operating system built on seL4, targeting AArch64.

## Vision

AIOS is a research operating system exploring AI-native development on a formally verified microkernel. External AI (Claude, etc.) generates and reviews code, which is compiled and deployed to AIOS. The long-term goal is self-hosted development within AIOS itself.

Security is a first-class design concern — seL4's capability-based isolation provides the foundation.

This is in an experimental/research phase. Collaborators welcome.

**AI Development Study**: This project serves as a study in AI-assisted OS development, currently using Claude Opus 4.6.

## Current State (v0.4.43)

AIOS boots on QEMU (aarch64, Cortex-A53, 4-core SMP) and provides a Unix-like environment:

- **95+ programs** in `/bin/`, loaded from ext2 filesystem on disk
- **79 sbase (suckless) Unix tools**: ls, cat, head, wc, sort, grep, sed, find, cp, rm, mkdir, touch, date, cal, seq, tr, and more
- **POSIX syscall shim**: 50+ syscalls (open, read, write, close, stat, opendir, getdents, uname, getpid, clock_gettime, pipe2, renameat, ...)
- **ext2 filesystem**: read/write, indirect blocks, multi-group allocation
- **VFS layer**: mount points (/ for ext2, /proc for procfs)
- **Process management**: isolated VSpaces, fault recovery, process table, background exec, kill
- **Unix pipes**: `cmd1 | cmd2 | cmd3` with 8KB ring buffers via pipe server
- **Shell**: line editor, command history, `$VAR` expansion, `&&`/`||` chains, `>` `<` redirection, quote stripping, Ctrl-C
- **Authentication**: SHA-3-512 password hashing, login/logout, su/passwd, MMU-isolated auth server
- **File permissions**: badge-based enforcement, non-root denied write to /etc/ and /bin/
- **pthreads**: create, join, mutex via thread server (real seL4 TCBs)
- **Kernel log**: 16KB ring buffer, /proc/log, dmesg builtin
- **Process viewer**: `aios_top` with ANSI refresh, quit with 'q'
- **Cross-compiler**: any POSIX C program compiles with `./scripts/aios-cc` and runs unmodified
- **Parallel sbase builder**: `python3 scripts/build_sbase.py` (79 tools, 16 jobs, 11s)

### What It Looks Like

    ============================================
      AIOS 0.4.x miniShell
    ============================================

    / $ ls
    bin  dev  etc  hello.txt  home  proc  sbin  tmp  var
    / $ ls -l /etc
    -rwxr-xr-x    1 root     0                72 Jan 01 00:00 fstab
    -rwxr-xr-x    1 root     0                 5 Jan 01 00:00 hostname
    -rwxr-xr-x    1 root     0               347 Jan 01 00:00 motd
    -rwxr-xr-x    1 root     0                30 Jan 01 00:00 passwd
    / $ cat /etc/hostname
    aios
    / $ date
    Thu Jan  1 00:02:17 GMT 1970
    / $ find /etc
    /etc
    /etc/fstab
    /etc/hostname
    /etc/motd
    /etc/passwd
    /etc/services.conf
    / $ md5sum /etc/hostname
    a0725f97be8260e3ce6a5d28db43c089  /etc/hostname
    / $ cal
        January 1970
    Su Mo Tu We Th Fr Sa
                 1  2  3
     4  5  6  7  8  9 10
    11 12 13 14 15 16 17
    18 19 20 21 22 23 24
    25 26 27 28 29 30 31

## Architecture

    ┌─────────────────────────────────────────────────┐
    │                 User Programs                    │
    │  ls, cat, grep, find, sed, sort, cp, rm, ...    │
    │  (79 sbase tools + 9 AIOS programs)             │
    ├─────────────────────────────────────────────────┤
    │              POSIX Syscall Shim                  │
    │  open/read/write/close/stat/opendir/uname/...   │
    │  (40+ syscalls via muslcsys_install_syscall)     │
    ├─────────────────────────────────────────────────┤
    │           miniShell (PATH, env, CWD)             │
    ├──────────┬──────────┬──────────┬────────────────┤
    │ serial   │    fs    │   exec   │    procfs      │
    │ server   │  thread  │  thread  │   (/proc)      │
    │ (UART)   │  (VFS)   │ (ELF    │                 │
    │          │          │  loader) │                 │
    ├──────────┴──────────┴──────────┴────────────────┤
    │              VFS (Virtual Filesystem)            │
    │         /  →  ext2     /proc  →  procfs          │
    ├─────────────────────────────────────────────────┤
    │    ext2 Driver          │    virtio-blk Driver   │
    │  (read/write, indirect  │    (DMA, legacy v1)    │
    │   blocks, multi-group)  │                        │
    ├─────────────────────────┴────────────────────────┤
    │                seL4 Microkernel                   │
    │  Capabilities, VSpaces, IPC Endpoints, SMP (4)   │
    │  EL2 Hypervisor Mode, ARM Generic Timer          │
    ├─────────────────────────────────────────────────┤
    │          QEMU virt (Cortex-A53 x4, 2GB)          │
    └─────────────────────────────────────────────────┘

## Building

### Prerequisites

- macOS (Apple Silicon) or Linux
- aarch64-linux-gnu-gcc cross-compiler
- CMake, Ninja
- Python 3
- QEMU (qemu-system-aarch64)

On macOS:

    brew install aarch64-unknown-linux-gnu cmake ninja qemu python3 gnu-sed
    echo 'export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"' >> ~/.zshrc

### Build

    # Build kernel + root task + programs
    cd AIOS
    rm -rf build-04 && mkdir build-04 && cd build-04
    cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
        -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
    ninja
    cd ..

    # Create disk image with rootfs and programs
    python3 scripts/mkdisk.py disk/disk_ext2.img 128 \
        --rootfs disk/rootfs \
        --install-elfs build-04/sbase \
        --install-elfs build-04/projects/aios/

### Boot

    qemu-system-aarch64 \
        -machine virt,virtualization=on \
        -cpu cortex-a53 -smp 4 -m 2G \
        -nographic -serial mon:stdio \
        -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
        -device virtio-blk-device,drive=hd0 \
        -kernel build-04/images/aios_root-image-arm-qemu-arm-virt

### Cross-Compile Your Own Programs

Any standard POSIX C program can run on AIOS with no modifications:

    // hello.c — no AIOS-specific code needed
    #include <stdio.h>
    #include <unistd.h>

    int main(int argc, char *argv[]) {
        printf("Hello from AIOS!\n");
        printf("PID: %d, UID: %d\n", getpid(), getuid());
        return 0;
    }

    ./scripts/aios-cc hello.c -o build-04/sbase/hello

## Programs Available

### sbase Unix Tools (79)

| Category | Commands |
|----------|----------|
| Files | cat, cp, mv, rm, ln, mkdir, rmdir, touch, chmod, chown, chgrp |
| Text | head, tail, wc, sort, uniq, cut, paste, fold, expand, tr, rev, nl, comm, join |
| Search | grep, find, strings |
| Info | ls, du, date, cal, uname, hostname, whoami, logname, tty, printenv, env |
| Crypto | md5sum, sha256sum, sha512sum, cksum |
| Stream | tee, sed, dd, split, seq |
| Misc | echo, yes, true, false, basename, dirname, pwd, sleep, expr, dc, test, printf |
| System | kill, nice, nohup, sync, which, xargs |

### AIOS-Specific Programs (9)

| Program | Purpose |
|---------|---------|
| sysinfo | System information display |
| posix_test | POSIX compliance test suite (26 tests) |
| posix_ps | Process status (reads /proc/status) |
| posix_id | UID/GID display |
| posix_help | Command listing |
| posix_mkdir | mkdir via filesystem IPC |
| posix_touch | touch via filesystem IPC |
| posix_rm | rm via filesystem IPC |
| posix_nice | Nice value placeholder |

## POSIX Compliance

26/26 tests pass covering:

- File I/O: open, read, write, lseek, close
- Stat: fstat, fstatat
- Identity: getpid, getppid, getuid, geteuid, getgid, getegid
- System: uname (via kernel IPC)
- Filesystem: getcwd, opendir, readdir, closedir
- Time: clock_gettime (ARM generic timer), gettimeofday
- Access: access (exists + nonexist)
- Descriptors: dup

Run `posix_test` in the shell to verify.

## Design History

| Phase | Status |
|-------|--------|
| 0.1.x | Initial experiments — abandoned |
| 0.2.x | Monolithic design — hit scaling wall |
| 0.3.x | Microkit/PD-based — redesigned for seL4 idioms |
| 0.4.x | **Current** — sel4utils processes, POSIX shim, Unix tools |

### 0.4.x Version History

| Version | Feature |
|---------|---------|
| v0.4.5 | SMP verified (4 cores via PSCI/SMC) |
| v0.4.6 | virtio-blk driver (legacy v1, DMA) |
| v0.4.7-8 | ext2 filesystem reader + shell navigation |
| v0.4.9 | Exec from shell (CPIO) |
| v0.4.10-11 | POSIX shim (printf, open/read/write/close via IPC) |
| v0.4.12-14 | POSIX programs + Unix-like shell |
| v0.4.15-16 | VFS + procfs + process lifecycle |
| v0.4.17-18 | 34 syscalls + getdents64 (opendir/readdir) |
| v0.4.19 | ext2 write support (mkdir, touch, rm) |
| v0.4.20-21 | Auto-init + miniShell + 30 programs |
| v0.4.22-24 | ELF-from-disk loader + PATH search + env vars |
| v0.4.25 | aios-cc cross-compiler wrapper |
| v0.4.26-27 | 37 sbase tools + __wrap_main + sbase ls |
| v0.4.28 | Crash recovery (exit via VM fault) |
| v0.4.29 | CWD propagation + 79 sbase tools + path resolution |
| v0.4.30 | pthreads: create, join, mutex (manual TCB in child VSpaces) |
| v0.4.31 | AIOS_LOG: 16KB ring buffer + serial echo + /proc/log + /proc/uptime |
| v0.4.32 | Auth server: SHA-3-512 (Keccak), user DB, sessions, /etc/passwd |
| v0.4.33 | Login prompt: password masking, 3 retries, logout |
| v0.4.34 | uid/gid propagation + getpwuid via auth IPC + parallel sbase builder |
| v0.4.35 | su/passwd + file permissions + line editor |
| v0.4.36 | Auth promoted to isolated process (MMU-isolated credentials) |
| v0.4.37 | uname IPC + shell && / || chains |
| v0.4.38 | Unix pipes + pipe server + UART buffer expansion |
| v0.4.39 | Quotes + redirection + multi-pipe |
| v0.4.40 | procfs enhancements + top + dmesg |
| v0.4.41 | MOTD, command history, top quit, PID 1 fix |
| v0.4.42 | $VAR expansion, Ctrl-C kills foreground, kill builtin |
| v0.4.43 | Background exec (&), jobs, rename/mv, write-to-fd |

## Roadmap

- [ ] Shell piping and redirection
- [ ] More POSIX syscalls (pipe, dup2, symlink, chmod at runtime)
- [ ] Dynamic ELF buffer (>1MB programs)
- [ ] virtio-net + TCP/IP networking
- [ ] Process niceness (runtime priority adjustment)
- [ ] Self-hosted development tools
- [ ] AI integration within the OS

## Documentation

- `docs/AI_BRIEFING.md` — Complete technical briefing for AI assistants
- `docs/LEARNINGS.md` — Hard-won debugging knowledge
- `docs/ARCHITECTURE.md` — System architecture
- `docs/DESIGN_0.4.md` — 0.4.x design decisions
- `docs/patches/` — Documented dependency patches

## License

MIT License. See LICENSE file.

Copyright (c) 2025-2026 AIOS Project
