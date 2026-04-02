# AIOS Architecture — v0.3.x

## Overview

AIOS (Open Aries) is a microkernel operating system built on seL4/Microkit for AArch64 (Cortex-A53, QEMU virt). It implements a POSIX-compatible user-space kernel inside a single seL4 Protection Domain (PD), with separate PDs for device drivers and system services.

```
┌─────────────────────────────────────────────────────────────────┐
│                        QEMU virt (AArch64)                      │
│  4 cores, 2GB RAM, virtio-blk, virtio-net, PL011 UART          │
├─────────────────────────────────────────────────────────────────┤
│                     seL4 Microkernel (14.0.0)                   │
│                     Microkit 2.1.0 (SMP)                        │
├─────────────────────────────────────────────────────────────────┤
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐          │
│  │ serial   │ │ blk      │ │ net      │ │ net      │          │
│  │ _driver  │ │ _driver  │ │ _driver  │ │ _server  │          │
│  │ PL011    │ │ virtio   │ │ virtio   │ │ IP stack │          │
│  │ pri=254  │ │ pri=250  │ │ pri=230  │ │ pri=210  │          │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘          │
│       │             │             │             │                │
│  ┌────┴─────────────┴─────────────┴─────────────┴──────────┐   │
│  │                    orchestrator (pri=200)                │   │
│  │  Service router · Timer · Preemption · Shutdown          │   │
│  │  Async IPC handler · Ctrl-C detection                    │   │
│  ├──────────────────────────────────────────────────────────┤   │
│  │  ┌──────────────────────────────────────────────────┐    │   │
│  │  │              sandbox (pri=150, child PD)         │    │   │
│  │  │  User-space kernel · Scheduler · Threads         │    │   │
│  │  │  Processes · Signals · POSIX libc                │    │   │
│  │  │  128 MB memory pool · Up to 256 processes        │    │   │
│  │  │  Up to 1024 threads · 16 FDs per process         │    │   │
│  │  └──────────────────────────────────────────────────┘    │   │
│  └──────────────────────────────────────────────────────────┘   │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐                       │
│  │ vfs      │ │ fs       │ │ auth     │                       │
│  │ _server  │ │ _server  │ │ _server  │                       │
│  │ pri=235  │ │ pri=240  │ │ pri=210  │                       │
│  └──────────┘ └──────────┘ └──────────┘                       │
└─────────────────────────────────────────────────────────────────┘
```

## Protection Domains

AIOS uses 9 seL4 Protection Domains, each isolated by the microkernel:

| PD | Priority | Role | Lines |
|----|----------|------|-------|
| serial_driver | 254 | PL011 UART RX/TX via ring buffers | 139 |
| blk_driver | 250 | virtio-blk disk I/O | 237 |
| fs_server | 240 | ext2/FAT16/FAT32 filesystem implementation | 1539 |
| vfs_server | 235 | Virtual filesystem layer, routes to fs_server | 129 |
| net_driver | 230 | virtio-net packet I/O | 286 |
| auth_server | 210 | Login authentication, /etc/passwd | 917 |
| net_server | 210 | TCP/IP stack, socket API | 603 |
| orchestrator | 200 | Service router, timer, preemption, async IPC | 576 |
| sandbox | 150 | User-space kernel: scheduler, threads, processes, POSIX | 1781 |

The sandbox is a child PD of the orchestrator, sharing memory regions for IPC.

## IPC Architecture

All inter-PD communication uses Microkit channels (seL4 endpoints):

```
sandbox ─── PPC CH_SANDBOX(7) ──→ orchestrator
                                      ├── PPC CH_VFS(5)  ──→ vfs_server ──→ PPC CH_FS(4) ──→ fs_server
                                      │                                         └── notify(10) ──→ blk_driver
                                      ├── PPC CH_AUTH(11) ──→ auth_server
                                      ├── PPC CH_NET_SRV(12) ──→ net_server ←── notify(8) ←── net_driver
                                      └── notify CH_SERIAL(1) ──→ serial_driver

Timer: IRQ30 → orchestrator notified() → notify(CH_SANDBOX) → sandbox notified() → preempt
```

Two IPC mechanisms are used:

