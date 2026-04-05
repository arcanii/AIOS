# AIOS fork() Implementation — Save Point & Learnings

## Date: 2026-04-05
## Version: v0.4.45 (build 993)
## Status: WIP — core infrastructure in place, child execution not yet working

## What We Built

### Infrastructure (working)

1. **PIPE_FORK IPC label (65)** — fork requests routed through pipe_server
   (always available, never blocks, runs in root VSpace)

2. **Badged pipe_ep** — each child gets a minted pipe_ep with badge = ap_idx + 1,
   so pipe_server can identify the calling process for fork

3. **ELF segment recording** — at exec time, we now record PT_LOAD segments
   (vaddr, memsz, flags) in `active_proc_t.segs[]` for use during fork

4. **Cap slot tracking** — `active_proc_t` stores child_ser_slot, child_fs_slot,
   child_thread_slot, child_auth_slot, child_pipe_slot so fork can replicate
   the exact CSpace layout

5. **aios_sys_clone()** in aios_posix.c — intercepts `__NR_clone` syscall,
   sends PIPE_FORK to pipe_server via badged pipe_ep

6. **fork_test.c** — test program in CMakeLists.txt and on disk

7. **Helper functions** (in aios_root.c):
   - `fork_copy_region()` — duplicate writable pages via CNode_Copy + temp map + memcpy
   - `fork_copy_into_existing()` — copy content into child's pre-existing pages
   - `fork_share_region()` — share read-only pages (e.g., .text)
   - `do_fork()` — orchestrates the full fork sequence

### fork_test output (current state)

```
fork_test: about to fork...
[fork] parent PID 9 → child PID 10 (2 segs, sp=10011b10)
fork_test: I am the PARENT, child PID = 10
```

Parent works correctly. Child spawns but doesn't print — faults in TLS region.

## Hard-Won Learnings

### 1. vspace_get_cap() does NOT see sel4runtime pages

The sel4runtime startup code (`_start` → `__sel4_start_c`) allocates and maps
pages for TLS, auxv, and the initial stack frame. These pages are NOT tracked
by the sel4utils vspace manager — `vspace_get_cap()` returns `seL4_CapNull` for
addresses in the 0x10050000+ region.

**Evidence:**
```
[fork] page 10050000: parent_cap=0 child_cap=0
[fork] page 10051000: parent_cap=0 child_cap=0
...all zero from 10050000 to 10058000
```

**Implication:** Cannot fork by probing and copying all parent pages. The sel4runtime
memory region is invisible to the vspace API.

### 2. Frame caps belong to their VSpace

`vspace_get_cap()` returns a cap that is already mapped into the parent's page table.
seL4 will NOT allow mapping the same cap into root's VSpace — you get:
```
ARMPageMap: Attempting to remap a frame that does not belong to the passed address space
```

**Fix:** Must `seL4_CNode_Copy()` the frame cap first, then map the copy into root
for reading. Clean up the copy after.

### 3. sel4utils_configure_process_custom allocates stack/IPC pages

When you create a child with `sel4utils_configure_process_custom()`, it allocates
stack pages and an IPC buffer page in the child's VSpace. Trying to
`vspace_reserve_range_at()` for the same addresses fails:
```
sel4utils_reserve_range_at_no_alloc: Range not available at 0x10011000
```

**Fix:** Use `fork_copy_into_existing()` which detects pre-existing child pages
and copies content into them rather than allocating new ones.

### 4. spawn with resume=0 means sel4runtime never runs

If you call `sel4utils_spawn_process_v(proc, ..., 0 /* don't resume */)`, the
child's `_start` never executes. This means:
- tpidr_el0 = 0 (TLS never initialized)
- IPC buffer pointer in TLS = 0
- No environ, no auxv processing

Any attempt to use seL4 IPC from the child crashes because the IPC buffer
address is read from TLS (via tpidr_el0).

### 5. spawn with resume=1 but tpidr still 0

Even with `resume=1` and a brief delay before suspend, `ReadRegisters` shows
`tpidr=0`. The sel4runtime `_start` code calls `seL4_SetTLSBase()` which is a
*separate syscall* that sets tpidr_el0 in the kernel TCB — it doesn't show up
in ReadRegisters until after the TCB has been scheduled and the syscall completes.

The timing window between spawn and suspend may not be enough for `_start` to
complete. Or the suspend races with `_start`.

### 6. CNode slots already occupied after spawn

