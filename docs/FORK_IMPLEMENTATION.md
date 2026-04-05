# AIOS fork() Implementation — Definitive Guide

## Status: WORKING (v0.4.46)
## Date: 2026-04-05

## Summary

AIOS implements POSIX `fork()` on bare seL4 15.0.0 (AArch64, non-MCS scheduler).
The child process receives a complete copy of the parent's address space and resumes
execution at the instruction after the fork() call, with return value 0.

This is believed to be the first implementation of fork() on bare seL4 without
CAmkES or Microkit.

## Architecture

```
Parent Process                          pipe_server (root VSpace)
     │                                        │
     ├─ fork() ──────────────────────────────►│
     │  aios_sys_clone()                      │
     │  seL4_Call(pipe_ep, PIPE_FORK)          │
     │  (parent blocks)                       │
     │                                  ┌─────┴─────┐
     │                                  │ do_fork()  │
     │                                  │            │
     │                                  │ 1. Load same ELF into new VSpace
     │                                  │ 2. Copy .data pages from parent
     │                                  │ 3. Copy stack pages from parent
     │                                  │ 4. Copy caps from parent CSpace
     │                                  │ 5. Set registers (PC+4, x0-x5)
     │                                  │ 6. Write IPC buffer (MR0=0)
     │                                  │ 7. Resume child
     │                                  └─────┬─────┘
     │                                        │
     ◄─── reply: MR0 = child_pid ─────────────┤
     │    (parent continues,                   │
     │     fork() returns child_pid)           │
     │                                         │
     │         Child Process                   │
     │              │                          │
     │              ├─ resumes at PC+4         │
     │              │  x2=0 → fork() returns 0 │
     │              │  calls PIPE_GETPID       │
     │              │  for correct PID         │
     │              │                          │
```

## Key Design Decisions

### Why pipe_server handles fork (not exec_thread)

exec_thread blocks waiting for child exit (`seL4_Recv` on fault EP). It cannot
receive new IPC while blocked. pipe_server runs in root's VSpace, never blocks
on long operations, and every process already has a badged `pipe_ep`. The badge
identifies the calling process (badge = active_procs index + 1).

### Why ELF reload (not raw VSpace copy)

seL4's `sel4utils_configure_process_custom()` + `sel4utils_elf_load()` sets up:
- Page tables (PGD/PUD/PMD/PTE hierarchy)
- Stack pages (16 pages, tracked by vspace manager)
- IPC buffer page (tracked, configured in TCB)
- .text and .data segments (properly aligned, tracked)

Attempting to manually create a VSpace and map pages fails because:
1. `vspace_get_cap()` cannot see pages allocated by sel4runtime (TLS, auxv)
2. sel4runtime's static TLS array is in .data but its initialization state
   depends on the full `_start → __sel4runtime_load_env` path
3. Manual page table construction requires understanding seL4's 4-level
   AArch64 page table format and ASID assignment

By reloading the same ELF, we get a correctly structured VSpace for free,
then overwrite the writable portions with the parent's state.

### Why .data copy (not .text copy)

- **.text** (flags=5, PF_R|PF_X): Both parent and child share the same code.
  The ELF reload gives the child identical .text pages. No copy needed.
- **.data** (flags=6, PF_R|PF_W): Contains all mutable state — global variables,
  static TLS (16KB), morecore/heap area (1MB), musl internal buffers.
  Must be overwritten with parent's content so child inherits parent's state.

### Why stack copy skips IPC buffer page

The child's IPC buffer (at 0x10000000) is configured by `sel4utils_configure_thread`
and registered with the child's TCB via `seL4_TCB_Configure`. If we overwrite it
with the parent's IPC buffer content, the kernel's TCB configuration becomes
inconsistent. The IPC buffer is initialized fresh for each process.

## Memory Layout

A typical AIOS process (fork_test) has this memory map:

```
0x400000 - 0x420930   .text (PT_LOAD, R-X)     ~131 KB, 33 pages
0x421000 - 0x54c000   .data+.bss (PT_LOAD, RW) ~1.2 MB, 299 pages
                      Includes:
                      - Global variables
                      - static_tls[16384]           (sel4runtime TLS)
                      - morecore_area[1048576]       (musl heap)
                      - musl internal buffers
0x10000000            IPC buffer (1 page)
0x10001000-0x10011000 Stack (16 pages, grows down)
0x10011xxx            Stack top (SP points here)
```

Total mapped pages: ~349 (33 .text + 299 .data + 17 stack/IPC)

### Critical Discovery: No Dynamic Page Allocation

Every byte of process memory is statically allocated in the ELF binary:
- **TLS**: `static char static_tls[16384]` in sel4runtime/src/env.c
- **Heap**: `char morecore_area[1048576]` in libsel4muslcsys/src/sys_morecore.c
- **Stack**: Allocated by sel4utils at process creation time
- **IPC buffer**: Allocated by sel4utils at thread creation time

Zero pages are allocated at runtime. `sys_brk()` and `sys_mmap()` just move
pointers within the static morecore_area. This means `vspace_get_cap()` can
see ALL mapped pages (they're all ELF-loaded or sel4utils-allocated).

The one exception that caused days of debugging: the .data segment boundary.

## The Off-By-One Bug

The root cause of all fork failures was a single page not being copied.

### The Bug

```c
/* WRONG */
int np = (int)((seg->memsz + PAGE_SIZE - 1) / PAGE_SIZE);
uintptr_t base = seg->vaddr & ~((uintptr_t)PAGE_SIZE - 1);
```

The .data segment starts at `vaddr=0x421f80` (not page-aligned).
`base` is aligned down to `0x421000`. But `memsz=0x129308` counts from
`vaddr`, not from `base`. The page count computes from memsz alone:

```
np = (0x129308 + 0xFFF) / 0x1000 = 298 pages
Last page = 0x421000 + 297 * 0x1000 = 0x54a000
```

But the segment actually extends to `vaddr + memsz = 0x421f80 + 0x129308 = 0x54b288`.
The TLS data (tpidr=0x54b1d8) is on page `0x54b000` — **page 299, not copied!**

### The Fix

```c
/* CORRECT */
uintptr_t base = seg->vaddr & ~((uintptr_t)PAGE_SIZE - 1);
uintptr_t end = seg->vaddr + seg->memsz;
int np = (int)((end - base + PAGE_SIZE - 1) / PAGE_SIZE);
```

This computes the page count from the aligned base to the actual end: 299 pages.

### Why This Matters

The 299th page (0x54b000) contains the TLS block where `__sel4_ipc_buffer` is
stored. Without this page, the child's IPC buffer pointer is NULL. The first
`seL4_Call` in the child reads MR0 from address `NULL + 8 = 0x8`, causing a
VMFault. This manifested as fault label=5 (VMFault on AArch64 HYP), addr=8.

## Register Setup (AArch64 seL4 ABI)

After `seL4_Call`, the AArch64 seL4 kernel returns:

| Register | Purpose | Value for child |
|----------|---------|----------------|
| x0 | Badge | 0 |
| x1 | Reply MessageInfo | `seL4_MessageInfo_new(0, 0, 0, 2)` |
| x2 | MR0 (first message register) | 0 (fork returns 0) |
| x3 | MR1 | child_pid |
| x4 | MR2 | 0 |
| x5 | MR3 | 0 |
| PC | Program counter | parent_PC + 4 (skip svc) |
| SP | Stack pointer | parent_SP (same stack) |
| tpidr_el0 | TLS base pointer | parent_tpidr (TLS is in copied .data) |

The parent's PC points AT the `svc` instruction (the seL4 syscall). The child
must resume AFTER it, hence PC + 4.

## IPC Buffer Setup

In addition to registers, the child's IPC buffer must have MR0=0:

```c
seL4_Word *ipc_words = (seL4_Word *)((char *)ipc_tmp + page_off);
ipc_words[1] = 0;           /* MR0 = 0 */
ipc_words[2] = child_pid;   /* MR1 = child's PID */
```

IPC buffer layout:
- Offset 0: seL4_MessageInfo_t (tag)
- Offset 8: msg[0] = MR0
- Offset 16: msg[1] = MR1

On AArch64 fast path, MRs 0-3 come from registers (x2-x5), not the IPC buffer.
But we write both to be safe — the seL4_GetMR() function may read from either
depending on the path taken.

## Capability Space Setup

Caps are copied from the parent's CSpace to the child's CSpace at the **exact
same slot numbers**:

| Slot | Cap | Purpose |
|------|-----|---------|
| 1 | CNode | CSpace root (set by sel4utils) |
| 2 | Fault EP | Fault endpoint (set by sel4utils) |
| 3 | PD | Page directory (set by sel4utils) |
| 4 | ASID Pool | (set by sel4utils) |
| 5 | TCB | Thread control block (set by sel4utils) |
| 6 | (skip) | Sched context (non-MCS) |
| 7 | (skip) | Reply object (non-MCS) |
| 8 | serial_ep | TTY server endpoint |
| 9 | fs_ep | Filesystem endpoint (badged) |
| 10 | thread_ep | Thread server endpoint (badged) |
| 11 | auth_ep | Auth server endpoint |
| 12 | pipe_ep | Pipe server endpoint (badged) |

Slots 1-7 are populated by `sel4utils_configure_process_custom`.
Slots 8-12 are copied from the parent's CSpace using `seL4_CNode_Copy`:

```c
seL4_CNode_Copy(child_cnode, slot, 12,   /* dest */
                parent_cnode, slot, 12,   /* src */
                seL4_AllRights);
```

Using the parent's CSpace as source (not root's) ensures badged caps retain
their badges.