- **PPC (Protected Procedure Call)**: Synchronous request/response. Caller blocks until callee returns. Used for all syscalls (sandbox → orchestrator → service PDs).
- **Notifications**: Asynchronous one-way signals. Used for timer interrupts, serial I/O, and Ctrl-C delivery.

## Sandbox Kernel

The sandbox PD contains a complete user-space kernel that manages processes and threads within a single 128 MB memory pool. It implements:

### Scheduler

- **Preemptive**: 10ms quantum via ARM generic timer (IRQ 30)
- **Priority-aware**: Highest-priority READY thread selected first, round-robin on tie
- **Context switch**: setjmp/longjmp with full FP/SIMD save (x19-x28, x29, x30, sp + q0-q31 + FPCR/FPSR = 768 bytes per thread)
- **Thread states**: FREE(0), READY(1), RUNNING(2), BLOCKED(3), FINISHED(4), BLOCKED_MTX(5), BLOCKED_COND(6), BLOCKED_IO(7)

### Processes

- Up to 256 concurrent processes (MAX_PROCS)
- Each process: code loaded from ext2 filesystem, heap (bump allocator), stack, 16 file descriptors
- Process states: FREE, ALIVE, ZOMBIE
- Per-process signal table: `sig_pending` bitmap + `sig_handlers[32]`
- Foreground/background flag for job control

### Threads (POSIX pthreads)

- Up to 1024 concurrent threads (MAX_THREADS)
- `pthread_create/join/detach/exit` with void* return values
- Block/wake mutex (not spin): contending threads sleep on wait_channel, unlock wakes highest-priority waiter
- Block/wake condition variables: wait blocks, signal wakes one, broadcast wakes all
- Block/wake read-write locks
- Thread-Local Storage: 64 keys per thread
- Thread-safe malloc: heap_lock with block/wake
- Automatic reaping: scheduler frees detached+finished threads and zombie processes

### Signal Infrastructure

- Per-process: `sig_pending` (32-bit bitmap) + `sig_handlers[32]`
- `deliver_signals()` called by scheduler before thread resume
- Default actions: SIGINT/SIGTERM/SIGKILL terminate, SIGCHLD ignore
- SIGKILL/SIGSTOP cannot be caught or ignored
- Ctrl-C chain: serial_driver → orchestrator peeks RX for 0x03 → sets CTRL_C_FLAG → sandbox delivers SIGINT to foreground process

### Memory Layout

```
0x20000000  sandbox_io    (4 KB)   Shared memory with orchestrator
0x20001000  sandbox_mem   (128 MB) Process memory pool
            ├── Process 0: code + heap + stack
            ├── Process 1: code + heap + stack
            └── ...
```

All processes share the same flat address space (no MMU-based isolation between processes).

## Filesystem

### Stack

```
Application → sandbox kernel (ks_open/ks_read/...) 
            → PPC to orchestrator
            → PPC to vfs_server (VFS layer)
            → PPC to fs_server (ext2/FAT implementation)
            → notify to blk_driver (disk I/O)
```

### ext2 Implementation

- Read/write support for files and directories
- Inode-based with block groups
- Directory entries with rec_len chaining
- File permissions (rwxr-xr-x) respected
- Custom tools: `mkext2.py` (create image), `ext2_inject.py` (populate image)
- Programs injected without `.bin` extension (POSIX-clean names)

## Shell

1566-line POSIX-compatible shell (`programs/shell.c`) with:

- **Command resolution**: POSIX PATH walker (`$PATH`, default `/bin:/sbin`). No extension appending. `./cmd` for current directory.
- **Builtins**: cd, pwd, ls, echo, cat, cp, head, wc, sort, env, export, ps, top, kill, jobs, fg, exit, help, clear, source, uname, date
- **Job control**: `cmd &` for background, `jobs` to list, `kill pid` to terminate, `fg` to foreground
- **I/O redirection**: `>` (write), `>>` (append), `<` (input), `|` (pipe)
- **Variable expansion**: `$VAR` and `$PATH` in commands
- **Script execution**: `source file.sh` or `sh file.sh`
- **Login**: username/password authentication via auth_server

## Syscalls

The sandbox kernel implements 63 syscall numbers organized by function:

