# AIOS 0.4.x Architecture

## Overview

AIOS 0.4.x runs directly on the seL4 15.0.0 microkernel without Microkit or CAmkES. A single root task (`aios_root`) bootstraps the system, spawns isolated processes, manages threads, and provides service dispatch. Every user process gets hardware-enforced memory isolation via separate seL4 VSpaces.

## Kernel Configuration

| Parameter | Value |
|-----------|-------|
| Kernel | seL4 15.0.0 |
| Platform | qemu-arm-virt |
| Architecture | AArch64 (ARMv8-A) |
| CPU | Cortex-A53 |
| Cores | 4 (SMP) |
| Scheduler | Classic (non-MCS), round-robin at equal priority |
| Hypervisor | EL2 mode (KernelArmHypervisorSupport=ON) |
| Memory | 2 GB (505 MB available after kernel reservations) |
| Debug | KernelDebugBuild=ON, KernelPrinting=ON |

EL2/hypervisor mode is required because QEMU's PSCI implementation uses SMC (not HVC) to boot secondary cores. The elfloader rejects HVC-based PSCI.

## System Architecture

```
┌─────────────────────────────────────────────────────────┐
│              seL4 15.0.0 (4 cores, EL2)                 │
│                                                         │
│  Provides: TCBs, VSpaces, CSpaces, Endpoints,          │
│            Notifications, IRQ handlers, Untypeds        │
│            Priority scheduling, SMP affinity            │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  aios_root (root task, priority 200)                    │
│  ┌────────────────────────────────────────────────────┐ │
│  │ 1. Receive BootInfo from kernel                    │ │
│  │ 2. Bootstrap allocman → vka → vspace              │ │
│  │ 3. Map UART device frame (PL011 at 0x9000000)     │ │
│  │ 4. Run test suite (5 tests)                       │ │
│  │ 5. Spawn serial_server + mini_shell               │ │
│  │ 6. Poll UART RX, forward keys via IPC             │ │
│  └────────────────────────────────────────────────────┘ │
│       │                    │                            │
│  ┌────┴────────┐   ┌──────┴──────┐                     │
│  │serial_server│   │ mini_shell  │                     │
│  │ VSpace B    │   │ VSpace C    │                     │
│  │ CSpace B    │   │ CSpace C    │                     │
│  │ TCB (p200)  │   │ TCB (p200)  │                     │
│  │             │   │             │                     │
│  │ EP: serial  │   │ EP: serial  │                     │
│  │ PUTC/GETC   │   │ calls ser   │                     │
│  │ KEY_PUSH    │   │ for all I/O │                     │
│  └─────────────┘   └─────────────┘                     │
└─────────────────────────────────────────────────────────┘
```

## Process Model

Each process is created via `sel4utils_configure_process_custom()` and receives:

- **VSpace**: Own page table root. MMU enforces isolation — accessing another process's memory causes a VM fault delivered to root task via fault endpoint.
- **CSpace**: Own capability space (4096 slots, 12-bit). Contains only capabilities explicitly granted by root.
- **TCB**: Own kernel thread control block. Priority set via `seL4_TCB_SetPriority()`. Core affinity via `seL4_TCB_SetAffinity()`.
- **Fault endpoint**: Kernel delivers faults (VM fault, cap fault) as IPC messages to root task.

### Process Lifecycle

```
sel4utils_configure_process_custom()  → allocate VSpace + CSpace + TCB
sel4utils_copy_cap_to_process()       → grant endpoint capabilities
sel4utils_spawn_process_v()           → set entry point, start TCB
   ... process runs ...
seL4_Recv(fault_ep)                   → root catches exit/fault
sel4utils_destroy_process()           → reclaim all resources
```

### spawn_with_args Helper

Root task uses a helper function to spawn processes with endpoint capabilities passed via argv:

```c
int spawn_with_args(name, priority, proc, fault_ep, ep_count, eps, child_slots)
```

This configures the process, copies endpoint caps into its CSpace, converts slot numbers to strings, and passes them as argv to the child's main().

