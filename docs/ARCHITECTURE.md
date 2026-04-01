# AIOS Architecture

## Vision

AIOS is a microkernel operating system built on seL4, designed for stability,
security, and AI-native development. External AI (Claude, etc.) generates and
reviews code, which is compiled and deployed to AIOS. The long-term goal is
self-hosted development within AIOS itself.

## Design Principles

- **Microkernel**: minimal trusted computing base, all services in userspace
- **POSIX-compatible**: applications expect a POSIX-like interface
- **AI-native**: AI agent is a first-class system component
- **Linux as reference**: re-engineer Linux patterns for microkernel security
- **Network-first**: status reporting, remote management, AI API access

## Hardware Targets

- Development: QEMU virt (AArch64) - current platform
- Primary: Raspberry Pi 4/5 (BCM2711/BCM2712, AArch64)
- Stretch: x86-64 (Ryzen Strix Halo)

---

## Architecture Versions

### 0.2.x (Deprecated)

The 0.2.x design used 8 separate sandbox PDs for user processes. This created
issues with threading (pthreads impossible across PDs) and inefficient IPC.

### 0.3.x (Current Design)

The 0.3.x design consolidates to 8 Protection Domains total. A single sandbox
PD contains an internal user-space kernel that manages all processes and threads.

See docs/sandbox_kernel_design.md for full details.

---

## Architecture Overview (0.3.x)

    +------------------------------------------------------------------+
    |                        SANDBOX PD (150)                          |
    |  +------------------------------------------------------------+  |
    |  |              Sandbox Kernel (user-space)                   |  |
    |  |  +--------+ +--------+ +--------+ +--------+               |  |
    |  |  | shell  | | httpd  | | prog   | |  ...   |   Processes   |  |
    |  |  +--------+ +--------+ +--------+ +--------+               |  |
    |  |  Scheduler, Threads, Memory, Local Syscalls                |  |
    |  +----------------------------+-------------------------------+  |
    |                               | PPC (remote syscalls)            |
    +-------------------------------+----------------------------------+
    |                       ORCHESTRATOR (200)                         |
    |               Service Router, Policy, Preemption Tick            |
    +-----------+-----------+-----------+------------------------------+
    | fs_server | net_server| auth_server                              |
    |   (240)   |   (210)   |   (210)                                  |
    +-----------+-----------+-----------+------------------------------+
    | blk_driver| net_driver| serial_driver                            |
    |   (250)   |   (230)   |   (254)                                  |
    +-----------+-----------+-----------+------------------------------+
    |                  seL4 Microkernel (Microkit)                     |
    +------------------------------------------------------------------+
    |                  Hardware (RPi / QEMU)                           |
    +------------------------------------------------------------------+

## Protection Domain Layout (0.3.x)

| PD              | Priority | Role                                    |
|-----------------|----------|-----------------------------------------|
| serial_driver   | 254      | UART hardware access                    |
| blk_driver      | 250      | Disk hardware (virtio-blk, SD card)     |
| fs_server       | 240      | ext2 filesystem                         |
| net_driver      | 230      | Network hardware (virtio-net)           |
| net_server      | 210      | TCP/IP stack (lwIP)                     |
| auth_server     | 210      | Authentication and credentials          |
| orchestrator    | 200      | Service router, policy, preemption tick |
| sandbox         | 150      | User-space kernel, all user processes   |

Down from 14 PDs in 0.2.x to 8 PDs in 0.3.x.

Removed: llm_server (deferred), echo_server, vfs_server (merged), proc_server (merged), sbx0-sbx7.

## Sandbox PD Memory Layout

Total: 512 MB allocated to sandbox PD

    0x20000000  sandbox_io    4 KB     IPC page shared with orchestrator
    0x20001000  sandbox_mem   512 MB   Single region for kernel + user space

Internal layout managed by sandbox kernel:

    Kernel zone (first 1 MB):
      - Process table (dynamic)
      - Thread table (dynamic)
      - Memory allocator metadata
      - Scheduler state

    User memory pool (~510 MB):
      - Per-process: code region, heap, thread stacks
      - Dynamically allocated and freed

## Syscall Flow (0.3.x)

    User program -> sandbox kernel -> (if needed) orchestrator -> service PD

**Local syscalls** (handled in sandbox kernel):

    pthread_create, pthread_join, pthread_yield
    mutex_lock, mutex_unlock
    getpid, getppid, gettid
    malloc, free, sbrk
    sleep (timer-based yield)

**Remote syscalls** (forwarded via PPC to orchestrator):

    open, read, write, close
    exec, fork, exit, waitpid
    socket, connect, send, recv
    kill, signal
    auth operations

## IPC Protocol (sandbox <-> orchestrator)

Single shared IO page (4 KB):

    Offset 0x000: command/status word
    Offset 0x004: syscall number
    Offset 0x008: arg0..arg5 (48 bytes)
    Offset 0x040: return value
    Offset 0x044: error code
    Offset 0x080: data buffer (3.8 KB for read/write/exec payloads)

Sandbox kernel makes PPC call to orchestrator for remote syscalls.
Orchestrator processes and returns result synchronously.

## Scheduling

Cooperative + timer-assisted preemption:

- Round-robin across all READY threads (all processes)
- Context switch on: yield, syscall, mutex contention, sleep
- Orchestrator sends periodic notification for preemption tick
- Sandbox kernel notified() handler saves context and switches

## Code Loading

1. User types "exec foo.bin" in shell
2. Shell calls SYS_EXEC -> sandbox kernel
3. Sandbox kernel allocates code+heap region from pool
4. Sandbox kernel calls orchestrator (PPC): load foo.bin
5. Orchestrator reads file via fs_server into sandbox_mem
6. Sandbox kernel creates process entry, main thread, jumps to entry

## Fault Protection

All processes share one address space - no hardware isolation between them.
The sandbox kernel mitigates risk with:

- Guard canaries between allocations
- Stack canaries on thread stacks
- Bounds checking on syscall arguments
- Watchdog: force-switch threads that run too long without yielding
- Orchestrator watchdog: restart sandbox PD if unresponsive

---

## Boot Sequence

1. seL4 kernel starts, Microkit initializes all PDs
2. Drivers init: serial, block, network
3. fs_server mounts root filesystem (ext2)
4. orchestrator reads /etc/init.cfg
5. sandbox PD initializes internal kernel
6. Starts network services
7. Starts shell on serial console

## Status Reporting

- HTTP server on port 80 serves system status JSON + web UI
- Reports: uptime, memory, disk, network, build info
- Interactive: user can submit priorities, approve/reject proposals
- WebSocket for real-time log streaming

## Development Workflow

**Phase 1 (Current): External AI**
- Claude/GPT generates code on host
- Cross-compile with aarch64-gcc
- Deploy via disk image or network
- Test in QEMU, then on RPi

**Phase 2: Assisted Development**
- AIOS hosts git client
- External AI pushes code via network
- AIOS compiles (TCC port) and tests
- Results reported via status UI

**Phase 3: Self-Hosted**
- AI agent runs inside AIOS (LLM or API access)
- Reads own source, proposes changes
- Compiles, tests, commits to git
- Human reviews via web UI