| Range | Category | Examples |
|-------|----------|----------|
| 1-16 | File I/O | open, close, read, write, stat, mkdir, rmdir, getcwd, chdir |
| 17-19 | File ops | access, umask, dup |
| 20-29 | Process | exit, getpid, sleep, exec, getuid, getgid, getppid |
| 30-31 | Terminal | putc, getc |
| 40-46 | Sockets | socket, bind, listen, accept, connect, send, recv |
| 50-51 | Memory | brk, mmap |
| 60-76 | Extended | dup2, truncate, pipe, time, spawn, waitpid, kill, signal, fork |
| 80-82 | Internal | suspended, getpid_, getprocs |
| 90-93 | System | login, logout, shutdown, sync |

## Async IPC

Framework for non-blocking syscalls (scaffolding wired, used for getc):

```
sandbox_io shared memory layout:
  0x000 - 0x0FF: Control area (SBX_CMD, SBX_STATUS, etc.)
  0x0F0:         CTRL_C_FLAG (out-of-band Ctrl-C from serial)
  0x100 - 0x13F: Async request/response header
  0x200 - 0xFFF: Data area
```

The getc path uses yield-loop: shell thread stays READY between input checks, yielding timeslices so background threads run. The PPC to orchestrator doubles as a Microkit event loop return point for notification delivery.

## Build System

```
make              Build kernel image (build/loader.img)
make programs     Build all user programs (programs/*.bin + sbin + tests)
make ext2-disk    Create and populate ext2 disk image
make run          Boot in QEMU (4 cores, 2GB, virtio-blk/net)
make bump-patch   Increment version patch number
make clean        Remove build artifacts
```

Programs are cross-compiled with `aarch64-linux-gnu-gcc`, linked with a custom `link.ld`, and objcopy'd to flat binaries. Intermediates go in `build/prg/`.

## Test Suite

4 test programs, 42 tests total:

| Program | Tests | Coverage |
|---------|-------|----------|
| test_basic | 12 | syscalls, memory allocation, string operations |
| test_fileio | 8 | file create/read/write/stat, mkdir/rmdir |
| test_threads | 13 | pthread create/join, mutex contention, condvar, rwlock, TLS, preemption |
| test_signals | 9 | kill, signal handler registration, SIG_IGN, SIGKILL/SIGSTOP protection |

All 42 tests pass with concurrent background processes running.

## User Programs

34 programs in `/bin/`, 1 in `/sbin/`:

**Utilities**: cat, cp, echo, head, ls, mkdir, mv, rm, rmdir, sort, wc, whoami, uname
**System**: info, fstat, daemon, idle, netstat, wget, shutdown (sbin, root-only)
**Tests/Benchmarks**: bench, sieve, fib, stress, memtest, ftest, dirtest, forktest, spawn_test, posix_test, posixtest2, slot_test, socktest
**Shell**: Full POSIX-compatible shell with job control

## Known Limitations

1. **File I/O blocks all threads**: Sync PPC to orchestrator suspends entire sandbox PD. Window is microseconds per call.
2. **Single address space**: No memory protection between processes. All share 128 MB pool.
3. **Single seL4 TCB**: Sandbox is one seL4 schedulable entity. No true SMP parallelism.
4. **No fork**: `fork()` returns -1. Process duplication requires MMU-based address space copying.
5. **Ctrl-C via QEMU**: QEMU `-nographic` intercepts Ctrl-C before guest. Chain wired but untestable in current setup.
6. **Bump allocator**: malloc never frees. Long-running processes eventually exhaust heap.

## Version History (0.3.x)

| Version | Milestone |
|---------|-----------|
| 0.3.7 | Build tooling, PD sync, consistency checks |
| 0.3.8 | VFS server PD, channel cleanup, dead code archive |
| 0.3.9 | POSIX stubs (272/272 coverage) |
| 0.3.10 | Verified preemptive pthreads (35/35 tests) |
| 0.3.11 | Full FP/SIMD context save (q0-q31) |
| 0.3.12 | Priority scheduler, block/wake mutex/cond/rwlock |
| 0.3.13 | Shutdown command, POSIX PATH walker |
| 0.3.14 | Thread-safe malloc, thread reaper, async IPC framework |
| 0.3.15 | Shell parser fixes, script fallback probe |
| 0.3.16 | Signal infrastructure, split test suite (42/42) |
| 0.3.17 | Real background spawn, kill feedback, kernel zombie reaper |
| 0.3.18 | Async getc, Ctrl-C chain, concurrent background I/O |