## Thread Model

Threads are created with `sel4utils_configure_thread()` within an existing VSpace. Each thread gets its own seL4 TCB but shares the VSpace and CSpace of its parent.

```c
sel4utils_configure_thread(&vka, &vspace, &vspace, fault_ep, cspace, cspace_data, &thread);
seL4_TCB_SetPriority(thread.tcb.cptr, auth_tcb, priority);
seL4_TCB_SetAffinity(thread.tcb.cptr, core_id);
sel4utils_start_thread(&thread, entry_fn, arg0, arg1, 1);
```

Threads sharing a VSpace can access the same memory (verified: 4 workers increment shared counter to 4). seL4's kernel scheduler handles preemption, priority, and SMP core assignment.

### SMP Verification

Thread affinity test pins each of 4 worker threads to a different core. Interleaved UART output proves true parallel execution — characters from different threads appear mixed in the output stream, which only happens when threads execute simultaneously on separate physical cores.

## IPC Model

All inter-process communication uses seL4 endpoints:

```
Client                          Server
  │                               │
  ├─ seL4_SetMR(0, data)         │
  ├─ seL4_Call(ep, msg) ──────►  seL4_Recv(ep, &badge)
  │       (client blocks)         ├─ process message
  │                               ├─ seL4_SetMR(0, reply_data)
  ◄────────────────────────────  seL4_Reply(reply_msg)
  ├─ seL4_GetMR(0)               │
```

### Serial Server IPC Protocol

| Label | Name | Request | Reply |
|-------|------|---------|-------|
| 1 | PUTC | MR0 = char | (empty) |
| 2 | GETC | (empty) | MR0 = char or -1 |
| 3 | PUTS | MR0..MRn = chars | (empty) |
| 4 | KEY_PUSH | MR0 = char (from root) | (empty) |

Output path: shell → PUTC IPC → serial_server → seL4_DebugPutChar → UART TX.
Input path: UART RX → root polls PL011 → KEY_PUSH IPC → serial_server buffer → shell GETC.

## Device Access

### UART (PL011)

Physical address `0x09000000`, IRQ 33. The root task maps the device frame into its own VSpace using `sel4platsupport_alloc_frame_at()` + `vspace_map_pages()`. It polls the RX register for keyboard input and forwards characters via IPC.

Serial output uses `seL4_DebugPutChar()` to avoid contention with kernel's UART usage.

## Memory Management

### Bootstrap Sequence

```
seL4_BootInfo
  → bootstrap_use_current_simple(&simple, pool_size, pool)
    → allocman (untyped memory allocator)
      → allocman_make_vka(&vka, allocman)
        → VKA interface (kernel object allocation)
          → sel4utils_bootstrap_vspace_with_bootinfo_leaky()
            → VSpace manager (virtual memory)
```

Static bootstrap pool: 400 pages (1.6 MB). allocman then manages all remaining untypeds (~505 MB).

### Resource Cleanup

`sel4utils_destroy_process()` reclaims VSpace, CSpace, TCB, and all allocated untypeds. Essential — without cleanup the system exhausts untyped memory after ~5 process spawns.

## Build System

seL4's standard CMake + Ninja build:

```
CMakeLists.txt      → includes settings.cmake, then tools/seL4/cmake-tool/all.cmake
settings.cmake      → platform, arch, SMP, hypervisor, debug flags
projects/aios/      → AiosChildApp macro, MakeCPIO, DeclareRootserver
```

All child ELFs are embedded into a CPIO archive linked into the root task. The elfloader packages kernel + root task into the final boot image.

### Key Build Commands

```bash
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
ninja
```

Output: `build-04/images/aios_root-image-arm-qemu-arm-virt`

### Boot Command

