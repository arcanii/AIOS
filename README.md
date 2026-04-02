# Open Aries (AIOS)

A microkernel operating system built on [seL4](https://sel4.systems/), targeting AArch64 with true process isolation, kernel-scheduled threads, and SMP parallelism across 4 cores.

```
  ___                      _         _
 / _ \ _ __   ___ _ __    / \   _ __(_) ___  ___
| | | | '_ \ / _ \ '_ \  / _ \ | '__| |/ _ \/ __|
| |_| | |_) |  __/ | | |/ ___ \| |  | |  __/\__ \
 \___/| .__/ \___|_| |_/_/   \_\_|  |_|\___||___/
      |_|
```

## Status

**v0.4.5** — Experimental / Research

Built directly on bare seL4 15.0.0 (no Microkit). Every process gets its own hardware-isolated address space, capability set, and kernel-scheduled threads.

### What Works

- 4-core SMP (AArch64, QEMU virt, cortex-a53)
- Hardware memory isolation (MMU-enforced VSpaces per process)
- Kernel-scheduled threads (real seL4 TCBs with priority + affinity)
- IPC-based service architecture (endpoints, badged caps)
- Process lifecycle (spawn, run, fault/exit, cleanup, respawn)
- Fault containment (NULL deref caught as VM fault, system survives)
- Interactive shell with UART keyboard input
- 5/5 built-in test suite

### Test Suite

| Test | Description | Status |
|------|-------------|--------|
| Process spawn | Child in own VSpace + TCB | PASS |
| IPC echo | 5 round-trips, val+1 verified | PASS |
| Multi-threading + SMP | 4 TCBs pinned to 4 cores, shared counter | PASS |
| Process isolation | NULL deref caught as VM fault at 0x0 | PASS |
| Crash survival | New process spawns after crash | PASS |

This is a research project exploring AI-assisted OS development. External AI (Claude) generates and reviews code.

## Quick Start

### Prerequisites

- macOS (Apple Silicon) or Linux (x86-64)
- `aarch64-linux-gnu-gcc` (GCC 15 tested)
- `qemu-system-aarch64` (v7+)
- `cmake` (3.16+), `ninja`
- Python 3.9+ with: `pyyaml pyfdt jinja2 ply lxml pyelftools`
- GNU `cpio` (macOS: `brew install cpio`)

### Install Dependencies (macOS)

```bash
brew install aarch64-linux-gnu-gcc qemu cmake ninja cpio
pip3 install pyyaml pyfdt jinja2 ply lxml pyelftools --break-system-packages
```

### Clone and Set Up

```bash
git clone https://github.com/arcanii/AIOS.git
cd AIOS
git checkout v0.4.x

# Clone seL4 ecosystem into deps/
mkdir -p deps && cd deps
git clone https://github.com/seL4/seL4.git kernel
git clone https://github.com/seL4/seL4_libs.git
git clone https://github.com/seL4/sel4runtime.git
git clone https://github.com/seL4/seL4_tools.git
git clone https://github.com/seL4/util_libs.git
git clone https://github.com/seL4/musllibc.git
cd ..

# Create required symlinks
ln -s deps/kernel kernel
mkdir -p tools/seL4 projects
ln -s ../../deps/seL4_tools/cmake-tool tools/seL4/cmake-tool
ln -s ../deps/seL4_libs projects/seL4_libs
ln -s ../deps/sel4runtime projects/sel4runtime
ln -s ../deps/seL4_tools projects/seL4_tools
ln -s ../deps/util_libs projects/util_libs
ln -s ../deps/musllibc projects/musllibc

# Apply GCC 15 musl patch (see docs/patches/musl-gcc15.md)
sed -i '' 's/visibility push(protected)/visibility push(default)/' deps/musllibc/src/internal/vis.h
sed -i '' 's/visibility("protected")/visibility("default")/' deps/musllibc/src/internal/stdio_impl.h
```

### Build

```bash
mkdir build-04 && cd build-04
cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- \
    ..
ninja
```

### Run

```bash
qemu-system-aarch64 \
    -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 \
    -m 2G \
    -nographic \
    -serial mon:stdio \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

Exit QEMU: `Ctrl-A X`

### Expected Output

```
Core 1 is up with logic id 1
Core 2 is up with logic id 2
Core 3 is up with logic id 3

--- Test 1: Process spawn ---
[test1] PASS: child spawned and exited

--- Test 2: IPC echo ---
[test2] PASS: 5 IPC round-trips correct

--- Test 3: Multi-threading + SMP (4 TCBs, 4 cores) ---
[test3] PASS: 4 threads, counter=4
[test3] Cores used: 0 1 2 3

--- Test 4: Process isolation ---
[test4] Caught VM fault at address 0x0
[test4] PASS: crash contained! System still alive.

--- Test 5: Crash doesn't kill other processes ---
[test5] PASS: new process ran after crash

  Test Results: 5/5 passed

$ hello
Hello from AIOS 0.4.x!
```

## Architecture

```
seL4 15.0.0 kernel (AArch64, 4 cores, EL2 hypervisor)
│
└── aios_root (root task)
    ├── UART poller (PL011 at 0x9000000)
    ├── Process server (spawn/cleanup/fault handling)
    ├── Thread manager (TCBs with priority + core affinity)
    │
    ├── serial_server (VSpace B) ── IPC ── seL4_DebugPutChar
    ├── mini_shell (VSpace C) ── IPC ── serial_server
    ├── echo_server (VSpace D) ── IPC ── root task
    ├── hello_child (VSpace E) ── runs, exits
    └── crash_test (VSpace F) ── NULL deref → VM fault → contained
```

Each process has its own VSpace (page table), CSpace (capabilities), and TCB (kernel thread). A crash in one process cannot affect any other.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for full technical details.

## Project Structure

```
AIOS/
├── src/
│   ├── aios_root.c           # Root task: boot, process server, tests, shell
│   └── apps/                  # Child processes (each gets own VSpace)
│       ├── hello_child.c      # Simple test process
│       ├── echo_server.c      # IPC echo service
│       ├── serial_server.c    # Serial I/O via IPC
│       ├── mini_shell.c       # Interactive shell
│       └── crash_test.c       # Deliberate fault (isolation test)
├── projects/aios/
│   └── CMakeLists.txt         # Build: child apps + CPIO + root task
├── deps/                      # seL4 ecosystem (gitignored)
│   ├── kernel -> seL4
│   ├── seL4_libs/
│   ├── sel4runtime/
│   ├── seL4_tools/
│   ├── util_libs/
│   └── musllibc/
├── docs/
│   ├── ARCHITECTURE.md        # Technical architecture
│   ├── AI_BRIEFING.md         # Context doc for AI assistants
│   ├── DESIGN_0.4.md          # Original design document
│   └── patches/musl-gcc15.md  # GCC 15 musllibc patch
├── ref/v03x/                  # Archived 0.3.x codebase
├── CMakeLists.txt             # Top-level (includes seL4 build)
├── settings.cmake             # Platform config
└── LICENSE                    # MIT
```

## Version History

| Version | Milestone |
|---------|-----------|
| 0.3.x | Microkit-based, user-space kernel in sandbox PD, 42/42 tests |
| 0.4.0 | Bare seL4 root task boots |
| 0.4.1 | First isolated child process (own VSpace + TCB) |
| 0.4.2 | IPC endpoints, serial server, mini shell |
| 0.4.3 | Real multi-threading (4 seL4 TCBs) |
| 0.4.4 | 5/5 test suite, crash isolation verified |
| 0.4.5 | SMP — 4 cores, thread affinity, hypervisor mode |

## Contributing

Experimental/research phase. Collaborators welcome.

MIT License — see [LICENSE](LICENSE)
