# AIOS — Open Aries

A microkernel operating system built on seL4, targeting AArch64.

## Vision

AIOS is a research operating system exploring AI-native development on a formally verified microkernel. External AI (Claude, etc.) generates and reviews code, which is compiled and deployed to AIOS. The long-term goal is self-hosted development within AIOS itself.

Security is a first-class design concern — seL4's capability-based isolation provides the foundation.

This is in an experimental/research phase. Collaborators welcome.

**AI Development Study**: This project serves as a study in AI-assisted OS development, currently using Claude Opus 4.6.

## Current State (v0.4.50)

AIOS boots on QEMU (aarch64, Cortex-A53, 4-core SMP) and provides a Unix-like environment with fork+exec+waitpid:

- **116 programs** in `/bin/`, loaded from ext2 filesystem on disk
- **93 sbase (suckless) Unix tools**: ls, cat, head, wc, sort, grep, sed, find, cp, rm, mkdir, touch, date, cal, seq, tr, sha1sum, sha512sum, bc, ed, tar, and more
- **fork()+exec()+waitpid()**: Full process creation — believed to be first on bare seL4 without CAmkES/Microkit
- **POSIX syscall shim**: 55+ syscalls (open, read, write, close, stat, fork, execve, waitpid, getpid, pipe2, ...)
- **ext2 filesystem**: read/write, indirect blocks, multi-group allocation
- **VFS layer**: mount points (/ for ext2, /proc for procfs)
- **Process management**: isolated VSpaces, fork/exec/wait, fault recovery, process table, background exec, kill
- **Unix pipes**: `cmd1 | cmd2 | cmd3` with 8KB ring buffers via pipe server
- **TTY server**: line discipline (cooked/raw mode), Ctrl-C/U/W/D, echo control
- **Shell**: line editor, command history, `$VAR` expansion, `&&`/`||` chains, `>` `<` redirection, quote stripping, Ctrl-C
- **Authentication**: SHA-3-512 password hashing, login/logout, su/passwd, MMU-isolated auth server
- **File permissions**: badge-based enforcement, non-root denied write to /etc/ and /bin/
- **pthreads**: create, join, mutex via thread server (real seL4 TCBs)
- **Kernel log**: 16KB ring buffer, /proc/log, dmesg builtin
- **Process viewer**: `aios_top` with ANSI refresh, quit with 'q'
- **Cross-compiler**: any POSIX C program compiles with `./scripts/aios-cc` and runs unmodified
- **Parallel sbase builder**: `python3 scripts/build_sbase.py` (93 tools, 16 jobs, ~14s)

### What It Looks Like

    ============================================
      AIOS 0.4.x
    ============================================

    / $ fork_test
    fork_test: about to fork+exec (pid=9)...
    AIOS System Information
    =======================
    Kernel:    seL4 15.0.0
    Arch:      AArch64 (ARMv8-A)
    CPU:       Cortex-A53
    Cores:     4 (SMP)
    =======================
    fork_test: child=10 exited with 0

    / $ ls
    bin  dev  etc  hello.txt  home  proc  sbin  tmp  var
    / $ cat /etc/hostname
    aios
    / $ date
    Thu Jan  1 00:02:17 GMT 1970
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
    │  fork_test, sysinfo, posix_test                 │
    │  (93 sbase tools + 20 AIOS programs)            │
    ├─────────────────────────────────────────────────┤
    │              POSIX Syscall Shim                  │
    │  open/read/write/close/stat/fork/exec/waitpid   │
    │  (55+ syscalls via muslcsys_install_syscall)     │
    ├─────────────────────────────────────────────────┤
    │           miniShell (PATH, env, CWD)             │
    ├──────────┬──────────┬──────────┬────────────────┤
    │  tty     │    fs    │   exec   │   pipe         │
    │ server   │  thread  │  thread  │   server       │
    │ (UART,   │  (VFS)   │ (ELF    │  (pipes,fork,  │
    │  line    │          │  loader) │   exec,wait)   │
    │  disc.)  │          │          │                │
    ├──────────┴──────────┴──────────┴────────────────┤
    │         VFS    │    procfs    │  auth server     │
    │    /  → ext2   │  /proc      │  SHA-3-512       │
    ├────────────────┴─────────────┴──────────────────┤
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

### fork()+exec()+waitpid() on seL4

AIOS implements the full Unix process creation pattern on bare seL4:

1. **fork()**: Reloads same ELF into new VSpace, copies .data (299 pages) + stack (16 pages), copies CSpace caps at same slot numbers, sets AArch64 registers (PC+4, x0=badge, x1=msginfo, x2=MR0=0)
2. **exec()**: Destroys old process image, loads new ELF, spawns with fresh sel4runtime, preserves PID/ppid/fault_ep
3. **waitpid()**: SaveCaller deferred reply pattern — parent blocks until child exits, receives exit code via zombie table