```bash
qemu-system-aarch64 -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G -nographic -serial mon:stdio \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

## New Subsystems (v0.4.30-35)

### Thread Server (v0.4.30)
Handles THREAD_CREATE/THREAD_JOIN IPC. Creates threads in child VSpaces via manual TCB setup: allocates TCB + fault EP + IPC buffer + stack, copies fault EP into child CSpace, configures via seL4_TCB_Configure, sets registers via WriteRegisters. Mutex via userspace spinlock + seL4_Yield.

### Kernel Log (v0.4.31)
16KB ring buffer in root VSpace. AIOS_LOG_INFO/WARN/ERROR/DEBUG macros with HH:MM:SS timestamps. Serial echo for real-time output. /proc/log for dmesg-style access. Per-module LOG_MODULE tagging.

### Auth Server (v0.4.32-35)
SHA-3-512 (Keccak) password hashing. 16-user database loaded from /etc/passwd at boot. Session-based access control (4 concurrent sessions). IPC commands: LOGIN, LOGOUT, WHOAMI, CHECK_FILE, CHECK_KILL, CHECK_PRIV, USERADD, PASSWD, SU, GROUPS, USERMOD, GET_USER.

### File Permission Enforcement (v0.4.35)
fs_ep minted per-process with badge = active_procs index. fs_thread extracts badge, looks up uid. Non-root denied write/mkdir/unlink on /etc/ and /bin/. Extensible to full Unix permission model.

### Login Flow (v0.4.33-35)
mini_shell presents "AIOS login:" prompt. Password masked with asterisks. 3 attempts before reset. Successful login sets session uid/gid/token. su/exit maintain 4-deep identity stack.

### Pipe Server (v0.4.38)
Root-side thread managing 8 concurrent pipes with 8KB ring buffers. IPC labels: PIPE_CREATE, PIPE_WRITE, PIPE_READ, PIPE_CLOSE, PIPE_KILL. pipe_ep passed to all children via argv. Shell orchestrates pipe chains: creates pipes, spawns left/right with stdout/stdin redirected. Also handles process kill requests (PIPE_KILL) since exec_thread blocks on child exit.

### Shell Features (v0.4.37-43)
Line editor with left/right cursor. Command history (16 entries, up/down arrows). $VAR expansion. Quote stripping. && / || chains. Multi-pipe (up to 8 stages). > / < redirection via pipe capture + FS_WRITE_FILE. Background execution (&) with jobs listing. kill builtin. Ctrl-C kills foreground process. dmesg builtin.

### Process Control (v0.4.42-43)
Ctrl-C: UART poll intercepts 0x03, destroys foreground process, unblocks exec_thread via fault EP message. kill: IPC to pipe_server which calls process_kill(). Background exec: EXEC_RUN_BG replies to shell immediately without waiting for child exit.

## Known Limitations

- Non-MCS scheduler (MCS sched context setup fails — deferred)
- UART TX via seL4_DebugPutChar (avoids kernel contention)
- UART RX polling (no interrupt-driven input)
- No fork() (spawn only)
- Process priority is static (no runtime nice/renice)
- Single QEMU platform (no real hardware tested)
- Auth server isolated in own VSpace (v0.4.36)
- No background reaping (zombie processes from exited bg jobs)
- No job control (fg/bg/suspend)
- Pipe runs sequentially (left finishes before right starts)
- pthread_join blocks thread_server (one join at a time)
- Serial output interleaves with background process output

## Comparison with 0.3.x

| Feature | 0.3.x (Microkit) | 0.4.x (bare seL4) |
|---------|-------------------|---------------------|
| Process isolation | None (shared space) | MMU-enforced VSpaces |
| Threads | User-space setjmp/longjmp | Real seL4 TCBs |
| SMP | Single TCB | 4 cores, per-thread affinity |
| Scheduling | User-space round-robin | Kernel preemptive |
| Crash containment | Kills sandbox | VM fault caught, system lives |
| IPC | Microkit PPC (blocks PD) | seL4 endpoints (per-thread) |
| Shell | Full POSIX (34 commands) | Mini (5 commands) |
| Filesystem | ext2 read/write | Not yet ported |
| Test suite | 42/42 | 5/5 (architecture tests) |
