# AIOS fork() Implementation — Definitive Guide

## Status: WORKING (v0.4.46, build 1013+)
## Date: 2026-04-05

## Summary

AIOS implements POSIX `fork()`, `waitpid()`, `getpid()`, and `exit()` on bare
seL4 15.0.0 (AArch64, non-MCS scheduler, 4-core SMP). The child process
receives a complete copy of the parent's address space and resumes execution
at the instruction after the fork() call, with return value 0. The parent can
block until the child exits and retrieve its exit code.

This is believed to be the first implementation of fork()+waitpid() on bare
seL4 without CAmkES or Microkit.

```
/ $ fork_test
fork_test: about to fork (my pid=9)...
fork_test: CHILD here (pid=10)
fork_test: PARENT (pid=9), child=10 exited with 42
```

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
     │                                  │ 1. Load same ELF
     │                                  │ 2. Copy .data (299 pages)
     │                                  │ 3. Copy stack (16 pages)
     │                                  │ 4. Copy caps from parent CSpace
     │                                  │ 5. Mint child's pipe_ep (unique badge)
     │                                  │ 6. Set registers (PC+4, x0-x5)
     │                                  │ 7. Write IPC buffer (MR0=0, MR1=pid)
     │                                  │ 8. Resume child
     │                                  └─────┬─────┘
     │                                        │
     ◄─── reply: MR0 = child_pid ─────────────┤
     │    (parent continues,                   │
     │     fork() returns child_pid)           │
     │                                         │
     ├─ waitpid(child_pid) ──────────────────►│
     │  seL4_Call(pipe_ep, PIPE_WAIT)          │
     │  (parent blocks, SaveCaller)            │
     │                                         │
     │         Child Process                   │
     │              │                          │
     │              ├─ resumes at PC+4         │
     │              │  x2=0 → fork() returns 0 │
     │              │  resets aios_pid cache    │
     │              │                          │
     │              ├─ getpid() ──────────────►│ PIPE_GETPID
     │              │◄── reply: child's PID ───┤
     │              │                          │
     │              ├─ (does work)             │
     │              │                          │
     │              ├─ exit(42) ──────────────►│ PIPE_EXIT(42)
     │              │◄── reply ────────────────┤
     │              │                          │
     │              ├─ NULL deref (fault) ─────┤
     │              │  (triggers reaper)       │
     │              ×                          │
     │                                   ┌─────┴─────┐
     │                                   │ spin-reap  │
     │                                   │ NBRecv on  │
     │                                   │ fault EP   │
     │                                   │ → child    │
     │                                   │   exited   │
     │                                   └─────┬─────┘
     │                                         │
     ◄─── reply via SaveCaller: ───────────────┤
     │    MR0=child_pid, MR1=42                │
     │    (parent unblocked,                   │
     │     waitpid returns, status=42)         │
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
1. Manual page table construction requires understanding seL4's 4-level
   AArch64 page table format and ASID assignment
2. sel4utils tracks all page mappings internally; bypassing it causes
   `vspace_get_cap()` to return NULL for pages that exist
3. The child's TCB must be configured with a valid VSpace root and IPC buffer

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

### Why child needs its own badged pipe_ep

The child's pipe_ep cap is initially copied from the parent's CSpace, giving it
the parent's badge. This means pipe_server can't distinguish child from parent.
Critical for: PIPE_EXIT (must store exit code on child, not parent), PIPE_GETPID
(must return child's PID, not parent's).

Fix: after copying caps from parent, delete the child's pipe_ep slot and mint a
fresh cap with `badge = child_idx + 1`:

