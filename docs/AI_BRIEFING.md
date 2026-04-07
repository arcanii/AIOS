# AIOS AI Briefing Document

## Project Overview

**AIOS (Open Aries)** is a microkernel operating system built on seL4, targeting AArch64/QEMU.

- **Repository**: https://github.com/arcanii/AIOS
- **Branch**: main
- **Developer**: Bryan
- **Current Version**: v0.4.55

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
- Incremental ninja sufficient for aios_root.c or posix_*.c changes
- Rebuild sbase after aios_posix.c changes (`python3 scripts/build_sbase.py --clean --jobs 16`)

### Commit Protocol

- Commit at each milestone with version tag in message. Each patch should contain 5 or more new features.
- Format: "v0.4.XX: short description\n\ndetails"
- git push origin main after each commit

### No Hacks Protocol

- Pure POSIX — no alias tables, no prefix stripping, no magic.
- Strict Unix philosophy to do things the right way.
- Do work that would make Richard Stallman proud.
- Shell searches $PATH, sends full path to exec_thread
- exec_thread loads exactly what it's given
- mkdisk installs files with original names
- Temporary workarounds must be documented and removed when proper solution available

### Memory Budget Awareness

- The rootserver has a fixed memory budget. Adding static buffers (even 512KB)
  can push it past limits, causing `ext2 init failed: -1` at boot.
- The file-scope `elf_buf[1024 * 1024]` is shared between exec_thread, fork,
  and exec. NEVER add another large static buffer.
- If the rootserver image grows past ~608ac (varies), ext2 init fails silently.
- Always check rootserver size in boot output if ext2 init fails.

## Build & Boot Commands

### Full Rebuild

    cd ~/Desktop/github_repos/AIOS
    rm -rf build-04 && mkdir build-04 && cd build-04
    cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
        -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
    ninja

### Rebuild sbase (parallel builder)

    python3 scripts/build_sbase.py [--clean] [--jobs N]

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
    │   ├── aios_root.c          # Root task: boot, drivers, servers, fork/exec (~2500 lines)
    │   ├── aios_auth.c          # Auth server: SHA-3-512, user DB, sessions, permissions
    │   ├── aios_log.c           # Kernel log: 16KB ring buffer + serial echo
    │   ├── ext2.c               # ext2 filesystem (read + write + indirect blocks)
    │   ├── vfs.c                # Virtual filesystem switch (mount dispatch)
    │   ├── procfs.c             # /proc: version, uptime, status, mounts, log, meminfo
    │   ├── lib/
    │   │   ├── aios_posix.c     # POSIX shim (55+ syscalls + fork + exec + waitpid)
    │   │   └── aios_posix.h     # POSIX shim header + IPC labels
    │   └── apps/                # AIOS programs (fork_test, mini_shell, tty_server, etc.)
    ├── include/aios/
    │   ├── version.h, build_number.h, ext2.h, vfs.h, procfs.h
    │   ├── aios_auth.h          # Auth protocol: IPC labels, user/session types
    │   ├── aios_log.h           # Log macros: AIOS_LOG_INFO/WARN/ERROR/DEBUG
    │   └── tty.h                # TTY IPC labels
    ├── scripts/
    │   ├── mkdisk.py            # Disk image CLI
    │   ├── ext2/builder.py      # ext2 image builder (multi-group, indirect blocks)
    │   ├── ext2_dump.py         # ext2 disk image inspector
    │   ├── aios-cc              # Cross-compiler wrapper
    │   ├── build_sbase.py       # Parallel sbase builder (16 jobs, progress bar)
    │   └── bump-patch.sh, bump-minor.sh, bump-build.sh, version.sh
    ├── disk/
    │   ├── disk_ext2.img        # 128MB ext2 disk image (gitignored)
    │   └── rootfs/              # Filesystem content overlay
    ├── docs/
    │   ├── AI_BRIEFING.md       # This file
    │   ├── FORK_IMPLEMENTATION.md # Definitive fork/waitpid/exec guide
    │   ├── DESIGN_TTY.md        # TTY architecture (5 phases)
    │   ├── SESSION_2026_04_05.md # Session summary
    │   ├── NEXT_20260405.md     # Next steps: modularize + TTY Phase 2
    │   ├── ARCHITECTURE.md, DESIGN_0.4.md, LEARNINGS.md
    │   └── patches/             # Documented dep patches
    ├── projects/aios/CMakeLists.txt  # Build config
    ├── deps/                    # gitignored: seL4, musllibc, etc.
    ├── build-04/                # gitignored: build output
    └── settings.cmake           # seL4 kernel config