See `docs/FORK_IMPLEMENTATION.md` for the definitive technical guide.

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

    # Build sbase tools (93 Unix utilities)
    python3 scripts/build_sbase.py --clean --jobs 16

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

### sbase Unix Tools (93)

| Category | Commands |
|----------|----------|
| Files | cat, cp, mv, rm, ln, mkdir, rmdir, touch, chmod, chown, chgrp, xinstall |
| Text | head, tail, wc, sort, uniq, cut, paste, fold, expand, tr, rev, nl, comm, join, cols |
| Search | grep, find, strings |
| Info | ls, du, date, cal, uname, hostname, whoami, logname, tty, printenv, env |
| Crypto | md5sum, sha256sum, sha512sum, sha1sum, sha224sum, sha384sum, sha512-224sum, sha512-256sum, cksum |
| Stream | tee, sed, dd, split, seq |
| Encoding | uuencode, uudecode |
| Archive | tar |
| Math | bc, dc, expr |
| Editor | ed |
| Misc | echo, yes, true, false, basename, dirname, pwd, sleep, test, printf, mknod |
| System | kill, nice, renice, nohup, sync, which, xargs |

### AIOS-Specific Programs (20)

| Program | Purpose |
|---------|---------|
| fork_test | fork+exec+waitpid test |
| sysinfo | System information display |
| posix_test | POSIX compliance test suite (26 tests) |
| posix_ps | Process status (reads /proc/status) |
| posix_id | UID/GID display |
| posix_help | Command listing |
| posix_mkdir | mkdir via filesystem IPC |
| posix_touch | touch via filesystem IPC |
| posix_rm | rm via filesystem IPC |
| posix_nice | Nice value placeholder |
| aios_top | Process viewer (ANSI refresh) |
| daemon | Background daemon test |
| mini_shell | Interactive Unix shell |
| tty_server | TTY line discipline server |
| auth_server | SHA-3-512 authentication |
| serial_server | Legacy UART server |
| echo_server | Echo test server |
| hello_child | Hello world child process |
| crash_test | Fault recovery test |
| blk_test | Block device test |

## POSIX Compliance

26/26 tests pass. 55+ syscalls implemented including fork, execve, waitpid, getpid.

Run `posix_test` in the shell to verify.

## Design History

| Phase | Status |
|-------|--------|
| 0.1.x | Initial experiments — abandoned |
| 0.2.x | Monolithic design — hit scaling wall |
| 0.3.x | Microkit/PD-based — redesigned for seL4 idioms |
| 0.4.x | **Current** — sel4utils processes, POSIX shim, Unix tools, fork+exec |

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
| v0.4.37 | uname IPC + shell && / \|\| chains |
| v0.4.38 | Unix pipes + pipe server + UART buffer expansion |
| v0.4.39 | Quotes + redirection + multi-pipe |
| v0.4.40 | procfs enhancements + top + dmesg |
| v0.4.41 | MOTD, command history, top quit, PID 1 fix |
| v0.4.42 | $VAR expansion, Ctrl-C kills foreground, kill builtin |
| v0.4.43 | Background exec (&), jobs, rename/mv, 14 new sbase tools (93 total) |
| v0.4.44 | tty_server Phase 1: line discipline, cooked/raw mode |
| v0.4.45-46 | **fork()+waitpid()**: process duplication on seL4, exit codes, zombie reaping |
| v0.4.47 | **exec()**: process image replacement, fork+exec+waitpid complete |

## Roadmap

- [ ] exec argv passthrough (currently only path)
- [ ] Modularize aios_root.c (~2500 lines → 7 files)
- [ ] TTY Phase 2: getty/shell separation via fork+exec
- [ ] TTY Phase 3: multiple virtual terminals (Ctrl-A 1/2/3/4)
- [ ] More POSIX syscalls (symlink, chmod, dup2)
- [ ] Dynamic ELF buffer (>1MB programs)
- [ ] virtio-net + TCP/IP networking
- [ ] Copy-on-write fork (v0.5.x)
- [ ] Self-hosted development tools
- [ ] AI integration within the OS

## Documentation

- `docs/FORK_IMPLEMENTATION.md` — Definitive fork/waitpid/exec guide (15 learnings)
- `docs/AI_BRIEFING.md` — Complete technical briefing for AI assistants
- `docs/DESIGN_TTY.md` — TTY architecture (5 phases)
- `docs/SESSION_2026_04_05.md` — Session summary (fork+exec development)
- `docs/NEXT_20260405.md` — Next steps: modularization + TTY Phase 2
- `docs/LEARNINGS.md` — Hard-won debugging knowledge
- `docs/ARCHITECTURE.md` — System architecture
- `docs/DESIGN_0.4.md` — 0.4.x design decisions
- `docs/patches/` — Documented dependency patches

## License

MIT License. See LICENSE file.

Copyright (c) 2025-2026 AIOS Project
