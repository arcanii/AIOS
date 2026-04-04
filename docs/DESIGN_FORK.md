# AIOS fork() Design

## Executive Summary

Implement POSIX `fork()` on seL4 by duplicating a process's VSpace, CSpace, TCB state,
and POSIX shim state. The child receives a new VSpace with shared .text pages (read-only)
and copied .data/.bss/heap/stack pages (copy-on-write is deferred to v0.5.x; initial
implementation does eager copy). The fork is orchestrated by the process server in
aios_root via a new `EXEC_FORK` IPC label.

## Why fork() Matters

| Use case | Requires fork? | Alternative |
|---|---|---|
| Shell subshells `$(cmd)` | Yes | None |
| `system()` / popen() | Yes | None |
| getty → exec shell | exec-replace possible, fork is cleaner |
| sbase `time` command | Yes — times a child | None |
| sbase `flock` command | Yes — runs child with lock held | None |
| Future: sshd + pty | Yes — standard daemon pattern | posix_spawn partial |
| Future: shell scripting | Yes — pipes, backgrounding | Hack-only |

## seL4 Constraints

seL4 has no kernel fork primitive. We must implement it entirely in userspace:

1. **No COW support in seL4 MMU fault handling** — seL4 delivers VM faults to a
   fault handler thread, but the handler cannot remap pages atomically. Initial
   implementation uses **eager copy** (duplicate all writable pages at fork time).

2. **VSpace is a page table tree** — we can walk it via `vspace_get_cap()` for any
   mapped virtual address.

3. **CSpace is a capability tree** — we can copy caps via `seL4_CNode_Copy`.

4. **TCB state** — `seL4_TCB_ReadRegisters` / `seL4_TCB_WriteRegisters` gives full
   register dump including PC, SP, LR, and all GPRs.

5. **No shared memory side effects** — a fork'd child gets independent copies of
   writable pages. Writes in parent don't affect child and vice versa.

## Architecture

```
Parent process                    Process Server (aios_root)
     │                                    │
     ├─ syscall(SYS_clone) ──────────────►│
     │  (via aios_posix.c shim)           │
     │  seL4_Call(exec_ep,                │
     │    EXEC_FORK)                      │
     │                                    │
     │                         ┌──────────┴──────────┐
     │                         │ 1. Create new proc  │
     │                         │    - VSpace          │
     │                         │    - CSpace (12-bit) │
     │                         │    - TCB             │
     │                         │    - fault EP        │
     │                         │                      │
     │                         │ 2. Share .text pages │
     │                         │    (read-only, same  │
     │                         │     physical frames) │
     │                         │                      │
     │                         │ 3. Copy .data/.bss   │
     │                         │    heap, stack pages  │
     │                         │    (new frames, copy  │
     │                         │     content via root  │
     │                         │     temp mapping)     │
     │                         │                      │
     │                         │ 4. Copy CSpace caps  │
     │                         │    (serial, fs, auth, │
     │                         │     pipe, thread eps) │
     │                         │                      │
     │                         │ 5. ReadRegisters     │
     │                         │    from parent TCB   │
     │                         │                      │
     │                         │ 6. WriteRegisters    │
     │                         │    to child TCB      │
     │                         │    (x0 = 0 for child)│
     │                         │                      │
     │                         │ 7. Resume child      │
     │                         └──────────┬──────────┘
     │                                    │
     ◄─ reply: MR0 = child_pid ──────────┤
     │  (parent gets child PID)           │
     │                                    │
     │         Child process              │
     │              │                     │
     │              ◄─ resumes with x0=0 ─┘
     │              │  (child gets 0)
     │              │
```

## Memory Map to Duplicate

A typical AIOS process has this layout (from DESIGN_0.4.md):

```
0x0000_0000_0040_0000  .text   (ELF PT_LOAD, R-X)  → SHARE (same frames)
0x0000_0000_0050_0000  .data   (ELF PT_LOAD, RW-)  → COPY
0x0000_0000_005x_xxxx  .bss    (zeroed, RW-)       → COPY
0x0000_0000_006x_xxxx  heap    (brk/mmap, RW-)     → COPY
        ...
0x0000_0000_7FFF_0000  stack   (grows down, RW-)   → COPY
0x0000_0000_7FFF_F000  IPC buffer (1 page, RW-)    → NEW (child gets own)
```

### How We Know What's Mapped

Two sources of truth:

1. **ELF regions** — `proc->elf_phdrs` (stored by sel4utils at load time) tells us
   where .text and .data segments are, their sizes, and their permissions.