## PID Management

### Problem
After fork, the child's POSIX shim globals contain the parent's PID (copied
via .data). `getpid()` returns the parent's PID.

### Solution: PIPE_GETPID IPC
The child calls `PIPE_GETPID` via pipe_server immediately after fork returns 0.
pipe_server uses the badge to look up the caller's `active_procs` entry and
returns the correct PID.

```c
/* In fork return path */
if (result == 0) {
    seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
        seL4_MessageInfo_new(PIPE_GETPID, 0, 0, 0));
    aios_pid = (int)seL4_GetMR(0);
}
```

### getpid() also queries on first call
```c
static long aios_sys_getpid(va_list ap) {
    if (aios_pid <= 1 && pipe_ep) {
        /* Query real PID from pipe_server */
        seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
            seL4_MessageInfo_new(PIPE_GETPID, 0, 0, 0));
        long pid = (long)seL4_GetMR(0);
        if (pid > 0) aios_pid = (int)pid;
    }
    return (long)aios_pid;
}
```

## Zombie Reaping

Forked children that exit (via VM fault) must be cleaned up. pipe_server
calls `reap_check()` at the top of its main loop:

```c
static void reap_check(void) {
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
        if (!active_procs[i].active) continue;
        if (active_procs[i].ppid <= 0) continue;  /* not forked */
        seL4_MessageInfo_t probe = seL4_NBRecv(active_procs[i].fault_ep.cptr, NULL);
        if (seL4_MessageInfo_get_label(probe) != 0) {
            /* Child exited — clean up */
            sel4utils_destroy_process(&active_procs[i].proc, &vka);
            vka_free_object(&vka, &active_procs[i].fault_ep);
            proc_remove(active_procs[i].pid);
            active_procs[i].active = 0;
        }
    }
}
```

This uses non-blocking receive (`seL4_NBRecv`) to check each forked child's
fault endpoint. If the child has faulted (exited), it's destroyed and removed
from the process table.

## Hard-Won Learnings

