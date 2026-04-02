# AIOS 0.4.x Architecture Design

## Executive Summary

AIOS 0.4.x replaces Microkit with a custom process server built directly on seL4's raw capability API. This gives each user process its own seL4 TCB (kernel-scheduled thread), VSpace (hardware-isolated address space), and CSpace (capability set). The result: true preemptive multitasking with MMU-enforced process isolation, real SMP parallelism, and async IPC — while keeping seL4's verified kernel untouched.

## Why: What 0.3.x Cannot Do

| Limitation | Root Cause | 0.4.x Solution |
|------------|-----------|-----------------|
| All user processes share one address space | Microkit: one PD = one VSpace | Each process gets its own VSpace |
| All threads are one seL4 TCB | Microkit: one PD = one TCB | Each thread gets its own TCB |
| No true SMP parallelism | Single TCB can only run on one core | TCBs scheduled across all cores |
| File I/O blocks all threads | Sync PPC blocks entire PD | Async endpoints + per-thread blocking |
| No fork() | Can't duplicate address space | VSpace copy-on-write (future) |
| Zombie reaping is user-space hack | No real process lifecycle | Kernel-level wait/exit semantics |

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                   seL4 15.0.0 Kernel                    │
│         (unchanged, formally verified AArch64)          │
├─────────────────────────────────────────────────────────┤
│                                                         │
│   aios_root (root task, PID 0)                         │
│   ┌─────────────────────────────────────────────────┐  │
│   │ Boot: receive BootInfo, init allocman/vka/vspace │  │
│   │ Create all server processes + endpoints          │  │
│   │ Process server: spawn/wait/exit/kill             │  │
│   │ Capability allocator: TCBs, VSpaces, endpoints   │  │
│   └─────────────────────────────────────────────────┘  │
│        │          │          │          │               │
│   ┌────┴───┐ ┌────┴───┐ ┌───┴────┐ ┌──┴─────┐        │
│   │ serial │ │  blk   │ │  net   │ │  net   │        │
│   │ driver │ │ driver │ │ driver │ │ server │        │
│   │ (TCB)  │ │ (TCB)  │ │ (TCB)  │ │ (TCB)  │        │
│   └────────┘ └────────┘ └────────┘ └────────┘        │
│        │          │                                     │
│   ┌────┴──────────┴──────────────────────────────┐     │
│   │           Service Layer                       │     │
│   │  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────────┐ │     │
│   │  │ VFS  │  │  FS  │  │ Auth │  │ ProcServ │ │     │
│   │  │(TCB) │  │(TCB) │  │(TCB) │  │  (TCB)   │ │     │
│   │  └──────┘  └──────┘  └──────┘  └──────────┘ │     │
│   └──────────────────────────────────────────────┘     │
│                       │                                 │
│   ┌───────────────────┴──────────────────────────┐     │
│   │           User Processes                      │     │
│   │  ┌──────────┐  ┌──────────┐  ┌──────────┐   │     │
│   │  │  shell   │  │  daemon  │  │  test_*   │   │     │
│   │  │ VSpace A │  │ VSpace B │  │ VSpace C  │   │     │
│   │  │ TCB 1    │  │ TCB 2    │  │ TCB 3,4   │   │     │
│   │  │ CSpace A │  │ CSpace B │  │ CSpace C  │   │     │
│   │  └──────────┘  └──────────┘  └──────────┘   │     │
│   └──────────────────────────────────────────────┘     │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## Key Differences from 0.3.x

### Process Model

**0.3.x:** All processes live in one 128MB flat memory pool inside one PD. The sandbox kernel manages a `proc_t` table and does setjmp/longjmp context switches. No hardware isolation.

**0.4.x:** Each process gets:
- Its own **seL4 VSpace** (page table root) — MMU-enforced isolation
- Its own **seL4 CSpace** (capability node) — can only access granted resources
- One or more **seL4 TCBs** — kernel-scheduled, truly preemptible, SMP-capable
- A **scheduling context** (MCS) — CPU time budget per thread
- Capabilities to **IPC endpoints** for syscalls

