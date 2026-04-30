# AIOS Design: Copy-on-Write Fork

## Problem statement

Current `do_fork()` in `src/process/fork.c` copies parent's writable
pages eagerly (`fork_dup_region`). For each writable page:

1. Allocate a fresh frame (`vka_alloc_frame`)
2. CNode_Copy the parent's cap to root cspace
3. Map BOTH parent cap and new frame into root vspace temporarily
4. Memory-copy the page content (4096 bytes)
5. Unmap both
6. Delete the duplicated cap, free its slot
7. Reserve+map the new frame in the child's vspace

This is expensive in time AND memory:
- Time: 6+ seL4 syscalls + a 4 KB memcpy per page
- Memory: child gets full duplicate even if it just calls exec immediately

Most fork+exec patterns (`shell -> A | B`) discard the parent image
within microseconds. With v0.4.106-107 demand-paged BSS the BSS pages
aren't mapped at fork time (saves the bulk), but data + stack are
still eagerly duplicated.

COW eliminates this: at fork, both parent and child point to the same
frames mapped read-only. Pages are duplicated lazily on first write.

## Approach

### Core mechanism

1. **Fork time**: For each writable region, instead of copying:
   - Strip write permission from the parent's existing mapping
   - CNode_Copy the parent's frame cap into the child's CSpace
   - Map the copy in the child's vspace at the same vaddr,
     **read-only**

   Both parent and child now hold independent caps to the same
   physical frame, both read-only. Reading works fine; writing
   triggers VM fault.

2. **Write fault**: extend the existing fault handler (BSS path
   added in v0.4.106) with a new branch:
   - If fault_addr is in a COW range and the fault is a write fault
     (FSR write-bit set):
     - Allocate fresh frame
     - Map parent's cap into root temporarily, copy bytes
     - Unmap original COW cap from faulting vspace
     - Map fresh frame at same vaddr **read+write**
     - Decrement refcount on the old frame
     - If refcount hits zero, free the old frame
     - Reply (resume thread; faulting instruction retries)

3. **Process exit**: tear-down sees R/O caps and unmaps them. The
   underlying frames are reference-counted; they're freed only when
   the last user goes away.

### Tracking COW ranges

Per process, track a list of `[start, end)` ranges that are
COW-shared. On VM fault, the handler checks if the address falls in
any such range. Implementation options:

**Option A: array in active_proc_t** (chosen for prototype)
```c
#define MAX_COW_RANGES 8
typedef struct {
    uintptr_t start;
    uintptr_t end;
} cow_range_t;

/* in active_proc_t */
cow_range_t cow_ranges[MAX_COW_RANGES];
int num_cow_ranges;
```

ELF gives us a few LOAD segments so 8 ranges is plenty for the
typical case. Each fork merges ranges if contiguous.

**Option B: bitmap per page**
More precise (per-page state) but heavier. Defer until we observe
real fragmentation.

### Frame refcounting

When parent and child share a frame, each holds an independent cap
(via CNode_Copy). seL4 doesn't refcount frames automatically -- if
either process deletes its cap, the other still has access, but
the VKA's notion of "is this frame allocated?" is wrong.

For safety we maintain our own refcount table:

```c
#define MAX_TRACKED_FRAMES 8192
typedef struct {
    seL4_CPtr  frame_cap;   /* the original cap (parent's) */
    uintptr_t  paddr;       /* physical address */
    int        refcount;    /* parent + children using it */
} cow_frame_t;
```

On fork:
- For each parent frame in COW range:
  - Look up or insert in cow_frame[] table
  - refcount++

On write fault:
- Look up the frame
- If refcount == 1, just promote to writable in place (no copy needed)
- If refcount > 1, allocate fresh + copy + decrement

On process tear-down:
- For each COW page in this process:
  - Look up frame, refcount--
  - If refcount == 0, vka_free_object the frame and remove from table
  - Else just unmap from this vspace

This refcount table must be consistent across all forking. It lives
in root task memory, accessed only from root task threads (fork.c
and pipe_server.c), so no locks needed.

### Detecting write vs read fault

seL4 VMFault delivers a fault status register (FSR) which encodes
whether the access was a write. On AArch64:
- ESR_EL1 ISS field has the WnR bit
- seL4 puts FSR in MR `seL4_VMFault_FSR`
- WnR bit position depends on fault type; for data abort
  with translation fault, WnR is bit 6 of the lower ISS

Check: `(seL4_GetMR(seL4_VMFault_FSR) >> 6) & 1`

A read fault on a COW page should not happen if we mapped it
read-only correctly (read works on read-only mappings). If it does
happen, treat as a real fault (kill process).

### Mapping permissions