2. **vspace_get_cap()** — for any virtual address, returns the frame cap if mapped,
   or `seL4_CapNull` if not. We can probe address ranges to find mapped pages.

### Strategy

For each ELF PT_LOAD segment:
- If **executable** (PF_X): **share** — same physical frames, read-only in child
- If **writable** (PF_W): **copy** — allocate new frames, copy content

For stack and IPC buffer:
- **Stack**: copy all mapped stack pages
- **IPC buffer**: allocate new (child needs its own IPC buffer)

For heap (brk region):
- Tracked by musllibc/posix shim. Pass heap top via register or IPC.

## Process Server Implementation

### New IPC Label

```c
#define EXEC_FORK  25   /* Fork calling process */
```

### Fork Handler (in exec_thread)

```c
case EXEC_FORK: {
    /* Badge identifies the calling process */
    int parent_idx = (int)badge - 1;
    proc_entry_t *parent = &active_procs[parent_idx];
    sel4utils_process_t *pp = &parent->proc;

    /* 1. Allocate child resources */
    vka_object_t child_fault_ep;
    vka_alloc_endpoint(&vka, &child_fault_ep);

    sel4utils_process_config_t cfg = process_config_new(&simple);
    cfg = process_config_create_cnode(cfg, 12);
    cfg = process_config_create_vspace(cfg, NULL, 0);
    cfg = process_config_priority(cfg, 200);
    cfg = process_config_auth(cfg, simple_get_tcb(&simple));
    cfg = process_config_fault_endpoint(cfg, child_fault_ep);

    sel4utils_process_t child;
    sel4utils_configure_process_custom(&child, &vka, &vspace, cfg);

    /* 2. Walk parent's ELF regions and duplicate */
    for (int r = 0; r < pp->num_elf_phdrs; r++) {
        Elf_Phdr *ph = &pp->elf_phdrs[r];
        if (ph->p_type != PT_LOAD) continue;

        uintptr_t vaddr = ph->p_vaddr;
        size_t memsz = ph->p_memsz;
        int num_pages = (memsz + PAGE_SIZE - 1) / PAGE_SIZE;

        if (ph->p_flags & PF_X) {
            /* Executable segment: share read-only */
            reservation_t res = vspace_reserve_range_at(
                &child.vspace, (void *)vaddr, num_pages * PAGE_SIZE,
                seL4_CanRead, 1);
            vspace_share_mem_at_vaddr(&pp->vspace, &child.vspace,
                (void *)vaddr, num_pages, seL4_PageBits,
                (void *)vaddr, res);
        } else {
            /* Writable segment: eager copy */
            fork_copy_pages(&pp->vspace, &child.vspace,
                           vaddr, num_pages);
        }
    }

    /* 3. Copy stack pages */
    fork_copy_stack(parent, &child);

    /* 4. Allocate new IPC buffer for child */
    /* (handled by sel4utils_configure_process_custom) */

    /* 5. Copy endpoint caps to child CSpace */
    sel4utils_copy_cap_to_process(&child, &vka, serial_ep.cptr);
    /* ... fs_ep, auth_ep, pipe_ep, thread_ep (minted with child badge) */

    /* 6. Copy registers, set child return value = 0 */
    seL4_UserContext regs;
    seL4_TCB_ReadRegisters(pp->thread.tcb.cptr, 0, 0,
                           sizeof(regs)/sizeof(seL4_Word), &regs);
    regs.x0 = 0;  /* child sees fork() return 0 */
    seL4_TCB_WriteRegisters(child.thread.tcb.cptr, 0, 0,
                            sizeof(regs)/sizeof(seL4_Word), &regs);

    /* 7. Register child in proc table */
    int child_idx = proc_add_fork(parent, &child);

    /* 8. Resume child */
    seL4_TCB_Resume(child.thread.tcb.cptr);

    /* 9. Reply to parent with child PID */
    seL4_SetMR(0, (seL4_Word)child_pid);
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
    break;
}
```

### fork_copy_pages Helper