A buggy or malicious process cannot access another process's memory. The MMU will fault, seL4 will deliver the fault to the process server, which can kill the faulting process.

### Thread Model

**0.3.x:** User-space setjmp/longjmp scheduler. One seL4 TCB does round-robin across up to 1024 software threads. Timer interrupt triggers notification → context switch.

**0.4.x:** Each POSIX thread maps to a real seL4 TCB.
- `pthread_create()` → `seL4_Untyped_Retype(seL4_TCBObject)` + `seL4_TCB_Configure()`
- Thread priority → `seL4_TCB_SetPriority()`
- Preemption is done by seL4's kernel scheduler (no timer notification chain needed)
- SMP: threads with separate TCBs can run on different cores simultaneously
- Mutex/condvar can use seL4 Notifications as underlying primitive

### IPC Model

**0.3.x:** All syscalls go through one Microkit PPC channel (CH_SANDBOX → orchestrator). Blocks entire PD.

**0.4.x:** Each process gets an endpoint capability to the relevant server:
- File ops → VFS endpoint
- Process ops → ProcServ endpoint  
- Terminal I/O → Serial endpoint

IPC is per-thread: when thread A calls the VFS server, only thread A blocks. Threads B, C, D continue running on other cores.

```
0.3.x:  Thread A ──PPC──> orchestrator ──PPC──> vfs ──PPC──> fs
        (ALL threads blocked during entire chain)

0.4.x:  Thread A ──Call──> vfs_ep ──Call──> fs_ep
        Thread B running on core 1 (unaffected)
        Thread C running on core 2 (unaffected)
```

## Boot Sequence

```
1. seL4 kernel boots, creates root task TCB
2. aios_root receives BootInfo (all capabilities to physical resources)
3. aios_root initializes:
   - allocman (untyped memory allocator)
   - vka (kernel object allocator interface)
   - vspace (virtual memory manager for root task)
4. aios_root creates IPC endpoints:
   - ep_serial, ep_blk, ep_fs, ep_vfs, ep_auth, ep_net, ep_proc
5. aios_root spawns server processes (each gets own TCB + VSpace):
   - serial_driver (mapped to UART MMIO, IRQ cap)
   - blk_driver (mapped to virtio-blk MMIO, IRQ cap)
   - fs_server (shared memory with blk_driver, ep_blk cap)
   - vfs_server (ep_fs cap)
   - auth_server (mapped to auth_store)
   - net_driver + net_server
6. aios_root becomes process server:
   - Listens on ep_proc for spawn/wait/exit/kill requests
   - Loads ELF binaries into new VSpaces
   - Grants endpoint caps to child processes
7. Process server spawns /bin/shell as first user process
8. Shell runs with its own TCB, VSpace, CSpace
   - Syscalls → seL4_Call(ep_vfs, ...) for file ops
   - Syscalls → seL4_Call(ep_proc, ...) for process ops
   - getc → seL4_Call(ep_serial, ...) — only this thread blocks
```

## Process Server (aios_root)

The process server is the core of 0.4.x. It replaces both Microkit's capDL loader and the 0.3.x sandbox kernel.

### Data Structures

```c
typedef struct {
    int pid;
    int ppid;
    int uid;
    int state;              /* FREE, ALIVE, ZOMBIE */
    int exit_code;
    int foreground;
    char name[64];

    /* seL4 objects */
    vspace_t vspace;        /* virtual address space */
    vka_object_t cnode;     /* capability space */
    sel4utils_process_t sel4proc;  /* sel4utils process handle */

    /* Threads */
    int num_threads;
    int thread_ids[MAX_THREADS_PER_PROC];

    /* Signal handling */
    uint32_t sig_pending;
    uintptr_t sig_handlers[32];

    /* File descriptors (caps to server endpoints) */
    seL4_CPtr fd_table[MAX_FDS];
} proc_t;

typedef struct {
    int tid;
    int proc_idx;           /* owning process */
    int state;

    /* seL4 objects */
    vka_object_t tcb;       /* seL4 TCB object */
    vka_object_t sc;        /* scheduling context (MCS) */
    vka_object_t ipc_buf_frame;
    seL4_CPtr reply_cap;    /* for seL4_Reply */
} thread_t;
```

