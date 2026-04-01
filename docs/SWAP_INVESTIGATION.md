# Sandbox Swap/Preemption Investigation

## Status: Blocked — Architectural Redesign Required

Investigation date: April 2026
Crash address: 0x204cb4 in sandbox.elf (strb in run_program stack restore loop)

## Problem Statement

When more processes are launched than available sandbox slots (8), the
orchestrator must preempt running processes to make room. This swap
mechanism has multiple bugs and a fundamental architectural limitation.

## Bug Summary

### Fixed

| # | Bug | Root Cause | Fix |
|---|-----|-----------|-----|
| 1 | Crash at 0x204cb4, ADDR=0x10000000000 | Stale proc_state magic on slot reuse | Clear magic before loading new code |
| 2 | Crash at 0x21100000 (prefetch fault) | pd_restart before code loaded (SMP race) | Defer pd_restart until after code loading |
| 3 | Processes self-suspend immediately after load | Stale SBX_SUSPEND_FLAG in shared IO | Clear flag at slot transitions |
| 4 | Double-preemption corruption | pick_victim didn't skip PROC_PREEMPTING | Added state check in selection loop |
| 5 | Cooperative suspend impossible | All 8 sandboxes pinned to cpu=0 | Distributed across 4 cores |

### Unsolved: Cross-Core Cache Coherence

**sandbox_heap** is mapped `cached="true"` in both the orchestrator and
sandbox PDs. When a sandbox on core 1 writes `proc_state_t` (including
the magic number 0x50524F43) to heap[0], the orchestrator on core 0
reads stale data from its own cache:

PREEMPT: kill PID 2 st=4 magic=0 ↑ ↑ | └── cached heap: NOT visible └── uncached IO: visible



`SBX_ST_SUSPENDED` (st=4) is visible because sandbox_io is uncached.
`PROC_STATE_MAGIC` (magic) is invisible because sandbox_heap is cached.

**Attempted mitigations that failed:**
- `dc cvac` (clean by VA to PoC) + `dsb ish` after writing proc_state
- Microkit may not configure inner-shareable attributes on memory regions
- QEMU virt platform cache model may differ from real hardware

## Architecture Issues

### 1. PPC Deadlock on Cooperative Suspend
Shell PPC (SYS_SPAWN) → orchestrator protected() → sets SBX_SUSPEND_FLAG on victim → orchestrator is BUSY handling shell's PPC → victim sees flag, calls save_and_yield() → victim PPCs SYS_SUSPENDED to orchestrator → BLOCKED: orchestrator can't handle two PPCs simultaneously → victim writes SBX_ST_SUSPENDED to shared memory (visible) → victim blocks waiting for PPC reply (never comes during this handler) → timeout fires, orchestrator checks heap magic (invisible due to cache) → process killed and restarted from disk

### 2. Single-Threaded Orchestrator

The orchestrator handles one PPC at a time. It cannot:
- Process a victim's SYS_SUSPENDED while handling the caller's SYS_SPAWN
- Yield to let the victim respond during a synchronous PPC handler

### 3. Register Access Limitation

Microkit does not expose a PD's CPU registers to its parent. Only the
sandbox itself can save its own context via `arch_save_context()`. This
means the orchestrator **cannot** construct a valid proc_state_t for a
forcibly stopped process.

## Memory Layout Reference

| Region | Sandbox VA | Orchestrator VA | Cached | Size |
|--------|-----------|----------------|--------|------|
| sandbox_io | 0x20000000 | 0x30000000+ | **No** | 4 KB |
| sandbox_heap | 0x20100000 | 0x40000000+ | **Yes** | 16 MB |
| sandbox_code | 0x21100000 | 0x30001000+ | No | 4 MB |
| swap_region | — | orchestrator-local | — | 256 MB |

## Process State (proc_state_t)

Located at heap[0] (first 8 KB reserved via PROC_STATE_RESERVE):
- magic (0x50524F43), version
- heap_used, out_len, puts_count, stack_top
- fd_pos[16], fd_size[16]
- ctx[104] (arch_context_t: x19-x30, sp)
- saved_sp, saved_stack_size
- Stack snapshot data follows struct (max ~7.7 KB)

## Proposed Redesign: Single-Sandbox Kernel

Replace N protection domains with one sandbox PD containing a small
user-space kernel:

┌─────────────────────────────────────────┐ │ Orchestrator (PD) │ │ - Shell, fs, auth, scheduling │ │ - Single PPC channel to sandbox │ └────────────────┬────────────────────────┘ │ PPC + shared memory ┌────────────────┴────────────────────────┐ │ Sandbox Kernel (single PD) │ │ ┌──────────┐ ┌──────────┐ ┌────────┐ │ │ │ Process 1│ │ Process 2│ │ Proc N │ │ │ │ (thread) │ │ (thread) │ │(thread)│ │ │ └──────────┘ └──────────┘ └────────┘ │ │ - Internal process table │ │ - Cooperative scheduling (setjmp/longjmp│ │ or arch_save_context/restore) │ │ - Single address space, software MMU │ │ - No cross-PD cache coherence issues │ │ - No PPC deadlocks │ │ - No slot limit (memory-bound only) │ └─────────────────────────────────────────┘


### Benefits
- **No cache coherence issues**: all processes share one PD's cache domain
- **No PPC deadlocks**: context switching is internal (no kernel calls)
- **No pd_restart races**: processes are threads, not PDs
- **No 8-slot limit**: limited only by available heap memory
- **Simpler orchestrator**: one channel instead of 8

### Implementation Notes
- Use existing setjmp/longjmp (jmp_buf = 22 registers, 176 bytes) for
  context switching between processes
- Each process gets a stack allocation from the 16 MB heap
- Code for each process loaded into heap (not separate code region)
- Sandbox kernel intercepts syscalls and forwards to orchestrator
- fork() = save context + allocate new stack + copy state
- exec() = load new code into heap + jump to entry point
EOF