```c
/*
 * Duplicate writable pages from parent to child VSpace.
 * For each page: allocate new frame, map into root temporarily,
 * copy content, unmap from root, map into child at same vaddr.
 */
static int fork_copy_pages(vspace_t *parent_vs, vspace_t *child_vs,
                           uintptr_t start, int num_pages) {
    for (int i = 0; i < num_pages; i++) {
        void *vaddr = (void *)(start + i * PAGE_SIZE);

        /* Get parent's frame cap at this address */
        seL4_CPtr parent_cap = vspace_get_cap(parent_vs, vaddr);
        if (parent_cap == seL4_CapNull) continue;  /* unmapped page, skip */

        /* Allocate new frame for child */
        vka_object_t new_frame;
        vka_alloc_frame(&vka, seL4_PageBits, &new_frame);

        /* Map both into root's VSpace temporarily for copy */
        void *parent_tmp = vspace_map_pages(&vspace, &parent_cap, NULL,
            seL4_AllRights, 1, seL4_PageBits, 1);
        void *child_tmp = vspace_map_pages(&vspace, &new_frame.cptr, NULL,
            seL4_AllRights, 1, seL4_PageBits, 1);

        /* Copy page content */
        memcpy(child_tmp, parent_tmp, PAGE_SIZE);

        /* Unmap from root */
        vspace_unmap_pages(&vspace, parent_tmp, 1, seL4_PageBits, NULL);
        vspace_unmap_pages(&vspace, child_tmp, 1, seL4_PageBits, NULL);

        /* Reserve + map into child at same vaddr */
        reservation_t res = vspace_reserve_range_at(child_vs, vaddr,
            PAGE_SIZE, seL4_AllRights, 1);
        vspace_map_pages_at_vaddr(child_vs, &new_frame.cptr, NULL,
            vaddr, 1, seL4_PageBits, res);
    }
    return 0;
}
```

## Stack Duplication

The stack is the trickiest part. We don't track stack pages explicitly —
musllibc sets up the stack during `sel4utils_spawn_process_v`. We need to
know:
- Stack top (from registers: SP)
- Stack bottom (allocated at process creation, typically 16 pages = 64KB)

### Approach

1. Read parent's SP from registers
2. The stack region is at a known range (configured during process setup).
   sel4utils allocates stack pages and records them.
3. Probe downward from stack top with `vspace_get_cap()` to find all
   mapped stack pages.
4. Copy each mapped stack page to the child.

```c
static int fork_copy_stack(proc_entry_t *parent, sel4utils_process_t *child) {
    seL4_UserContext regs;
    seL4_TCB_ReadRegisters(parent->proc.thread.tcb.cptr, 0, 0,
                           sizeof(regs)/sizeof(seL4_Word), &regs);
    uintptr_t sp = regs.sp;

    /* Stack grows down. Probe from SP page downward. */
    uintptr_t page = sp & ~(PAGE_SIZE - 1);

    /* Also probe upward to stack top (IPC buffer is above stack) */
    for (uintptr_t probe = page; probe < page + 16 * PAGE_SIZE; probe += PAGE_SIZE) {
        seL4_CPtr cap = vspace_get_cap(&parent->proc.vspace, (void *)probe);
        if (cap == seL4_CapNull) break;
        fork_copy_pages(&parent->proc.vspace, &child->vspace, probe, 1);
    }

    /* Probe downward */
    for (uintptr_t probe = page - PAGE_SIZE; ; probe -= PAGE_SIZE) {
        seL4_CPtr cap = vspace_get_cap(&parent->proc.vspace, (void *)probe);
        if (cap == seL4_CapNull) break;
        fork_copy_pages(&parent->proc.vspace, &child->vspace, probe, 1);
    }

    return 0;
}
```

## POSIX Shim Changes (aios_posix.c)

### fork() Implementation

```c
static long aios_sys_clone(va_list ap) {
    unsigned long flags = va_arg(ap, unsigned long);

    /* Only handle basic fork (flags == SIGCHLD) */
    if (flags != SIGCHLD && flags != 0) {
        return -ENOSYS;  /* clone with threads/namespaces not supported */
    }

    /* Send EXEC_FORK to process server */
    seL4_MessageInfo_t reply = seL4_Call(exec_ep,
        seL4_MessageInfo_new(EXEC_FORK, 0, 0, 0));

    /* MR0 = child PID (parent) or 0 (child) */
    long result = (long)seL4_GetMR(0);

    return result;
}
```

Register in aios_init:
```c
muslcsys_install_syscall(__NR_clone, aios_sys_clone);
```

### wait()/waitpid() Implementation