After `sel4utils_spawn_process_v()`, the child's CSpace slots 8-12 are already
populated by the spawn's argv parsing. Trying to `seL4_CNode_Copy()` into those
slots fails:
```
CNode Copy/Mint/Move/Mutate: Destination not empty.
```

**Fix:** Either delete the slots before copying, or don't try to copy caps after
spawn — pass the correct argv with the right slot numbers to spawn instead.

### 7. seL4_TCB_Configure signature (non-MCS)

```c
seL4_TCB_Configure(tcb, fault_ep, cspace_root, cspace_root_data,
                   vspace_root, vspace_root_data, buffer, bufferFrame)
```

Not `seL4_TCB_Configure(tcb, cspace_root, guard, vspace_root, buffer, frame)`.
The fault_ep is the second parameter.

### 8. CSpace guard API

Use `seL4_CapData_Guard_new()`, NOT `seL4_CNode_Guard_new()`.

### 9. fork_test ELF memory map

```
PT_LOAD: vaddr=0x400000..0x420930  memsz=0x20930  flags=5 (R-X, .text)
PT_LOAD: vaddr=0x421f80..0x54b288  memsz=0x129308 flags=6 (RW-, .data+.bss)
PT_TLS:  vaddr=0x421f80..0x421f8c  memsz=0xc      flags=4 (TLS template)
```