### Syscall Dispatch

User processes invoke syscalls via `seL4_Call()` on a badged endpoint:

```c
/* User process side */
int fd = syscall(SYS_OPEN, "/etc/passwd", O_RDONLY);
// Expands to:
seL4_MessageInfo_t msg = seL4_MessageInfo_new(SYS_OPEN, 0, 0, 2);
seL4_SetMR(0, (seL4_Word)path_in_shared_buf);
seL4_SetMR(1, O_RDONLY);
seL4_MessageInfo_t reply = seL4_Call(ep_vfs, msg);
int result = seL4_GetMR(0);
```

The badge on the endpoint identifies the calling process. The server extracts the badge and looks up the process:

```c
/* VFS server side */
void server_loop(void) {
    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep_vfs, &badge);
        int pid = badge;  /* badge = caller's PID */
        int syscall_nr = seL4_MessageInfo_get_label(msg);
        
        switch (syscall_nr) {
        case SYS_OPEN: ...
        case SYS_READ: ...
        case SYS_WRITE: ...
        }
        
        seL4_Reply(reply_msg);
    }
}
```

### Memory Layout per Process

Each process gets a standard Unix-like virtual address space:

```
0x0000_0000_0040_0000  .text  (ELF loaded here)
0x0000_0000_0050_0000  .data / .bss
0x0000_0000_0060_0000  heap (grows up, managed by brk/mmap)
        ...
0x0000_0000_7FFF_0000  stack (grows down)
0x0000_0000_7FFF_F000  IPC buffer (1 page)
```

The process server allocates physical frames and maps them into the process's VSpace. It can also share frames between processes (e.g., shared memory IPC buffers).

## Migration Plan

### What Carries Over (minimal changes)