```c
#define EXEC_WAIT 26

static long aios_sys_wait4(va_list ap) {
    int pid = va_arg(ap, int);
    int *status = va_arg(ap, int *);
    int options = va_arg(ap, int);

    /* Send EXEC_WAIT to process server */
    seL4_SetMR(0, (seL4_Word)pid);
    seL4_SetMR(1, (seL4_Word)options);
    seL4_MessageInfo_t reply = seL4_Call(exec_ep,
        seL4_MessageInfo_new(EXEC_WAIT, 0, 0, 2));

    int child_pid = (int)seL4_GetMR(0);
    int exit_status = (int)seL4_GetMR(1);

    if (status) *status = (exit_status & 0xFF) << 8;
    return child_pid;
}
```

## POSIX Shim State in Child

After fork, the child's POSIX shim globals (ser_ep, fs_ep_cap, etc.) contain
the *parent's* cap slot numbers. But the child has its own CSpace with caps
copied to potentially different slot numbers.

### Solution: Cap Slot Convention

Ensure child's caps are at the **same slot numbers** as parent's. This works
because:
1. Process server copies caps using `sel4utils_copy_cap_to_process` which
   allocates the next free slot
2. If we copy caps in the same order for both parent and child, and both
   start with CSpace slot 8 (SEL4UTILS_FIRST_FREE), slots will match

Alternatively: after fork, the child calls a `FORK_FIXUP` IPC to get its
correct cap slot numbers and reinitializes the shim. This is safer.

## waitpid() in Process Server

The process server needs to:
1. Track parent-child relationships (ppid in proc_entry_t)
2. When child exits (VM fault), store exit code in proc table, mark ZOMBIE
3. When parent calls EXEC_WAIT, check for zombie children:
   - If zombie child exists: return pid + status, free resources
   - If no zombie but live children: block (SaveCaller, wait for child exit)
   - If no children at all: return -ECHILD

## Phased Implementation

### Phase 1: Minimal fork — ELF segments only

- Implement EXEC_FORK handler
- Share .text, copy .data/.bss
- Copy stack via probing
- Copy registers (x0=0 for child)
- Copy endpoint caps (same slots)
- Child runs, can print, can exit
- No wait() yet — parent gets PID, child is fire-and-forget

**Test**: `fork_test.c` that forks and child prints "I am child"

### Phase 2: wait()/waitpid()

- Add EXEC_WAIT handler with zombie tracking
- Parent blocks until child exits
- Return exit status
- `sbase/time` should work

### Phase 3: exec() after fork

- New `EXEC_REPLACE` label: replace current process image with new ELF
- This is fork+exec pattern
- Getty can now fork, child execs shell

### Phase 4: Pipe + fork integration

- fork inherits pipe file descriptors
- `popen()` works: fork, pipe, exec
- Shell `$(command)` substitution

## Risk Assessment

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| vspace_get_cap returns NULL for mapped pages | Medium | Can't copy | Probe with known ranges from ELF phdrs |
| Stack size unknown | Medium | Partial copy | Probe both directions from SP |
| Heap pages not tracked | Medium | Child crashes | Track brk high-water in shim, pass via IPC |
| Cap slot mismatch in child | High | Child can't do IPC | Use FORK_FIXUP IPC or ensure same copy order |
| Large process = slow fork | Low | Performance | Eager copy is O(pages), COW deferred to v0.5.x |
| Reply cap management | Medium | Deadlock | SaveCaller pattern per LEARNINGS.md |
| Child inherits parent's blocked state | Low | Confusion | Fork only from running (not blocked) state |

## Data Structure Changes

### proc_entry_t additions

```c
typedef struct {
    /* existing fields... */
    int ppid;                  /* parent process index */
    int exit_code;             /* set on exit, read by wait() */
    int state;                 /* PROC_FREE, PROC_ALIVE, PROC_ZOMBIE */
    seL4_CPtr wait_reply;      /* saved reply cap for blocking wait() */
    int has_waiter;            /* parent is blocked in wait() */
} proc_entry_t;
```

### IPC Labels

```c
#define EXEC_FORK    25   /* Fork calling process */
#define EXEC_WAIT    26   /* Wait for child exit */
#define EXEC_REPLACE 27   /* Replace process image (exec) */
```

## Testing Plan

1. **fork_test**: fork, child prints, parent prints child PID
2. **fork_wait_test**: fork, child exits, parent waits, gets status
3. **fork_exec_test**: fork, child execs `echo hello`
4. **fork_pipe_test**: fork with pipe, parent writes, child reads
5. **time command**: sbase `time` works (fork + wait + timing)
6. **flock command**: sbase `flock` works (fork + exec with lock)
7. **stress test**: fork 10 times, all children run, parent waits all
