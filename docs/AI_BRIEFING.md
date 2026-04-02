# AIOS AI Briefing

This document gives an AI assistant the context needed to work on AIOS effectively. Read this first before any code changes.

## What Is AIOS

AIOS (Open Aries) is a microkernel OS built on seL4 15.0.0 for AArch64. It targets QEMU `virt` with a Cortex-A53, 4 cores, 2 GB RAM. The developer is Bryan, a senior banking executive with deep technical interests in OS development. The repo is at `github.com/arcanii/AIOS`, branch `v0.4.x`.

## Current State: v0.4.5

The system runs on bare seL4 — no Microkit, no CAmkES. A root task boots, spawns isolated processes, and manages threads. Key capabilities proven:

- **4-core SMP** with per-thread core affinity
- **Hardware memory isolation** (each process has its own VSpace)
- **Real kernel threads** (each thread is a seL4 TCB)
- **IPC via endpoints** (serial server, echo server)
- **Fault containment** (NULL deref caught, system survives)
- **Interactive shell** with UART keyboard input
- **5/5 test suite** passes every boot

## Repository Structure

```
AIOS/
├── src/aios_root.c         # Root task (342 lines) — THE main file
├── src/apps/               # Child processes (each gets own VSpace)
│   ├── hello_child.c       # Test: spawn + exit
│   ├── echo_server.c       # Test: IPC echo (val+1)
│   ├── serial_server.c     # Service: PUTC/GETC/PUTS/KEY_PUSH via IPC
│   ├── mini_shell.c        # Interactive shell (reads keyboard via serial)
│   └── crash_test.c        # Test: deliberate NULL deref
├── projects/aios/CMakeLists.txt  # Build config for all apps
├── CMakeLists.txt          # Top-level (includes seL4 cmake-tool)
├── settings.cmake          # Kernel config (platform, SMP, hypervisor)
├── deps/                   # seL4 ecosystem (gitignored, cloned separately)
│   ├── kernel -> seL4      # seL4 15.0.0 source
│   ├── seL4_libs/          # sel4utils, vka, vspace, allocman, etc.
│   ├── sel4runtime/        # C runtime for seL4 processes
│   ├── seL4_tools/         # cmake-tool, elfloader
│   ├── util_libs/          # libplatsupport, libcpio, libelf, etc.
│   └── musllibc/           # C library (patched for GCC 15)
├── docs/                   # Architecture, design, patches
└── ref/v03x/               # Archived 0.3.x codebase (Microkit era)
```

## How It Boots

1. `elfloader` loads seL4 kernel + root task ELF, boots 4 cores via PSCI/SMC
2. seL4 kernel starts, gives root task all capabilities via `BootInfo`
3. `aios_root` bootstraps: `allocman` → `vka` → `vspace`
4. Maps PL011 UART device frame for keyboard input
5. Runs 5 tests: process spawn, IPC echo, threading+SMP, isolation, crash survival
6. Spawns `serial_server` (IPC-based I/O) + `mini_shell` (interactive)
7. Enters keyboard polling loop: reads UART RX, forwards via IPC to serial_server

## How To Build

```bash
cd ~/Desktop/github_repos/AIOS
rm -rf build-04 && mkdir build-04 && cd build-04
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
ninja
```

Output: `build-04/images/aios_root-image-arm-qemu-arm-virt`

## How To Run

