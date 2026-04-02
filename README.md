# Open Aries (AIOS)

A microkernel operating system built on [seL4](https://sel4.systems/) / [Microkit](https://github.com/seL4/microkit), targeting AArch64. Features preemptive multitasking, POSIX-compatible threading, an ext2 filesystem, and a Unix-like shell — all running on the world's most formally verified microkernel.

```
  ___                      _         _
 / _ \ _ __   ___ _ __    / \   _ __(_) ___  ___
| | | | '_ \ / _ \ '_ \  / _ \ | '__| |/ _ \/ __|
| |_| | |_) |  __/ | | |/ ___ \| |  | |  __/\__ \
 \___/| .__/ \___|_| |_/_/   \_\_|  |_|\___||___/
      |_|
```

## Status

**v0.3.18** — Experimental / Research

- 9 seL4 Protection Domains (drivers, services, sandbox kernel)
- Preemptive scheduler with 10ms quantum, priority-aware
- POSIX pthreads: create/join/mutex/condvar/rwlock/TLS
- ext2 filesystem (read/write)
- 34 user programs, POSIX shell with job control
- 42/42 test suite passing
- Background process concurrency verified

This is a research project exploring AI-assisted OS development. External AI (Claude) generates and reviews code. The long-term goal is self-hosted development within AIOS itself.

## Quick Start

### Prerequisites

- **macOS** (Apple Silicon or Intel) or **Linux** (x86-64)
- `aarch64-linux-gnu-gcc` cross-compiler
- `qemu-system-aarch64` (v7+)
- Python 3.9+
- seL4 Microkit SDK 2.1.0

### Install Dependencies

**macOS (Homebrew):**
```bash
brew install aarch64-linux-gnu-gcc qemu python3
```

**Ubuntu/Debian:**
```bash
sudo apt install gcc-aarch64-linux-gnu qemu-system-arm python3
```

### Get the Microkit SDK

Download from [Microkit releases](https://github.com/seL4/microkit/releases) and extract:

```bash
mkdir -p ~/microkit
cd ~/microkit
# macOS Apple Silicon:
wget https://github.com/seL4/microkit/releases/download/2.1.0/microkit-sdk-2.1.0-macos-aarch64.tar.gz
tar xf microkit-sdk-2.1.0-macos-aarch64.tar.gz
```

Or set `MICROKIT_SDK` if installed elsewhere:
```bash
export MICROKIT_SDK=/path/to/microkit-sdk-2.1.0
```

### Build and Run

```bash
git clone https://github.com/arcanii/AIOS.git
cd AIOS
make                  # Build kernel image
make programs         # Build user programs
make ext2-disk        # Create and populate disk image
make run              # Boot in QEMU
```

### First Boot

```
login: root
password: root

$ echo $PATH
/bin:/sbin
$ ls
bin/   sbin/   etc/   home/   tmp/   dev/   var/   hello.txt
$ hello
Hello from AIOS!
$ daemon &
[1] 2
$ ps
  PID  PPID  UID  SLOT  STATE  FG  NAME
     1     0    0     0  RUN    fg  /bin/shell
     2     1    0     0  READY  bg  /bin/daemon
$ kill 2
[2] killed
$ shutdown
The system is going down for halt NOW!
System halted. It is safe to power off.
```

Exit QEMU: `Ctrl-A X`

### Run Tests

```
$ cd /bin/tests
$ ./test_basic
$ ./test_fileio
$ ./test_threads
$ ./test_signals
```

All 42 tests should pass (12 + 8 + 13 + 9).

## Architecture

9 seL4 Protection Domains, each isolated by the microkernel:

```
serial_driver (PL011 UART)         blk_driver (virtio-blk)
      │                                  │
      └──── orchestrator ────────────────┘
             │    │    │
        vfs_server  auth_server  net_server ── net_driver
             │
        fs_server (ext2)
             │
         ┌───┴───┐
         │sandbox │  ← user-space kernel
         │  shell │     processes, threads
         │  apps  │     scheduler, signals
         └───────┘
```

The **sandbox** PD contains a complete user-space kernel managing up to 256 processes and 1024 threads within 128 MB of memory. It implements preemptive scheduling, POSIX pthreads, signals, and a POSIX-compatible shell.

See [ARCHITECTURE.md](ARCHITECTURE.md) for full technical details.

## Build Targets

| Target | Description |
|--------|-------------|
| `make` | Build kernel image (`build/loader.img`) |
| `make programs` | Build all user programs (bin + sbin + tests) |
| `make ext2-disk` | Create ext2 disk image with programs and config |
| `make run` | Boot in QEMU (4 cores, 2GB RAM, virtio devices) |
| `make debug` | Boot with GDB server (`-S -s`) |
| `make clean` | Remove build artifacts |
| `make bump-patch` | Increment version patch number |
| `make version` | Print current version |

## Project Structure

```
AIOS/
├── src/                    # Kernel PD source code
│   ├── sandbox.c           # User-space kernel (1781 lines)
│   ├── orchestrator.c      # Service router, timer, IPC
│   ├── serial_driver.c     # PL011 UART driver
│   ├── blk_driver.c        # virtio-blk driver
│   ├── net_driver.c        # virtio-net driver
│   ├── net_server.c        # TCP/IP stack
│   ├── auth_server.c       # Login authentication
│   ├── vfs_server.c        # Virtual filesystem layer
│   ├── fs/                 # Filesystem implementations
│   │   ├── vfs.c           # VFS dispatch
│   │   ├── ext2.c          # ext2 (primary)
│   │   ├── fat16.c         # FAT16 (legacy)
│   │   └── fat32.c         # FAT32 (legacy)
│   └── arch/aarch64/       # Architecture-specific
│       ├── setjmp.S        # Context save (int + FP/SIMD)
│       └── context.h       # Context layout
├── include/
│   ├── aios/               # Kernel headers
│   │   ├── aios.h          # Syscall table + macros
│   │   ├── channels.h      # PD channel IDs
│   │   ├── async_ipc.h     # Async IPC protocol
│   │   └── version.h       # Version numbers
│   └── posix.h             # POSIX wrappers for user programs
├── libc/                   # Minimal C library
│   ├── include/            # signal.h, pthread.h, etc.
│   └── src/                # Stubs for POSIX functions
├── programs/               # User programs
│   ├── shell.c             # POSIX shell (1566 lines)
│   ├── *.c                 # 34 utilities (cat, ls, cp, etc.)
│   ├── sbin/               # Privileged programs
│   │   └── shutdown.c      # Root-only system shutdown
│   └── tests/              # Test suite
│       ├── test_basic.c    # Syscalls, memory, strings (12 tests)
│       ├── test_fileio.c   # File I/O, directories (8 tests)
│       ├── test_threads.c  # Pthreads, scheduling (13 tests)
│       └── test_signals.c  # Kill, signal handlers (9 tests)
├── tools/                  # Build tools
│   ├── mkext2.py           # Create ext2 disk image
│   ├── ext2_inject.py      # Populate disk with programs
│   ├── posix_audit.py      # POSIX compliance checker
│   └── gen_system_json.py  # System description generator
├── disk/                   # Files injected onto disk
│   ├── etc/                # hostname, motd, passwd, services
│   └── hello.txt
├── aios.system             # Microkit system description (PDs, channels, memory)
├── Makefile                # Build system
└── ARCHITECTURE.md         # Full technical documentation
```

## Shell Commands

**Builtins:** cd, pwd, ls, echo, cat, cp, head, wc, sort, env, export, ps, top, kill, jobs, fg, exit, help, clear, source, uname, date

**External programs:** bench, daemon, dirtest, fib, forktest, fstat, ftest, hello, idle, info, memtest, mkdir, mv, netstat, posix_test, rm, rmdir, sieve, socktest, sort, spawn_test, stress, uname, wc, wget, whoami

**Shell features:** `$PATH` resolution, `./cmd`, `cmd &` (background), `cmd > file`, `cmd >> file`, `cmd < file`, `cmd1 | cmd2`, `$VAR` expansion, script execution

## Key Design Decisions

**Why seL4?** Formally verified microkernel with mathematical proof of correctness. All AIOS services run in isolated PDs — a driver crash cannot take down the kernel.

**Why a user-space kernel?** The sandbox PD implements process/thread management in user space rather than using seL4 TCBs directly. This allows rapid iteration on scheduling and threading without modifying seL4, at the cost of true hardware isolation between user processes.

**Why POSIX?** Familiar programming model. Programs written for AIOS look like standard Unix programs. The 272-function POSIX surface enables porting existing software.

## Known Limitations

- File I/O blocks all threads briefly (sync PPC to orchestrator, microseconds per call)
- Single address space for all user processes (no inter-process memory protection)
- Single seL4 TCB for sandbox (no true SMP parallelism for user threads)
- `fork()` returns -1 (no process duplication)
- Bump allocator for malloc (no free, long-running processes exhaust heap)
- Ctrl-C chain wired but QEMU `-nographic` intercepts it before the guest

## Contributing

This project is in an experimental/research phase. Collaborators welcome.

- Issues and PRs on [GitHub](https://github.com/arcanii/AIOS)
- MIT License

## Acknowledgments

Built on [seL4](https://sel4.systems/) by Trustworthy Systems (UNSW) and the [seL4 Foundation](https://sel4.systems/). seL4 won the 2023 ACM Software System Award.

## License

MIT — see [LICENSE](LICENSE)
