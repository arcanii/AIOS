# AIOS BACKLOG

Items deferred out of HANDOVER's "What is pending" so the active table
stays focused. Each entry lists what it would ship, the rough size, and
the most relevant reference. Promote items back into HANDOVER when
they're up next.

---

## Medium-risk

### COW Step 3 -- wc/shutdown post-promotion EPERM
- **What ships**: enables `COW_STRIP_PARENT 1`. With strip on, dash forks
  for `wc`/`shutdown` post-promotion fail with EPERM. Mechanism is
  proven (parent_promotions counts, no kernel errors); a downstream
  state divergence kills subsequent fork+exec.
- **Repro**: flip the gate in [src/process/cow.c:44](src/process/cow.c).
- **Plan**: instrument `do_fork`'s 12 `return -1` paths to find which
  fires post-promotion. Most likely culprits: cap allocation interacting
  with the orphaned parent_cap, or a child cspace cap copy that ends
  up wrong.
- **Size**: ~30 LOC of fix on top of the diagnostic; one focused session.
- **See**: [docs/NEXT_20260503a.md](docs/NEXT_20260503a.md).

### Block cache write-back
- **What ships**: switch from write-through to write-back, with periodic
  flush. AIOS fs traffic is currently too low to make this measurable.
- **Size**: ~150 LOC.

### file-backed mmap
- **What ships**: `MAP_SHARED` on a regular file. Extends `PIPE_MMAP_ANON`
  with a file path + offset, fs_server reads the page into a fresh frame,
  caller maps it. `msync` for write-back is the hard bit.
- **Size**: ~300 LOC.

### COW Step 4 -- stack COW
- **What ships**: probe parent's stack tightly, share via `cow_setup_segment`.
  Previous attempt (NEXT_20260502b) collided with child's IPC buffer;
  bound the probe to the actual stack range.
- **Size**: ~200 LOC. Depends on Step 3 working in production.

### COW Step 5 -- parent-dies safety
- **What ships**: today, child holds R/O dups of parent's frames; if parent
  dies and `vspace_tear_down` frees the underlying frames, child caps
  dangle. Needs cookie-ownership transfer at fork time (or refcount-driven
  free in `cow_frame_release`).
- **Size**: uncertain, touches sel4utils internals.

---

## High-risk

### Server health probes -- full (with auto-restart)
- **What ships**: extends v0.4.121 ping probe with restart on stale
  age. Detecting death is easy; restoring server state across restart
  is the hard part (BSS-resident state, in-flight reply caps,
  registered clients).
- **Size**: ~400 LOC.

---

## Long-term research

### Swap / paging out
- **What ships**: anonymous-page eviction to disk + page-in on fault.
  Needs a swap area, an LRU policy across active_procs vspaces, and
  fault-handler integration.

---

## Tooling polish (small but deferred)

### Smoke-driver flakiness
- The python smoke driver occasionally fails to reach the dash prompt
  on first run (timing race with QEMU + getty + login). Workaround:
  retry once. Worth investigating with explicit prompt polling rather
  than fixed sleeps.

### SIGSEGV / fault-observation harness
- Would let us actually verify `mprotect(PROT_NONE)` faults reads, and
  `mprotect(R/X)` clears XN. Today the IPC return is real but the user
  has no way to observe the page-fault outcome.