The .data segment is ~1.2MB (includes musl's static buffers).
tpidr=0x54b1d8 is inside .data (0x421f80..0x54b288).

### 10. seL4_DebugPutChar doesn't work from child

Even raw `seL4_DebugPutChar()` produced no output from the child. This confirms
the child's TCB was never properly scheduled or its registers were wrong.

## Approaches Tried

### Approach 1: Empty VSpace + manual page mapping
- Configure process WITHOUT ELF load
- Share .text pages, copy .data pages, copy stack
- **Result:** sel4runtime region (0x10050000+) invisible to vspace_get_cap,
  child faults in TLS area

### Approach 2: Full ELF reload + overwrite .data + stack
- Load same ELF into child (gives proper .text, .data, sel4runtime setup)
- Spawn with resume=0 (don't run sel4runtime)
- Overwrite .data and stack from parent
- Set registers with parent's PC+4, x0=reply_msg, parent's tpidr
- **Result:** tpidr=0 because sel4runtime never ran. Child faults in TLS.

### Approach 3: Full ELF reload + spawn resume=1 + suspend + overwrite stack
- Load same ELF, spawn with resume=1 so sel4runtime runs
- Wait briefly, suspend child
- Copy parent's stack into child
- Set registers with parent's PC+4, x0=reply_msg, child's own tpidr
- **Result:** Cap slots already occupied (error 8). tpidr still 0 (race
  condition — sel4runtime didn't finish before suspend). Child faults in TLS.

## Next Approach: IPC-Based Fork

The fundamental problem: sel4runtime creates invisible state that we cannot
replicate from outside. The solution: **don't try to replicate it.**

### Concept: Two-Phase Fork via IPC

1. Parent calls `fork()` → sends PIPE_FORK to pipe_server
2. Pipe_server reads parent's ELF path, loads same ELF into new process
3. Child spawns normally (resume=1, full sel4runtime, real argv, real aios_init)
4. Child's `__wrap_main` calls `__real_main` which calls `fork()`
5. Child's `fork()` sends PIPE_FORK to pipe_server
6. Pipe_server recognizes child (by badge or a fork_pending flag)
7. Pipe_server replies 0 to child (fork returns 0)
8. Pipe_server replies child_pid to parent (fork returns child_pid)

### How it works:

Parent and child run the SAME code path:
```c
pid_t pid = fork();
if (pid == 0) {
    // child path
} else {
    // parent path
}
```

Parent calls fork(), blocks in PIPE_FORK IPC.
Pipe_server spawns child with same ELF + argv.
Child starts from main(), calls fork().
Pipe_server sees fork_pending, replies 0 to child, replies child_pid to parent.
Both continue from the line after fork().

### Limitation:
- Stack-local variables from BEFORE fork() are lost in child
  (child starts fresh from main)
- This is closer to `posix_spawn()` semantics than true `fork()`
- For the getty/shell use case, this is fine
- For true fork() (preserving all state), need to solve the TLS problem

### Alternative: Kernel-assisted fork
- Patch seL4 (or elfloader) to expose page table walk
- Or: patch sel4runtime to register TLS pages with vspace manager
- Both are invasive but would enable true fork

## Files Modified

- `src/aios_root.c` — EXEC_FORK/PIPE_FORK, do_fork(), helpers, segment tracking
- `src/lib/aios_posix.c` — aios_sys_clone(), PIPE_FORK label
- `src/apps/fork_test.c` — test program
- `include/aios/tty.h` — TTY labels (from earlier phase)
- `projects/aios/CMakeLists.txt` — fork_test added
- `docs/DESIGN_FORK.md` — design document

## Current Code State

The code has accumulated debug prints and multiple attempted approaches.
Before the next implementation round, should:
1. Strip debug prints
2. Clean up do_fork() to use the chosen approach
3. Consider whether IPC-based fork or true fork is the target


## Source Code Analysis (2026-04-05, session 2)

### sel4runtime — NO dynamic page allocation

From deps/sel4runtime/src/env.c:
- TLS is a static 16KB array: `static char static_tls[CONFIG_SEL4RUNTIME_STATIC_TLS]` (16384 bytes)
- Located in .data segment (0x421f80..0x54b288)
- `try_init_static_tls()` copies 12 bytes of TLS image into static_tls, sets tpidr
- tpidr (0x54b1d8) points INTO this .data region
- sel4runtime allocates ZERO pages at runtime

### sel4utils/process.c — stack/argv writes tracked

- `sel4utils_spawn_process_v` writes auxv, argv, argc to stack using `sel4utils_stack_write`
- `sel4utils_stack_write` uses `vspace_get_cap(target_vspace, ...)` — works correctly
- Stack pages ARE tracked by vspace manager
- IPC buffer page also tracked (allocated by `sel4utils_configure_thread_config`)

### Key CSpace layout (from process.c)

Slots 1-7 are pre-allocated by sel4utils:
- 1: CNODE_SLOT (CSpace root)
- 2: ENDPOINT_SLOT (fault EP)
- 3: PD_SLOT (page directory)
- 4: ASID_POOL_SLOT
- 5: TCB_SLOT
- 6: SCHED_CONTEXT_SLOT (skipped on non-MCS)
- 7: REPLY_SLOT (skipped on non-MCS)
- 8+: user caps (serial_ep, fs_ep, etc.)

This is why cap slots 8-12 are correct for our 5 endpoints.
After spawn, those slots ARE occupied — CNode_Copy fails with error 8 (dest not empty).
Must either: not copy (they're already there), or Delete first then Copy.

### The 0x10050000 Mystery

Parent and child both show vspace_get_cap=0 at 0x10050000-0x10058000.
Yet child faults at PC=0x10050420.

This means:
- The child's PC was set to 0x4086f0 (in .text)
- Child executed from 0x4086f0, then jumped to 0x10050420
- The jump came from a return address on the copied stack
- In the parent, 0x10050420 is mapped to something (otherwise parent would have faulted)
- But vspace_get_cap can't see it

Hypothesis: musl's internal bookkeeping (malloc arena, stdio buffers) uses
addresses in this range. musl's sys_mmap implementation in libsel4muslcsys
may allocate pages through a separate path that doesn't register with
the sel4utils vspace manager. This would explain why:
- Pages exist (parent uses them without faulting)
- vspace_get_cap returns 0 (not tracked by sel4utils)
- The child doesn't have them (fork can't copy what it can't see)

### Next Investigation Steps

1. **Find musl's mmap implementation in libsel4muslcsys**:
   Look for sys_mmap, sys_brk in deps/seL4_libs/libsel4muslcsys/
   These likely allocate pages WITHOUT going through the vspace manager

2. **Scan CSpace to find all frame caps**:
   Walk the entire parent CSpace (slots 0-4095), call seL4_ARM_Page_GetAddress
   on each to find all mapped frames regardless of vspace tracking

3. **Alternative: Approach C (manual construction)**:
   Skip sel4utils entirely for the forked child. Manually:
   a. Create VSpace root (PGD)
   b. Walk parent's CSpace, for every frame cap: CNode_Copy + map into child
   c. Allocate fresh IPC buffer
   d. Create TCB, configure manually
   e. Copy registers from parent (PC+4, x0=0)
   f. Resume

4. **Alternative: Find and patch musl's mmap path**:
   If musl allocates via a separate vspace or raw seL4 calls, patch it
   to register with the sel4utils vspace manager. Then vspace_get_cap works.
