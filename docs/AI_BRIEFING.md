# AIOS AI Briefing Document

## Project Overview

**AIOS (Open Aries)** is a microkernel operating system built on seL4, targeting AArch64/QEMU.

* **Repository**: https://github.com/arcanii/AIOS
* **Branch**: main
* **Developer**: Bryan
* **Current Version**: v0.4.65

## Development Environment

* **Host**: macOS Apple Silicon M3 Max
* **Cross-compiler**: aarch64-linux-gnu-gcc (Homebrew, GCC 15)
* **Build system**: CMake + Ninja
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
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

### Cross-Compile External Programs

```
./scripts/aios-cc source.c -o output
```

### Build dash (cross-compile)

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
    $DASH/alias.c $DASH/mail.c $DASH/histedit.c $DASH/signames.c \
    $DASH/bltin/printf.c $DASH/bltin/test.c $DASH/bltin/times.c \
    -I $DASH -I $DASH/bltin -DSHELL -include $DASH/config.h \
    -o build-04/sbase/dash
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
|   +-- aios_auth.c          # Auth server: SHA-3-512, user DB, sessions, permissions
|   +-- aios_log.c           # Kernel log: 16KB ring buffer + serial echo
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
|   |   +-- boot_fs_init.c   # Filesystem init (virtio-blk + ext2 + VFS mounts)
|   |   +-- boot_services.c  # Server threads + process spawning
|   |   +-- blk_io.c         # Block I/O (virtio-blk read/write)
|   |   +-- spawn_util.c     # spawn_with_args, spawn_simple
|   +-- servers/
|   |   +-- exec_server.c    # ELF loader, process spawn, foreground wait
|   |   +-- pipe_server.c    # Pipes, fork, exec, wait, exit, signals (central hub)
|   |   +-- fs_server.c      # VFS dispatch (LS, CAT, STAT, MKDIR, WRITE, UNLINK)
|   |   +-- thread_server.c  # Thread create/join for child processes
|   +-- process/
|   |   +-- fork.c           # do_fork() implementation
|   |   +-- reap.c           # reap_forked_child, zombie management
|   +-- apps/                # AIOS programs + test_threads
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
|   +-- disk_ext2.img        # 128MB ext2 disk image (gitignored)
|   +-- rootfs/              # Filesystem content overlay (/etc, /tmp, /root)
+-- docs/
|   +-- AI_BRIEFING.md       # This file
|   +-- ARCHITECTURE.md, DESIGN_0.4.md, LEARNINGS.md
|   +-- DASH_PORT.md         # dash porting guide and syscall audit
|   +-- patches/             # Documented dep patches (musl-gcc15, platsupport)
+-- projects/aios/CMakeLists.txt  # Build config
+-- deps/                    # gitignored: seL4, musllibc, etc.
+-- build-04/                # gitignored: build output
|   +-- sbase/               # Cross-compiled sbase tools + dash
|   +-- projects/aios/       # AIOS programs
+-- settings.cmake           # seL4 kernel config
```

## Architecture

### Boot Sequence

1. ELF-loader loads kernel + root task
2. Root task initializes: allocator (4000 pages), SMP, virtio-blk, ext2, VFS
3. VFS mounts: / (ext2) and /proc (procfs)
4. Auth init: load /etc/passwd (SHA-3-512), auto-login root
5. Starts fs_thread + exec_thread + thread_server + pipe_server
6. Spawns tty_server + auth_server (CPIO, isolated processes)
7. Spawns getty via exec_thread (fork+exec capable VSpace)
8. Getty presents login prompt, authenticates via auth IPC, spawns mini_shell

### Process Model

* tty_server: UART I/O via IPC (PUTC/GETC), runs in isolated VSpace
* fs_thread: VFS dispatch (LS, CAT, STAT, MKDIR, WRITE, UNLINK, UNAME)
* exec_thread: Reads ELF from /bin/, loads into new VSpace, manages lifecycle
* pipe_server: Central hub for pipes, fork, exec, wait, exit, signals
* thread_server: Creates child threads within existing processes
* mini_shell: PATH search, CWD, environment variables, exec launcher
* User programs: isolated VSpaces, POSIX syscalls via IPC

### ELF Loading (from disk)

1. Shell searches $PATH, sends full path to exec_thread
2. exec_thread reads ELF via VFS (ext2 with indirect blocks)
3. elf_newFile() parses from 1MB static buffer (TODO: dynamic)
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

### Pipe Architecture (v0.4.65)

Pipes use shared-memory frames allocated via vka_alloc_frame and mapped into
the root task VSpace. Each pipe_t has a shm_buf pointer (backed by a 4K mapped
frame) used as the ring buffer. Data still flows through IPC message registers
for signaling, but the buffer storage is a proper mapped frame rather than
static BSS memory. Cleanup unmaps and frees the frame when all refs are dropped.

### IPC Protocol Labels

```
SER_PUTC=1, SER_GETC=2, SER_KEY_PUSH=4
FS_LS=10, FS_CAT=11, FS_STAT=12
FS_MKDIR=14, FS_WRITE_FILE=15, FS_UNLINK=16, FS_UNAME=17, FS_RENAME=18
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
```

### Implemented Syscalls (70+)

File I/O: open, openat (O_CREAT, O_APPEND), read, readv, write, writev, close, lseek, pread64, pwrite64, ftruncate
Directories: getdents64, chdir, mkdirat, unlinkat
Stat: fstat, fstatat, statx, fchmod, fchmodat, fchown, fchownat
Links: linkat (ENOSYS), symlinkat (ENOSYS), readlinkat (EINVAL)
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

### fd Table

* AIOS_FD_BASE=10, AIOS_MAX_FDS=32
* aios_fd_t: active, is_dir, is_pipe, pipe_id, pipe_read, is_devnull, is_append, path[128], data[4096], size, pos
* fds 0-2 handled specially (stdin/stdout/stderr via serial or pipe redirect)
* /dev/null: is_devnull=1, write returns count, read returns 0

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

### dash (Debian Almquist Shell) -- NEW in v0.4.64

* Source: ~/Desktop/github_repos/dash (cloned from github.com/tklauser/dash)
* 468KB statically linked AArch64 ELF
* Generated headers: token.h, syntax.h, nodes.h/c, builtins.h/c, init.c, signames.c, config.h
* Config: JOBS=0, SMALL=1, GLOB_BROKEN=1, _GNU_SOURCE
* Builtins.def: stripped C license comment block (mkbuiltins is not a C preprocessor), JOBS/SMALL conditionals removed
* Cross-compiled with: aios-cc -I $DASH -DSHELL -include $DASH/config.h
* Status: LINKED, first boot test shows binary executes (dash -c with no arg prints usage)
* stdout not yet visible for dash -c "echo hello" -- likely mini_shell quote parsing issue

### Programs on Disk

* 99 sbase Unix tools in /bin/
* 28 AIOS programs in /bin/aios/
* dash in /bin/dash
* Total: 128 programs

## Known Issues / Gotchas

* GCC 15 + musllibc: Patch vis.h and stdio_impl.h (protected->default visibility)
* platsupport Warning: Patched common.c (docs/patches/)
* GNU sed on macOS: PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
* texinfo: PATH="/opt/homebrew/opt/texinfo/bin:$PATH"
* CPIO caching: ninja does not detect child ELF changes, need full rebuild
* ELF buffer: Static 1MB (TODO: dynamic for v0.5.x)
* DMA: Must use single untyped Retype for contiguous pages
* Priority: All processes at 200 (different = deadlock)
* ext2: Never use packed structs on AArch64 (use rd16/rd32)
* aios-cc: Uses tr "/" "_" for object names (avoids cp.c vs libutil/cp.c collision)
* Process exit: Overridden to trigger VM fault (not seL4_DebugHalt)
* Multi-group ext2: Block allocation scans all groups; inode allocation group 0 only
* mini_shell: Does not handle single-quote args (dash -c 'echo hi' broken)
* dash builtins.def: Must strip C-style comments before mkbuiltins (it is a shell script not C)
* Disk image is gitignored: must rebuild after rm -rf build-04 (sbase binaries lost)
* O_APPEND: implemented at shell level (read-modify-write via FS_CAT+FS_WRITE_FILE), not at VFS level. ext2 vfs_create replaces entire file content.

## Pending Items

1. Dash: debug stdout for -c mode, test script file execution, interactive mode
2. Shell: mini_shell quote handling for dash -c invocations
3. Networking: virtio-net + TCP/IP
4. Dynamic ELF buffer: vka_alloc_frame for >1MB binaries
5. Process niceness: runtime seL4_TCB_SetPriority
6. ext2 write improvements: multi-block file write, triple indirect
7. Allocator right-sizing: data shows 4000 pages is ~100x oversized, reduce to ~500
8. VFS-level append: add FS_APPEND IPC op to avoid read-modify-write overhead
9. Shared-memory pipes phase 2: map frame into writer+reader VSpaces for zero-copy I/O

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

## Architecture After v0.4.65

```
src/aios_root.c           ~200 lines
src/lib/aios_posix.c      ~577 lines
src/lib/posix_internal.h   ~275 lines (+ is_append field)
src/lib/posix_file.c       ~592 lines (+ O_APPEND)
src/lib/posix_stat.c       ~318 lines
src/lib/posix_dir.c        ~188 lines
src/lib/posix_proc.c       ~325 lines
src/lib/posix_time.c       ~121 lines
src/lib/posix_misc.c       ~230 lines
src/lib/posix_thread.c     ~195 lines
src/lib/vka_audit.c        ~65 lines (NEW)
src/vfs.c                  ~170 lines
src/procfs.c               ~286 lines
src/servers/exec_server.c  ~380 lines
src/servers/pipe_server.c  ~900 lines (+ shm pipes, audit)
src/servers/fs_server.c    ~12K lines
src/servers/thread_server.c ~200 lines
src/boot/boot_services.c   ~117 lines
src/process/fork.c         ~495 lines
src/process/reap.c         ~130 lines
src/apps/mini_shell.c      ~1380 lines (+ O_APPEND redirect)
include/aios/vka_audit.h   ~38 lines (NEW)
```

## Test Results

```
posix_verify V3: 98/98 PASS
signal_test: 20/20 PASS
POSIX audit: 55/55 core (100%), 26/26 extended, 81/81 total (100%)
O_APPEND: echo hello > f; echo world >> f; cat f -> hello\nworld (PASS)
fork_test: PASS
pipe: echo hello | cat -> hello (PASS)
VKA audit: boot 7ep+4pg, fork cslots, pipe frames (PASS)
```
