# AIOS Design: Demand-Paged BSS

## Problem statement

Each child process allocates ~1536 pages eagerly at ELF load:
- 6 MB static `morecore_area` BSS = 1536 × 4 KB pages
- These pages are zero-filled and mapped into child VSpace by
  `sel4utils_elf_load`
- Most simple programs (`ls`, `cat`, `echo`) use < 100 KB of heap, so
  ~1500 pages are wasted per process

For pipelines with N processes, BSS alone consumes N × 1536 pages.
A 4-process pipeline (`shell | A | B | C`) needs ~6000 pages just for
empty BSS, exceeding the 8000-page pool when combined with boot
allocations (~4000 pages).

The fix: don't allocate BSS frames until the program actually touches
them. This is "demand paging" -- standard in real OSes.

## Two approaches considered

### Approach A: post-load unmap (chosen)

Pros:
- Reuses existing `sel4utils_elf_load` -- no deps patching
- Smaller code change (just adds fault handler + ELF range parsing)
- Lower risk

Cons:
- Wastes brief allocation during load (immediately freed)
- Frames are allocated AND released; minor performance cost

### Approach B: custom ELF loader

Pros:
- No allocation/free waste; frames never allocated
- Cleaner semantics (truly lazy from load time)

Cons:
- Replicate sel4utils_elf_load logic in our code
- Larger code change
- More edge cases (cap rights, segment overlap, file-data copy)

We choose **Approach A** for its smaller blast radius. Approach B can
follow if A's perf cost matters in practice.

## Design

### Data structures

Add to `active_proc_t` in `include/aios/root_shared.h`:

```c
/* v0.4.105: BSS lazy mapping range */
uintptr_t bss_lazy_start;   /* page-aligned, inclusive */
uintptr_t bss_lazy_end;     /* page-aligned, exclusive */
```

Set during exec_server after ELF load. Cleared on process exit.

### ELF parsing

After `sel4utils_elf_load` returns successfully, parse the ELF program
headers to find the BSS portion:

```c
static void identify_bss(elf_t *elf, uintptr_t *out_start, uintptr_t *out_end) {
    int n = elf_getNumProgramHeaders(elf);
    uintptr_t bss_start = 0;
    uintptr_t bss_end = 0;
    for (int i = 0; i < n; i++) {
        if (elf_getProgramHeaderType(elf, i) != PT_LOAD) continue;
        uintptr_t vaddr  = elf_getProgramHeaderVaddr(elf, i);
        size_t    filesz = elf_getProgramHeaderFileSize(elf, i);
        size_t    memsz  = elf_getProgramHeaderMemorySize(elf, i);
        if (memsz <= filesz) continue;
        /* BSS portion: from end of file data to end of memory size */
        uintptr_t s = ROUND_UP(vaddr + filesz, PAGE_SIZE_4K);
        uintptr_t e = ROUND_UP(vaddr + memsz, PAGE_SIZE_4K);
        if (e <= s) continue;
        /* Track largest BSS region (typically there's one big one) */
        if ((e - s) > (bss_end - bss_start)) {
            bss_start = s;
            bss_end = e;
        }
    }
    *out_start = bss_start;
    *out_end   = bss_end;
}
```

Stored in `ap->bss_lazy_start/end` in `exec_server.c`.

### Unmapping (post-load)

Immediately after `sel4utils_elf_load`, unmap the BSS pages:

```c
size_t pages = (ap->bss_lazy_end - ap->bss_lazy_start) / PAGE_SIZE_4K;
vspace_unmap_pages(&proc->vspace, (void *)ap->bss_lazy_start,
                   pages, seL4_PageBits, &vka);
```

This returns the frames to the VKA pool. The vspace reservation
remains so subsequent map operations work.

### Fault handler

Currently, `exec_thread` does:

```c
seL4_Recv(child_fault_ep.cptr, &child_badge);
/* always treat as exit */
```

Replace with classification loop:

```c
while (1) {
    seL4_MessageInfo_t msg = seL4_Recv(child_fault_ep.cptr, &child_badge);
    seL4_Word label = seL4_MessageInfo_get_label(msg);

    /* VM fault? */
    if (label == seL4_Fault_VMFault) {
        seL4_Word fault_addr = seL4_GetMR(seL4_VMFault_Addr);
        seL4_Word ip         = seL4_GetMR(seL4_VMFault_IP);

        if (fault_addr >= ap->bss_lazy_start && fault_addr < ap->bss_lazy_end) {
            /* BSS fault: allocate page, map, resume */
            if (handle_bss_fault(ap, fault_addr) == 0) {
                /* Resume by replying (empty) to the fault EP */
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                continue;
            }
            /* Fall through: allocation failed -> kill process */
        }
        /* Real VM fault outside BSS: log + treat as exit */
        AIOS_LOG_ERROR_V("VM fault outside BSS at addr=", (unsigned long)fault_addr);
    }
    /* Default: exit cleanup (existing path) */
    break;
}
```

### `handle_bss_fault`

```c
static int handle_bss_fault(active_proc_t *ap, uintptr_t fault_addr) {
    uintptr_t page = fault_addr & ~(PAGE_SIZE_4K - 1);
    /* Headroom check */
    if (vka_audit_check_headroom(1) < 0) return -1;
    /* Allocate frame in our vka, map in child vspace */
    void *vaddr = vspace_new_pages_at_vaddr(&ap->proc.vspace,
        (void *)page, 1, seL4_PageBits, ap->bss_reservation);
    if (!vaddr) {
        AIOS_LOG_ERROR("BSS fault: vspace_new_pages_at_vaddr failed");
        return -1;
    }
    /* Page is zero-filled by VKA; ARM64 frame allocator zeroes by default */
    vka_audit_frame(VKA_SUB_OTHER, 1);
    AIOS_LOG_DEBUG_V("BSS fault filled at vaddr=", (unsigned long)page);
    return 0;
}
```

Note: requires storing a `reservation_t` for the BSS range. Set during
unmap step using `vspace_reserve_range_at()` first if needed (the
existing reservation from sel4utils may be sufficient).

### Cleanup

On process exit, `sel4utils_destroy_process` walks the vspace and frees
all mapped frames. Demand-paged BSS pages are mapped (those that were
faulted in) -- they get freed normally. No special handling needed.

## Implementation phases

### Phase 1 (one focused session): proof of concept
- Add `bss_lazy_start/end` fields
- Parse ELF, store BSS range
- Unmap BSS pages after load
- Add fault classifier in exec_thread
- Test with simple program

### Phase 2: integrate everywhere
- Same logic in `fork.c` (forked children also get demand-paged BSS)
- Update `reap.c` for fork+exec faults

### Phase 3: optimization
- Skip unmap+remap for very large BSS (allocate via custom loader)
- Track per-process BSS page count for accounting

## Risk areas

1. **VMFault label encoding on AArch64**. seL4_Fault_VMFault is the
   right tag but its numeric value is platform-dependent. Verify
   experimentally.

2. **Reservation handling**. After `vspace_unmap_pages`, the
   reservation may still be valid (vspace tracks it independently).
   We need `vspace_new_pages_at_vaddr` to succeed inside the existing
   reservation. Test this.

3. **Race conditions**. If the child triggers multiple fast faults
   while exec_thread handles one, requests queue on the fault EP.
   Reply to one resumes that thread; subsequent Recv gets the next.
   Should be safe with seL4 endpoints.

4. **Forked child fault EPs**. Forked children have minted fault EPs
   pointing to a different endpoint. The fault delivery path needs
   verification for fork+exec.

5. **Threading**. If a child has multiple threads (via thread_server),
   they share the same fault EP. Handler must serialize page-mapping.

## Expected outcomes

After Phase 1:
- Per-process page count drops from ~1536 to ~10-30 for simple programs
- 4-process pipelines fit easily in 8000-page pool
- Heavy programs (TCC, zsh) use mmap IPC for explicit big allocations
  (already working in v0.4.104)

## References

- seL4 manual: VM faults and reply caps
- libsel4utils/src/elf.c: ELF loader internals
- libsel4utils/src/vspace.c: vspace_unmap_pages / new_pages_at_vaddr
- AIOS v0.4.104: PIPE_MMAP_ANON IPC for explicit anonymous mmap