### 1. vspace_get_cap() works for ALL process pages
Despite initial suspicion, `vspace_get_cap()` can see every mapped page in a
process. The "invisible pages at 0x10050000" were a red herring — those pages
don't exist. The faults at those addresses were caused by NULL pointer
dereferences from a missing IPC buffer pointer in TLS.

### 2. Frame caps belong to their VSpace
`vspace_get_cap()` returns a cap that's already mapped into the owner's page
table. seL4 won't let you map the same cap into root's VSpace. You must
`seL4_CNode_Copy()` the cap first, then map the copy into root for reading.

### 3. sel4utils pre-allocates stack pages
`sel4utils_configure_process_custom()` allocates stack + IPC buffer pages.
`vspace_reserve_range_at()` for the same addresses fails. Use
`fork_copy_into_existing()` which detects pre-existing pages and copies
content into them.

### 4. No dynamic memory allocation in seL4 userspace
TLS is a static 16KB array. Heap (morecore) is a static 1MB array.
Both are in .data. `sys_brk()` and `sys_mmap()` just move pointers.
This simplifies fork enormously — no need to track dynamic allocations.

### 5. AArch64 seL4 syscall ABI
After `svc 0`, the return convention is:
- x0 = badge (NOT MessageInfo!)
- x1 = reply MessageInfo
- x2 = MR0, x3 = MR1, x4 = MR2, x5 = MR3

Getting this wrong causes the child to interpret MR0 as a pointer and
dereference it, leading to bizarre faults at seemingly random addresses.

### 6. PC must advance past svc
ReadRegisters returns PC pointing AT the svc instruction. If you don't
add 4, the child re-executes the syscall with corrupted registers.

### 7. Page count must account for alignment
When a segment starts mid-page (e.g., 0x421f80), the page-aligned base
is one page earlier (0x421000). The page count must span from aligned
base to segment end, not just memsz/PAGE_SIZE.

### 8. IPC buffer page must NOT be overwritten
The child's IPC buffer is configured in its TCB by sel4utils. Overwriting
it with the parent's content corrupts the kernel's view of the buffer.

### 9. CSpace slots 8+ are free after configure_process
sel4utils populates slots 1-7 (CNode, fault EP, PD, ASID, TCB, SC, Reply).
Slots 8+ are available for user caps. This is consistent between parent
and child, so copying caps at the same slot numbers works.

### 10. seL4_CapData_Guard_new, not seL4_CNode_Guard_new
The API for creating CSpace guard data is `seL4_CapData_Guard_new()`.

## Files Modified

| File | Changes |
|------|---------|
| src/aios_root.c | do_fork(), fork helpers, PIPE_FORK/GETPID handlers, reap_check() |
| src/lib/aios_posix.c | aios_sys_clone(), PIPE_GETPID query, aios_pid tracking |
| src/apps/fork_test.c | Test program |
| include/aios/procfs.h | ppid field in active_proc_t |
| projects/aios/CMakeLists.txt | fork_test added |

## IPC Labels

| Label | Name | Purpose |
|-------|------|---------|
| 65 | PIPE_FORK | Fork calling process |
| 66 | PIPE_GETPID | Query caller's PID |

## Testing

```
/ $ fork_test
fork_test: about to fork (my pid=9)...
fork_test: PARENT here (pid=9), child=10
/ $ cat /proc/status
(proc table shows no zombie — child was reaped)
/ $ fork_test
fork_test: about to fork (my pid=12)...
fork_test: PARENT here (pid=12), child=13
```

Tested: 6 consecutive forks without resource exhaustion.

## Future Work

1. **waitpid()**: Parent blocks until child exits, receives exit status
2. **exec() after fork**: Replace child's image with a new ELF (fork+exec pattern)
3. **Copy-on-write**: Defer .data page copying until write fault (v0.5.x)
4. **Multi-threaded fork**: Handle processes with multiple threads
5. **Signal delivery**: SIGCHLD to parent when child exits