seL4 mapping rights are set at `vspace_map_pages_at_vaddr` time via
`seL4_CapRights_t`. Use:
- `seL4_CanRead` for COW (read-only)
- `seL4_AllRights` for promoted (read + write + execute)

Note: changing mapping rights after the fact requires
unmap + remap. We do this in the write-fault handler.

## Implementation phases

### Phase 1: minimal COW for fork()

- Add cow_ranges[] to active_proc_t
- Add cow_frame_t table in fork.c
- Modify fork_dup_region: instead of frame alloc + copy, do
  CNode_Copy + read-only remap; record range in cow_ranges
- Extend pipe_server fault handler:
  - Check VMFault_FSR for write bit
  - If write fault in cow_ranges: allocate, copy, remap writable,
    decrement refcount
- Test: fork + immediate write to data segment, observe single page
  copy (not whole region)

### Phase 2: integrate with BSS demand paging

- BSS faults already supported (v0.4.106-107)
- Handler now distinguishes:
  - Fault in bss_lazy range -> alloc + zero-map (no source frame)
  - Fault in cow_ranges -> alloc + copy from source frame
  - Else -> exit

### Phase 3: optimisations

- If refcount==1 at write fault, promote in place (no copy):
  unmap read-only, remap same frame writable
- Coalesce contiguous cow_ranges to reduce array entries
- Skip COW for pages we know will be discarded (e.g. when fork is
  followed by exec; but predicting this is hard)

### Phase 4 (optional): madvise/MADV_DONTNEED

- Tell kernel "I'm about to throw this away" -> can drop pages
- Useful for discard after fork+exec

## Estimated impact

For a `cmd args | other_cmd args` pipeline:
- Currently: shell forks twice. Each fork copies all writable pages
  eagerly. After exec, pages are immediately discarded.
- With COW: each fork shares R/O caps. exec destroys the (sharing)
  vspace before any writes happen -> 0 page copies.

Per-fork page allocations (writable regions):
- Stack: 4 pages
- Data: ~variable (depends on globals)
- BSS: 0 (already demand-paged)
- Total typical: ~10-50 pages saved per fork

For a 4-process pipeline with 3 forks: ~30-150 page-copies avoided
plus the 4KB memcpy per page (~120-600 KB of copying).

## Risk areas

1. **seL4 ARM64 fault encoding**. Need to verify FSR bit layout for
   read vs write distinction. Test with deliberate write fault on a
   read-only page.

2. **vspace API and read-only remap**. `vspace_map_pages_at_vaddr`
   takes rights -- but does it allow remapping the same vaddr? May
   need to unmap first. Test.

3. **Cap exhaustion**. Each fork creates one cap per shared page in
   child's CSpace. With many forks of large processes, child CSpace
   could fill. Use cnode size 16+ or trim CSpace per process.

4. **Race with exec**. If child write-faults while shell is doing
   PIPE_EXEC for it, the OLD vspace's COW pages must not be touched.
   PIPE_EXEC tears down vspace which calls our refcount path.

5. **Fork-in-fork**. If child forks before exec, grandchild needs to
   share with both parent and child. Refcount table handles this
   naturally (each frame's refcount can be >2).

6. **Multi-threaded**. If child has multiple threads sharing the
   same vspace and they all write the same COW page, only one needs
   to copy. Others should see the new mapping after the first
   resolves. Currently AIOS doesn't fork multi-threaded processes
   so this is theoretical.

## What we keep from the existing code

- `fork_share_region` (read-only sharing for .text/.rodata) is
  unchanged -- COW for *writable* regions extends this pattern.
- The existing fault handler in pipe_server.c is extended in place;
  no new endpoint, no new dispatcher.
- VKA_SUB_FORK accounting still tracks fork-time frame allocations
  (now zero in the common case).

## Estimated effort

Phase 1: 1-2 sessions
- ~150 LOC in fork.c (new cow_dup_region replacing fork_dup_region)
- ~80 LOC in pipe_server.c fault handler
- ~50 LOC for cow_frame_t table operations
- Test: fork + write, observe COW behaviour

Phase 2: integrate with BSS, ~30 LOC. Mostly conditional checks.

Phase 3: optimizations, ~50-100 LOC. Optional.

Total realistic: 2-3 focused sessions for a working v0.5-class
COW fork implementation.

## References

- `src/process/fork.c`: current eager-copy fork
- `src/servers/pipe_server.c`: existing BSS fault handler (the
  extension point)
- `docs/DESIGN_DEMAND_BSS.md`: companion document for BSS faults
- seL4 manual: VM faults, FSR encoding, cap rights
- libsel4vspace `vspace_map_pages_at_vaddr_with_config` for
  setting per-mapping rights