## Architecture

### Boot Sequence

1. ELF-loader loads kernel + root task
2. Root task initializes: allocator, SMP, virtio-blk, ext2, VFS
3. VFS mounts: / (ext2) and /proc (procfs)
4. Auth init: load /etc/passwd (SHA-3-512)
5. Starts fs_thread + exec_thread + thread_server + pipe_server (root VSpace threads)
6. Spawns tty_server + auth_server + mini_shell (isolated processes from CPIO)
7. mini_shell presents login prompt, authenticates via auth IPC

### Process Model

- **tty_server**: TTY line discipline, UART I/O, cooked/raw mode, Ctrl-C/U/W/D
- **fs_thread**: VFS dispatch (LS, CAT, STAT, MKDIR, WRITE, UNLINK, UNAME, RENAME)
- **exec_thread**: Reads ELF from /bin/, loads into new VSpace, manages lifecycle
- **pipe_server**: Unix pipes, fork(), exec(), waitpid(), kill(), getpid()
- **thread_server**: pthread create/join via TCB management
- **auth_server**: SHA-3-512 auth, user DB, sessions, file permission checks
- **mini_shell**: PATH search, CWD, env vars, pipes, redirection, exec launcher
- **User programs**: isolated VSpaces, POSIX syscalls via IPC

### fork()+exec()+waitpid() Architecture

See `docs/FORK_IMPLEMENTATION.md` for the complete technical guide. Key points:

- **fork()** routed through pipe_server (PIPE_FORK=65). Badge identifies caller.
- **Approach**: Reload same ELF into new VSpace, copy .data (299 pages) + stack (16 pages)
- **AArch64 ABI**: x0=badge, x1=msginfo, x2=MR0 (NOT x0=msginfo!)
- **Off-by-one fix**: Page count must use (end - aligned_base), not (memsz / PAGE_SIZE)
- **waitpid()**: SaveCaller pattern — parent blocks until reap_check detects child exit
- **exec()**: PIPE_EXEC destroys old process, loads new ELF, preserves PID/fault_ep
- **Exit codes**: sel4runtime exit callback sends PIPE_EXIT before faulting

### IPC Protocol Labels

    SER_PUTC=1, SER_GETC=2, SER_PUTS=3, KEY_PUSH=4
    FS_LS=10, FS_CAT=11, FS_STAT=12, FS_MKDIR=14
    FS_WRITE_FILE=15, FS_UNLINK=16, FS_UNAME=17, FS_RENAME=18
    EXEC_RUN=20, EXEC_NICE=21, EXEC_RUN_BG=24
    THREAD_CREATE=30, THREAD_JOIN=31
    AUTH_LOGIN=40 .. AUTH_LOAD_PASSWD=52
    PIPE_CREATE=60, PIPE_WRITE=61, PIPE_READ=62, PIPE_CLOSE=63
    PIPE_KILL=64, PIPE_FORK=65, PIPE_GETPID=66
    PIPE_WAIT=67, PIPE_EXIT=68, PIPE_EXEC=69
    TTY_WRITE=70, TTY_READ=71, TTY_IOCTL=72, TTY_INPUT=75

### Implemented Syscalls (55+)

File I/O: open, openat (O_CREAT), read, readv, write, writev, close, lseek
Directories: getdents64, chdir, mkdirat, unlinkat
Stat: fstat, fstatat
Identity: getpid, getppid, getuid, geteuid, getgid, getegid
System: uname (kernel IPC), getcwd, ioctl, fcntl, dup, dup3
Access: access, faccessat, umask, utimensat
Time: clock_gettime, gettimeofday, nanosleep (ARM generic timer)
Process: fork (clone), execve, wait4, exit, exit_group
Memory: mmap, munmap, brk, madvise (from muslcsys)

## External Tools

### sbase (suckless base utilities)

- Source: ~/Desktop/github_repos/sbase
- 93 tools cross-compiled via build_sbase.py
- bc requires pre-generated bc.c via bison: `/opt/homebrew/opt/bison/bin/bison -o bc.c bc.y`
- 5 sbase tools NOT compiled (need fork/sockets): cron, flock, setsid, time, tftp