| Component | Lines | Changes Needed |
|-----------|-------|----------------|
| fs/ext2.c | 1145 | Replace IPC macros (RD32/WR32 → seL4 message registers) |
| fs/vfs.c | 394 | Same |
| serial_driver.c | 139 | Replace Microkit notify/channel → seL4 Notification/Endpoint |
| blk_driver.c | 237 | Same |
| net_driver.c | 286 | Same |
| net_server.c | 603 | Same |
| auth_server.c | 917 | Same |
| shell.c | 1566 | Replace `sys->` syscall table → `seL4_Call()` wrappers |
| All programs/*.c | ~2000 | Same — replace syscall table with libc wrappers |
| tools/*.py | ~1500 | Unchanged (disk tools, ext2 builder) |
| setjmp.S | 95 | Not needed (seL4 does context save) — keep for user-space cooperative threads if desired |
| test_*.c | ~380 | Adapt syscall interface |

### What Gets Rewritten

| Component | Effort | Description |
|-----------|--------|-------------|
| aios_root (new) | High | Root task: boot, allocman, spawn servers, process server loop |
| libos (new) | Medium | User-space library: syscall wrappers using seL4_Call |
| Process loading | Medium | ELF loader into per-process VSpaces |
| aios.system | Gone | Replaced by aios_root's programmatic setup |
| sandbox.c | Gone | Replaced by seL4's kernel scheduler + aios_root process server |

### What Gets Deleted

- `sandbox.c` (1781 lines) — setjmp/longjmp scheduler, user-space threads, all replaced by seL4 kernel
- `orchestrator.c` (576 lines) — routing logic moves into per-server dispatch loops
- `aios.system` — Microkit system description, replaced by programmatic boot in aios_root
- Microkit SDK dependency — replaced by seL4_libs

## Build System Changes

### Dependencies

```
0.3.x: Microkit SDK 2.1.0 (includes seL4 kernel + capDL loader)
0.4.x: seL4 kernel source + seL4_libs + sel4runtime + elfloader
```

### Build Flow

```
0.3.x: make → compile PDs → microkit tool → loader.img
0.4.x: make → compile aios_root + servers + programs
        → link aios_root as root task ELF
        → build seL4 kernel with CMake
        → elfloader packages kernel + root task → image.elf
```

The build becomes CMake-based (seL4's standard build system) with our Makefile wrapping it for convenience.

## Phased Implementation

### Phase 1: Bare root task boots on seL4 (week 1)

Goal: aios_root boots, prints "Hello from root task", enumerates BootInfo.

- Set up seL4 CMake build for AArch64 QEMU virt
- Write minimal aios_root with allocman + vka + vspace bootstrap
- Boot in QEMU, verify all untypeds available
- Print memory map, available capabilities

### Phase 2: Process server spawns shell (week 2)

Goal: aios_root loads /bin/shell into its own VSpace, shell prints prompt.

- Implement ELF loader using sel4utils_configure_process
- Create VFS endpoint, stub file ops (enough for shell to start)
- Serial driver as a thread in root task initially (simplify)
- Shell gets its own TCB + VSpace, runs with real preemption

### Phase 3: File system + full server split (week 3)

Goal: shell can ls, cat, run programs — all with process isolation.

- Split serial_driver, blk_driver, fs_server into separate processes
- Wire up proper endpoint-based IPC
- Port ext2 filesystem code to new IPC model
- exec() spawns new process (new VSpace + TCB)
- Test suite passes in new architecture

### Phase 4: pthreads + SMP (week 4)

Goal: pthread_create maps to real seL4 TCBs, threads run on multiple cores.

- pthread_create → allocate TCB in same VSpace
- Mutex/condvar backed by seL4 Notification objects
- Pin threads to different cores for true parallelism
- Run stress test: N threads on 4 cores doing concurrent work

### Phase 5: Polish + regression (week 5)

Goal: Feature parity with 0.3.18, all 42 tests pass, plus new isolation tests.

- Signal delivery via fault handlers
- Ctrl-C via serial interrupt → process server → SIGINT
- Background jobs with real isolation
- New tests: process isolation (memory access fault), SMP concurrency
- ARCHITECTURE.md + README.md updated

## Risk Assessment

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|-----------|
| seL4 CMake build complexity | High | Blocks all work | Start from seL4test or UNSW AOS project as template |
| sel4utils API learning curve | Medium | Slows Phase 2 | seL4 tutorials cover exactly this path |
| ELF loading edge cases | Medium | Blocks shell | sel4utils_configure_process handles most cases |
| IPC performance regression | Low | Slower than 0.3.x | seL4 IPC is ~100 cycles, faster than our PPC chain |
| Debug difficulty | Medium | Hard to diagnose | seL4 debug build + printf, GDB via QEMU |

## Reference Projects

These existing seL4 projects demonstrate the patterns we need:

1. **seL4test** — seL4's own test suite, boots as root task, creates processes
   https://github.com/seL4/sel4test

2. **UNSW AOS** — University course project, builds a full OS on raw seL4
   https://github.com/SEL4PROJ/AOS-manifest

3. **nuclearpidgeon/seL4-roottask-test** — Minimal root task with dynamic process creation
   https://github.com/nuclearpidgeon/seL4-roottask-test

4. **seL4 tutorials (libraries-3)** — ELF loading + process creation tutorial
   https://docs.sel4.systems/Tutorials/libraries-3

## Decision Log

| Decision | Rationale |
|----------|-----------|
| Keep seL4 kernel, drop Microkit | seL4 supports everything we need; Microkit restricts it |
| Use seL4_libs (sel4utils, vka, etc.) | Handles capability management complexity; unverified but well-tested |
| CMake build system | seL4 ecosystem standard; required for kernel + library builds |
| C (not Rust) for root task | Consistency with 0.3.x codebase; sel4_libs are C |
| One TCB per POSIX thread | Direct mapping gives kernel-level preemption + SMP |
| Endpoint per service (not per syscall) | Reduces capability count; badge identifies caller |
| ELF loading (not flat binary) | Standard format; sel4utils handles it; enables proper .text/.data/.bss segments |
| Phase 1 target: QEMU virt AArch64 | Same platform as 0.3.x; known working |