```c
seL4_CNode_Delete(child_cnode, pipe_slot, depth);
seL4_CNode_Mint(child_cnode, pipe_slot, depth,
                pipe_src.root, pipe_src.capPtr, pipe_src.capDepth,
                seL4_AllRights, (seL4_Word)(child_idx + 1));
```

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
| 12 | pipe_ep | Pipe server endpoint (**re-minted with child's badge**) |

Slots 1-7 are populated by `sel4utils_configure_process_custom`.
Slots 8-11 are copied from the parent's CSpace using `seL4_CNode_Copy`.
Slot 12 (pipe_ep) is deleted and re-minted with the child's unique badge.

```c
/* Copy caps 8-11 from parent */
seL4_CNode_Copy(child_cnode, slot, 12,
                parent_cnode, slot, 12, seL4_AllRights);

/* Re-mint pipe_ep with child's badge */
seL4_CNode_Delete(child_cnode, pipe_slot, 12);
seL4_CNode_Mint(child_cnode, pipe_slot, 12,
                pipe_src.root, pipe_src.capPtr, pipe_src.capDepth,
                seL4_AllRights, (seL4_Word)(child_idx + 1));
```

## PID Management

### Problem
After fork, the child's POSIX shim globals contain the parent's PID (copied
via .data). `getpid()` returns the parent's PID.

### Solution: Reset + Lazy Query
In the fork return path, the child resets `aios_pid = 0`. The next `getpid()`
call triggers a `PIPE_GETPID` IPC to pipe_server, which returns the correct
PID based on the caller's badge.

```c
/* In aios_sys_clone, after fork returns 0 */
if (result == 0) {
    aios_pid = 0;  /* invalidate cached PID */
}

/* In aios_sys_getpid */
if (aios_pid <= 1 && pipe_ep) {
    seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
        seL4_MessageInfo_new(PIPE_GETPID, 0, 0, 0));
    long pid = (long)seL4_GetMR(0);
    if (pid > 0) aios_pid = (int)pid;
}
return (long)aios_pid;
```

### Why not query PID in fork return path directly?

Early attempts called `PIPE_GETPID` immediately in `aios_sys_clone()` after
fork returned 0. This crashed because the child had not been fully registered
in `active_procs` by the time pipe_server processed PIPE_GETPID. Using lazy
evaluation in `getpid()` avoids the race: by the time the child calls
`getpid()`, pipe_server has finished do_fork and the child is fully registered.

## waitpid() Implementation

### Challenge
pipe_server uses blocking `seL4_Recv` — it cannot simultaneously wait for
pipe IPC and child fault events.

### Solution: SaveCaller + Spin-Reap

When a parent calls `PIPE_WAIT` and the child is still alive:

1. **SaveCaller**: Save the parent's reply cap to a pre-allocated CSpace slot
2. **Defer**: Don't reply — parent remains blocked in seL4_Call
3. **Spin-Reap**: When pipe_server processes `PIPE_EXIT` from the child, it
   spins briefly (`seL4_Yield` loop) waiting for the child's fault, then
   reaps and replies to the waiting parent

```c
/* PIPE_WAIT handler — save reply cap */
seL4_CNode_Delete(seL4_CapInitThreadCNode, reply_cap, seL4_WordBits);
seL4_CNode_SaveCaller(seL4_CapInitThreadCNode, reply_cap, seL4_WordBits);

wait_pending[wi].active = 1;
wait_pending[wi].waiting_pid = caller_pid;
wait_pending[wi].child_pid = target_pid;
wait_pending[wi].reply_cap = reply_cap;
/* Do NOT reply — parent blocks until child exits */
```

### PIPE_EXIT + Spin-Reap

When a child sends `PIPE_EXIT(code)`:

1. Store `exit_status` on the child's `active_proc_t`
2. Reply to child (child then faults via NULL deref)
3. Spin: `seL4_NBRecv` on child's fault EP in a tight loop (up to 100 yields)
4. When fault arrives: call `reap_forked_child` which:
   - Destroys the child process
   - Looks up the waiting parent in `wait_pending`
   - Replies to parent via saved reply cap with (child_pid, exit_status)

```c
/* After replying to PIPE_EXIT */
for (int spin = 0; spin < 100; spin++) {
    seL4_Yield();
    seL4_MessageInfo_t probe = seL4_NBRecv(
        active_procs[caller_idx].fault_ep.cptr, NULL);
    if (seL4_MessageInfo_get_label(probe) != 0) {
        reap_forked_child(caller_idx);
        break;
    }
}
```

### Zombie Table

If a child exits before the parent calls waitpid, `reap_forked_child` saves
the exit status to a zombie table:

```c
typedef struct {
    int active;
    int pid;
    int ppid;
    int exit_status;
} zombie_t;
static zombie_t zombies[MAX_ZOMBIES];
```

When the parent later calls `PIPE_WAIT`, the handler checks the zombie table
first. If the child is already there, it returns immediately with the stored
exit status.

## Exit Code Delivery

### Challenge
musl's `_exit()` does a raw `__syscall(SYS_exit_group)` which executes `svc`
with x8=94. seL4 treats this as an invalid syscall, causing an immediate fault.
The exit code is lost.

### Solution: sel4runtime Exit Callback
Register `aios_exit_cb` with sel4runtime during `aios_init()`:

```c
static void aios_exit_cb(int code) {
    if (pipe_ep) {
        seL4_SetMR(0, (seL4_Word)code);
        seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_EXIT, 0, 0, 1));
    }
    /* Fault to trigger reaper */
    volatile int *p = (volatile int *)0;
    *p = 0;
    __builtin_unreachable();
}

/* In aios_init */
sel4runtime_set_exit(aios_exit_cb);
```

This intercepts `return N` from `main()` which goes through
`sel4runtime_exit(N) → aios_exit_cb(N) → PIPE_EXIT(N) → fault`.

**Note**: `_exit()` bypasses sel4runtime and goes directly to the kernel,
so `_exit(N)` loses the exit code. Use `return N` from main or `exit(N)`
(which calls atexit handlers then sel4runtime_exit).

### Exit Flow

```
main() returns 42
  → sel4runtime_exit(42)
    → __sel4runtime_run_destructors()
    → aios_exit_cb(42)
      → seL4_Call(pipe_ep, PIPE_EXIT) with MR0=42
      → pipe_server stores exit_status=42
      → reply
      → *(volatile int *)0 = 0  (NULL deref → fault)
        → pipe_server spin-reap detects fault
          → reap_forked_child()
            → reply to waiting parent with (pid, 42)
```

## seL4 Fault Types (AArch64 HYP)

During debugging, understanding fault labels was critical:

| Label | Type | MR0 | MR1 | MR2 | MR3 |
|-------|------|-----|-----|-----|-----|
| 1 | CapFault | IP | CPtr | InRecvPhase | LookupFailureType |
| 5 | VMFault | IP | FaultAddr | PrefetchFault | FSR |

Common fault patterns during fork development:
- `VMFault addr=0x8`: IPC buffer pointer is NULL (TLS page not copied)
- `CapFault CPtr=8`: serial_ep cap missing from child CSpace
- `CapFault CPtr=0x10050xxx`: garbage CPtr from wrong x0 ABI register
- `VMFault addr=0x10050xxx`: jump to unmapped address from corrupted stack

## SMP Considerations

AIOS runs on 4-core SMP. Key SMP effects observed during fork:

1. **Interleaved output**: Parent and child printf simultaneously on different
   cores, producing character-interleaved output (e.g., `ffoorrkk__tteesstt`).
   This is correct SMP behavior.

2. **Child exits before parent calls waitpid**: On SMP, the child can complete
   its entire lifetime before the parent even calls waitpid. The zombie table
   handles this case.

3. **reap_check race with pipe_server**: If reap_check runs in pipe_server's
   main loop (before seL4_Recv), it can destroy a child before the child's
   PIPE_EXIT is processed. Fix: moved reap_check to run after each message
   and as a spin-reap after PIPE_EXIT.

4. **PIPE_EXIT → fault timing**: After pipe_server replies to PIPE_EXIT, the
   child must fault (NULL deref) before the fault EP can be checked. On SMP,
   this happens near-instantly. The spin-reap loop (100 yields) handles it.

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

### 11. Forked children need unique badges
If the child inherits the parent's badged pipe_ep, pipe_server can't
distinguish them. PIPE_EXIT from child stores exit code on the parent's
active_proc entry instead of the child's. Fix: re-mint pipe_ep with
`badge = child_idx + 1` after copying caps.

### 12. seL4_Recv blocks — can't wait for child faults
pipe_server blocks in `seL4_Recv` waiting for the next IPC. If a child faults
between messages, nobody checks the fault EP. Fix: spin-reap after PIPE_EXIT
(the child will fault immediately after the reply).

### 13. _exit() bypasses sel4runtime
musl's `_exit()` does a raw `svc` syscall that seL4 doesn't understand,
causing an immediate fault with exit code lost. Only `return` from `main()`
or `exit()` (not `_exit()`) go through sel4runtime_exit and our callback.

### 14. aios_pid must be reset, not queried, in fork return
Calling PIPE_GETPID immediately in the fork return path crashes because the
child isn't fully registered yet (SMP race). Instead, reset `aios_pid = 0`
and let `getpid()` lazily query when first called.

### 15. Zombie table needed for SMP
On SMP, the child can exit before the parent calls waitpid. Without a zombie
table, the exit status is lost when reap_forked_child destroys the process.

## Files Modified

| File | Changes |
|------|---------|
| src/aios_root.c | do_fork(), fork helpers, PIPE_FORK/GETPID/WAIT/EXIT handlers, reap_check(), zombie table, spin-reap |
| src/lib/aios_posix.c | aios_sys_clone(), aios_sys_wait4(), aios_exit_cb(), aios_pid reset, PIPE_GETPID lazy query |
| src/apps/fork_test.c | Test program (fork + waitpid + exit code) |
| include/aios/procfs.h | ppid, exit_status fields in active_proc_t |
| projects/aios/CMakeLists.txt | fork_test added |

## IPC Labels

| Label | Name | Purpose |
|-------|------|---------|
| 65 | PIPE_FORK | Fork calling process |
| 66 | PIPE_GETPID | Query caller's PID |
| 67 | PIPE_WAIT | Block until child exits (SaveCaller) |
| 68 | PIPE_EXIT | Child sends exit code before dying |

## Data Structures

### active_proc_t (extended for fork)
```c
typedef struct {
    int active;
    int pid;
    int ppid;           /* parent PID (>0 for forked children) */
    uint32_t uid, gid;
    sel4utils_process_t proc;
    vka_object_t fault_ep;
    int num_threads;
    aios_thread_t threads[MAX_THREADS_PER_PROC];
    int num_segs;
    elf_seg_info_t segs[MAX_ELF_SEGS];
    seL4_CPtr child_ser_slot, child_fs_slot, child_exec_slot;
    seL4_CPtr child_auth_slot, child_pipe_slot, child_thread_slot;
    int exit_status;    /* set by PIPE_EXIT before child faults */
} active_proc_t;
```

### wait_pending_t (SaveCaller table)
```c
typedef struct {
    int active;
    int waiting_pid;    /* PID of parent that called waitpid */
    int child_pid;      /* PID being waited on (-1 = any) */
    seL4_CPtr reply_cap; /* saved reply cap to unblock parent */
} wait_pending_t;
```

### zombie_t (exit status for already-exited children)
```c
typedef struct {
    int active;
    int pid;
    int ppid;
    int exit_status;
} zombie_t;
```

## Testing

```
/ $ fork_test
fork_test: about to fork (my pid=9)...
fork_test: CHILD here (pid=10)
fork_test: PARENT (pid=9), child=10 exited with 42
/ $ fork_test
fork_test: about to fork (my pid=11)...
fork_test: CHILD here (pid=12)
fork_test: PARENT (pid=11), child=12 exited with 42
/ $ cat /proc/status
PID  PRI  NICE  STATE  UID   THR  NAME
(clean — no zombies)
```

Tested: 12+ consecutive fork+wait cycles without resource exhaustion.

## Future Work

1. **exec() after fork**: Replace child's image with a new ELF (fork+exec pattern)
2. **TTY Phase 2**: getty/shell separation using fork+exec
3. **Copy-on-write**: Defer .data page copying until write fault (v0.5.x)
4. **Multi-threaded fork**: Handle processes with multiple threads
5. **Signal delivery**: SIGCHLD to parent when child exits
6. **_exit() support**: Intercept SYS_exit_group at the musl syscall shim level