## Known Issues / Gotchas

- **GCC 15 + musllibc**: Patch vis.h and stdio_impl.h (protected->default visibility)
- **GNU sed on macOS**: PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
- **CPIO caching**: ninja doesn't detect child ELF changes, need full rebuild
- **ELF buffer**: Static 1MB shared between exec_thread, fork, and exec. Cannot add more.
- **Memory budget**: Adding static buffers to rootserver can cause ext2 init failure
- **DMA**: Must use single untyped Retype for contiguous pages
- **Priority**: All processes at 200 (different = deadlock)
- **ext2**: Never use packed structs on AArch64 (use rd16/rd32)
- **SMP fork timing**: Child can exit before parent calls waitpid (zombie table handles this)
- **_exit() bypass**: musl's _exit() skips sel4runtime callback, losing exit code
- **pipe_ep**: Currently non-static (was changed for fork_test extern access)
- **seL4 SMP**: Kernel compiled for 4 cores, cannot boot with fewer (crashes)
- **aios_root.c**: ~2500 lines, needs modularization (see NEXT_20260405.md)

## Key Learnings (fork/exec)

See docs/FORK_IMPLEMENTATION.md for 15 detailed learnings. Critical ones:

1. **AArch64 seL4 ABI**: x0=badge, x1=msginfo, x2=MR0 (NOT x0=msginfo)
2. **Off-by-one pages**: (end - aligned_base), not (memsz / PAGE_SIZE)
3. **No dynamic allocation**: TLS=16KB static, heap=1MB static, both in .data
4. **Frame caps need CNode_Copy**: Can't map parent's cap into root's VSpace directly
5. **Child needs unique badge**: pipe_ep must be re-minted for correct IPC routing
6. **Spin-reap pattern**: After PIPE_EXIT, spin NBRecv on fault EP before blocking

## Pending Items

1. **exec argv passthrough** — currently only path, not user arguments
2. **Modularize aios_root.c** — split into 7 files (see NEXT_20260405.md)
3. **TTY Phase 2** — getty/shell separation via fork+exec
4. **TTY Phase 3** — multiple virtual terminals (Ctrl-A switching)
5. **_exit() fix** — intercept at musl syscall level
6. **COW fork** — defer .data page copy until write fault (v0.5.x)

## Version History (0.4.x)

| Version | Feature |
|---------|---------|
| v0.4.5 | SMP verified (4 cores) |
| v0.4.6 | virtio-blk driver |
| v0.4.7-8 | ext2 filesystem + navigation |
| v0.4.9 | Exec from shell |
| v0.4.10-11 | POSIX shim (printf, open/read/write/close) |
| v0.4.12-14 | POSIX programs + Unix-like shell |
| v0.4.15-16 | VFS + procfs + process lifecycle |
| v0.4.17-18 | 34 syscalls + getdents64 |
| v0.4.19 | ext2 write support |
| v0.4.20-21 | Auto-init + 30 programs + miniShell |
| v0.4.22-24 | ELF-from-disk + PATH search + env vars |
| v0.4.25 | aios-cc cross-compiler + kernel uname |
| v0.4.26-27 | 37 sbase tools + __wrap_main |
| v0.4.28 | Crash recovery (exit via VM fault) |
| v0.4.29 | CWD propagation + 79 sbase tools + path resolution |
| v0.4.30 | pthreads: create, join, mutex (manual TCB in child VSpaces) |
| v0.4.31 | AIOS_LOG: 16KB ring buffer + serial echo |
| v0.4.32 | Auth server: SHA-3-512, user DB, sessions |
| v0.4.33-34 | Login, uid/gid propagation, getpwuid |
| v0.4.35-36 | su/passwd, file permissions, auth isolation |
| v0.4.37-39 | uname, pipes, quotes, redirection |
| v0.4.40-42 | procfs, top, dmesg, $VAR, Ctrl-C, kill |
| v0.4.56 | Background exec, jobs, 14 new sbase (93 total) |
| v0.4.44 | tty_server Phase 1: line discipline |
| v0.4.45-46 | **fork() + waitpid()** — process duplication on seL4 |
| v0.4.47 | **exec()** — fork+exec+waitpid complete |
| v0.4.57 | pthread in child processes, thread retval, cap revoke, 79/79 POSIX |