```bash
qemu-system-aarch64 -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G -nographic -serial mon:stdio \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

Exit: `Ctrl-A X`

## Key seL4 APIs Used

| API | Purpose |
|-----|---------|
| `sel4utils_configure_process_custom()` | Create process (VSpace + CSpace + TCB) |
| `sel4utils_spawn_process_v()` | Start process with argv |
| `sel4utils_copy_cap_to_process()` | Grant endpoint cap to child |
| `sel4utils_destroy_process()` | Reclaim all resources |
| `sel4utils_configure_thread()` | Create thread in existing VSpace |
| `sel4utils_start_thread()` | Start thread at entry point |
| `seL4_TCB_SetPriority()` | Set thread priority (0-255) |
| `seL4_TCB_SetAffinity()` | Pin thread to specific core |
| `seL4_Call()` / `seL4_Recv()` / `seL4_Reply()` | Synchronous IPC |
| `seL4_Recv(fault_ep)` | Catch child faults/exits |
| `sel4platsupport_alloc_frame_at()` | Get device frame cap |
| `vspace_map_pages()` | Map frame into VSpace |

## Key Design Patterns

### Adding a New Child Process

1. Create `src/apps/myapp.c` with standard `main(int argc, char *argv[])`
2. Add `AiosChildApp(myapp)` to `projects/aios/CMakeLists.txt`
3. Add `$<TARGET_FILE:myapp>` to the `MakeCPIO` list
4. In `aios_root.c`, spawn with `spawn_with_args("myapp", prio, ...)` or `spawn_simple("myapp", prio, ...)`

### Passing Capabilities to Children

```c
seL4_CPtr child_slot = sel4utils_copy_cap_to_process(&proc, &vka, ep_cap);
// child_slot is the CSpace slot number in the child's CSpace
// Pass as argv string so child knows which slot to use
```

### IPC Protocol Convention

Use message label to identify the operation, MR0..MRn for data:

```c
// Client
seL4_SetMR(0, data);
seL4_Call(ep, seL4_MessageInfo_new(LABEL, 0, 0, num_mrs));

// Server
seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
seL4_Word label = seL4_MessageInfo_get_label(msg);
seL4_Word data = seL4_GetMR(0);
```

### Priority Discipline

All interactive processes run at priority 200 (round-robin timeslicing). seL4's classic scheduler only yields to equal-priority threads via `seL4_Yield()`. Higher-priority threads preempt lower ones. The root task UART poller must be at equal priority to shell/serial_server or the system deadlocks.

## Known Issues / Gotchas

- **GCC 15 + musllibc**: Must patch `deps/musllibc/src/internal/vis.h` and `stdio_impl.h` to change `protected` visibility to `default`. See `docs/patches/musl-gcc15.md`.
- **SMP requires virtualization=on**: QEMU's PSCI uses HVC without it, which elfloader rejects. With `virtualization=on`, PSCI uses SMC and works. Kernel must have `KernelArmHypervisorSupport=ON`.
- **MCS scheduler broken**: `sel4utils_configure_process` fails to set up scheduling contexts on MCS config. Use `KernelIsMCS=OFF`.
- **seL4_Yield only yields to equal priority**: Not to lower. All cooperating processes must be at the same priority for round-robin.
- **Process cleanup required**: `sel4utils_destroy_process()` must be called after a child exits or untyped memory is exhausted after ~5 spawns.
- **macOS cpio**: The system `cpio` doesn't support `--append`. Must install GNU cpio: `brew install cpio`.
- **seL4_DebugPutChar for TX**: Direct PL011 TX writes conflict with kernel debug output. Use `seL4_DebugPutChar` instead.

## What Comes Next

1. **Filesystem**: Port virtio-blk driver + ext2 from 0.3.x as service processes
2. **Process niceness**: Runtime priority adjustment
3. **Full shell**: Port 34 commands from 0.3.x, load programs from disk
4. **Signal delivery**: Via fault handler mechanism
5. **MCS scheduler**: Debug sched context cap copy issue
6. **Networking**: virtio-net + TCP/IP stack

## 0.3.x Reference

The `ref/v03x/` directory contains the complete 0.3.x codebase which used Microkit. Key files to reference when porting:

- `ref/v03x/src/sandbox.c` — user-space kernel (1781 lines, process/thread management)
- `ref/v03x/src/fs/ext2.c` — ext2 filesystem (1145 lines)
- `ref/v03x/src/fs/vfs.c` — VFS dispatch (394 lines)
- `ref/v03x/programs/shell.c` — full POSIX shell (1566 lines)
- `ref/v03x/src/blk_driver.c` — virtio-blk driver (237 lines)
- `ref/v03x/src/serial_driver.c` — PL011 UART driver (139 lines)

These carry over to 0.4.x with IPC adapter changes — the core logic is reusable.

## Developer Workflow

Bryan works on macOS (Apple Silicon M3 Max). Standard workflow:

1. Edit source in project directory
2. `cd build-04 && ninja` to rebuild (incremental, fast)
3. Boot in QEMU, test
4. Commit via GitHub Desktop
5. Scripts are typically generated in `/tmp/` and run from there

When generating patches or scripts, write them as heredocs or Python scripts to `/tmp/` for Bryan to run. Avoid shell comments in heredocs (macOS zsh interprets `#` differently in some contexts).
