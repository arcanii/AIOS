# AIOS AI Briefing

This document gives an AI assistant the context needed to work on AIOS effectively. Read this first before any code changes.

## What Is AIOS

AIOS (Open Aries) is a microkernel OS built on seL4 15.0.0 for AArch64. It targets QEMU `virt` with Cortex-A53, 4 cores, 2 GB RAM. The developer is Bryan. The repo is at `github.com/arcanii/AIOS`, main branch.

## Current State: v0.4.9

The system runs on bare seL4 — no Microkit, no CAmkES. A root task boots, spawns isolated processes, and manages threads. Key capabilities:

- 4-core SMP with per-thread core affinity
- Hardware memory isolation (each process has its own VSpace)
- Real kernel threads (each thread is a seL4 TCB)
- IPC via endpoints (serial server, fs thread, exec thread)
- Fault containment (NULL deref caught, system survives, shell continues)
- ext2 filesystem on virtio-blk (ls, cat, cd, pwd, wc, head)
- Exec from shell — type a program name, it spawns in a new VSpace
- Native programs — sysinfo, hello_child, crash_test, echo_server
- Clean boot — banner, fs mount, prompt in ~1 second

## Repository Structure

```
AIOS/
├── src/
│   ├── aios_root.c         # Root task — boot, fs init, exec thread, keyboard loop
│   ├── ext2.c              # ext2 filesystem reader (raw byte reads)
│   ├── ext2.h              # ext2 types and API
│   └── apps/               # Child processes (each gets own VSpace)
│       ├── hello_child.c   # Test: spawn + exit
│       ├── echo_server.c   # Test: IPC echo (val+1)
│       ├── serial_server.c # Service: PUTC/GETC/PUTS/KEY_PUSH via IPC
│       ├── mini_shell.c    # Interactive shell with fs commands
│       ├── crash_test.c    # Test: deliberate NULL deref
│       ├── blk_test.c      # Test: virtio-blk from child process
│       └── sysinfo.c       # Native program: prints system info
├── include/
│   ├── virtio.h            # virtio MMIO definitions (legacy v1)
│   └── aios/
│       ├── version.h       # Semantic versioning
│       └── build_number.h  # Auto-incremented build counter
├── projects/aios/
│   └── CMakeLists.txt      # Build: child apps + CPIO + root task
├── scripts/
│   ├── bump-build.sh       # Increment build number (auto on compile)
│   ├── bump-patch.sh       # Increment patch version
│   ├── bump-minor.sh       # Increment minor version, reset patch
│   └── version.sh          # Print current version
├── disk/
│   └── disk_ext2.img       # 128MB ext2 filesystem image
├── deps/                   # seL4 ecosystem (gitignored)
├── docs/
│   ├── ARCHITECTURE.md     # Technical architecture
│   ├── AI_BRIEFING.md      # This file
│   ├── DESIGN_0.4.md       # Original design document
│   ├── LEARNINGS.md        # Hard-won debugging knowledge
│   └── patches/musl-gcc15.md
├── ref/v03x/               # Archived 0.3.x codebase
├── CMakeLists.txt          # Top-level (includes seL4 cmake-tool)
├── settings.cmake          # Kernel config
└── LICENSE                 # MIT
```

## How To Build

```bash
cd ~/Desktop/github_repos/AIOS
rm -rf build-04 && mkdir build-04 && cd build-04
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
ninja
```

CPIO is cached — if you change any child app, do a full rebuild (rm -rf build-04).

## How To Run

```bash
qemu-system-aarch64 -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G -nographic -serial mon:stdio \
    -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

Exit: `Ctrl-A X`

## Architecture: Threads and IPC

```
Root task (priority 200)
├── Main thread: keyboard polling (UART RX → KEY_PUSH IPC)
├── fs_thread: serves FS_LS(10), FS_CAT(11) from shell
├── exec_thread: serves EXEC_RUN(20), spawns child, waits, replies
│
├── serial_server (VSpace B) ← IPC → shell, root
├── mini_shell (VSpace C) ← IPC → serial, fs, exec
└── spawned programs (new VSpaces) ← exec_thread creates on demand
```

### Reply Cap Pattern (exec_thread)
```c
seL4_Recv(exec_ep, &badge);
seL4_CNode_Delete(..., reply_slot, seL4_WordBits);
seL4_CNode_SaveCaller(..., reply_slot, seL4_WordBits);
// ... spawn child, Recv on child_fault_ep ...
seL4_Send(reply_slot, reply_msg);
```

## Adding a New Program

1. Create `src/apps/myapp.c` with `int main(int argc, char *argv[])`
2. Add `AiosChildApp(myapp)` to `projects/aios/CMakeLists.txt`
3. Add `$<TARGET_FILE:myapp>` to MakeCPIO list
4. Full rebuild, type `myapp` at shell prompt

## Key Gotchas

- All processes/threads at priority 200 or deadlock
- Call `sel4utils_destroy_process()` after exit or OOM
- Reply caps destroyed by `seL4_Recv()` — use SaveCaller
- SaveCaller slot must be empty — Delete before each SaveCaller
- ninja doesn't rebuild CPIO — full rebuild for child app changes
- ext2: raw byte reads only, no packed structs on AArch64
- DMA: virtio used ring at page boundary (offset 0x1000)
- `tools/seL4/cmake-tool` symlink must exist
- GCC 15: patch musllibc visibility (see docs/patches/)

See `docs/LEARNINGS.md` for detailed debugging knowledge.

## What Comes Next

1. ext2 write support (mkdir, write, rm)
2. ELF loader from disk (load programs from ext2 instead of CPIO)
3. Port 0.3.x programs (recompile for 0.4.x IPC)
4. Process niceness / priority management
5. Networking (virtio-net + TCP/IP)

## Developer Workflow

Bryan works on macOS Apple Silicon M3 Max. Workflow:
1. Edit source in project directory
2. `cd build-04 && ninja` (or full rebuild if child apps changed)
3. Boot in QEMU with disk image, test
4. Commit via GitHub Desktop
5. Scripts generated to `/tmp/` as Python (avoid zsh heredoc issues)
