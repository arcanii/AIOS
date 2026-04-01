# Sandbox Kernel Design

## Overview

Replace 8 separate sandbox PDs with a single sandbox PD that acts as a
user-space kernel. All user processes and threads run within this PD.
The sandbox kernel manages scheduling, memory, threads, and IPC.
The orchestrator provides privileged services (fs, auth, net, devices).

## Protection Domain Layout

  serial_driver   (254) -- UART hardware
  blk_driver      (250) -- Disk hardware (virtio-blk)
  fs_server       (240) -- ext2 filesystem
  net_driver      (230) -- Network hardware (virtio-net)
  net_server      (210) -- TCP/IP stack
  auth_server     (210) -- Authentication + credentials
  orchestrator    (200) -- Service router, policy
  sandbox         (150) -- User-space kernel

Down from 14 PDs to 8. Removed: llm_server, echo_server, sbx0-sbx7.

## Sandbox PD Memory Layout (512 MB)

  0x20000000  sandbox_io   4 KB    IPC page shared with orchestrator
  0x20001000  sandbox_mem  512 MB   Single region for kernel + user space

  Internal layout managed by sandbox kernel:

    Kernel zone (first 1 MB):
      - Process table (dynamically sized)
      - Thread table (dynamically sized)
      - Memory allocator metadata
      - Scheduler state

    User memory pool (remaining ~510 MB):
      - Per-process: code region, heap, thread stacks
      - Dynamically allocated and freed
      - Limits bounded only by available memory

## Process Model

  - PID: unique, monotonically increasing
  - Code region: allocated from user pool, loaded by orchestrator
  - Heap region: grows via sbrk, managed by sandbox kernel allocator
  - One or more threads, each with its own stack
  - States: READY, RUNNING, BLOCKED, ZOMBIE
  - No fixed limit on process count (memory-bounded)

## Thread Model

  - Thread ID: unique within process
  - Stack: allocated from user pool (default 64 KB, configurable)
  - Saved context: callee-saved regs via setjmp/longjmp
  - States: READY, RUNNING, BLOCKED, FINISHED
  - No fixed limit on thread count (memory-bounded)

## Scheduling

  Cooperative + timer-assisted preemption:
  - Round-robin across all READY threads (all processes)
  - Context switch on: yield, syscall, mutex contention, sleep
  - Orchestrator sends periodic notification for preemption tick
  - Sandbox kernel notified() handler saves context and switches

## Syscall Flow

  User program -> sandbox kernel -> (if needed) orchestrator -> service PD

  Local syscalls (handled in sandbox kernel):
    pthread_create, pthread_join, pthread_yield, mutex_lock, mutex_unlock,
    getpid, getppid, gettid, malloc, free, sbrk, sleep (timer-based yield)

  Remote syscalls (forwarded to orchestrator via PPC):
    open, read, write, close, exec, fork, exit, waitpid,
    socket, connect, send, recv, kill, signal, auth operations

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

## Code Loading

  1. User types exec foo.bin in shell
  2. Shell calls SYS_EXEC -> sandbox kernel
  3. Sandbox kernel allocates code+heap region from pool
  4. Sandbox kernel calls orchestrator (PPC): load foo.bin
  5. Orchestrator reads file via fs_server into sandbox_mem
     (orchestrator has RW mapping of sandbox_mem)
  6. Sandbox kernel creates process entry, main thread, jumps to entry

## Preemptive Multitasking

  The orchestrator sends a periodic notification to the sandbox PD.
  The sandbox kernel notified() handler fires, saves the current
  thread context, and switches to the next READY thread.
  This gives preemptive behavior without hardware timer access.

## Fault Protection

  All processes share one address space -- no hardware isolation between them.
  The sandbox kernel mitigates risk with:
    - Guard canaries between allocations
    - Stack canaries on thread stacks
    - Bounds checking on syscall arguments
    - Watchdog: force-switch threads that run too long without yielding
    - Orchestrator watchdog: restart sandbox PD if unresponsive

## Migration Plan

  Phase 2a: Consolidate aios.system -- single sandbox PD, 512 MB memory
  Phase 2b: Rewrite sandbox.c as sandbox kernel with process/thread tables
  Phase 2c: Move scheduler from orchestrator into sandbox kernel
  Phase 2d: Simplify orchestrator to pure service router
  Phase 2e: Add preemption via orchestrator notification tick
  Phase 2f: Add pthread API for user programs
