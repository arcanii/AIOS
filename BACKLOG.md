# AIOS BACKLOG

Items deferred out of HANDOVER's "What is pending" so the active table
stays focused. Each entry lists what it would ship, the rough size, and
the most relevant reference. Promote items back into HANDOVER when
they're up next.

---

## Next up -- recommended order (queued 2026-06-03)

Execution order set after the v0.4.143 pipe-EOF fix shipped: reliability
first, then a clean feature, then efficiency, then the high-risk/hardware
item. Intended to be `/schedule`-d as one-per-day sessions; recorded here so
the order survives regardless of the scheduler.

1. **Harden pipes under load -- INVESTIGATED 2026-06-03, DEFERRED (resource
   ceiling).** Root cause is NOT the pipe path: it is a **resource ceiling** --
   `MAX_ACTIVE_PROCS = 16` (root_shared.h, BSS-shift hazard to change), VKA pool
   8000 pages, morecore 6 MB/proc -> ~16 concurrent procs max. Under heavy
   concurrency the failures CASCADE: VKA/slot pressure -> PIPE_EXEC/do_fork fail
   (EPERM / "Cannot fork") -> a reader that fails to exec leaves the writer with
   no reader -> the writer's bytes are dropped (PIPE_WRITE `written<wlen`). 3+
   concurrent QEMU boots also overwhelm the host. These are largely artifacts of
   ARTIFICIAL multi-QEMU host-CPU contention; single-instance + real RPi4 work
   reliably. A secondary, genuine pipe-write **data-loss** bug exists (client
   advances `sent += chunk`, ignoring server `written`; 4096 ring drops overflow
   when the reader lags). A client busy-yield fix was tried (v0.4.144) and
   REVERTED -- it busy-spins on a full ring, adding pressure and deadlocking late
   readers. The only safe data-loss fix is server-side NON-spinning writer
   blocking (mirror pipe_read_blocked -> pipe_write_blocked, stash + resume on
   drain, EPIPE on read_closed) -- but it does NOT fix the load ceiling. A real
   "harden" needs capacity/admission work (swap, footprint reduction, careful
   limit raising) -- large, low payoff for real use. See docs/NEXT_20260603b.md.
   Repro: 2-3 concurrent QEMU `--smp 4` on separate disk copies (`--no-mirror`);
   an unclean QEMU kill corrupts `disk_ext2.img` -- regenerate via mkdisk.py.

2. **file-backed mmap** -- new POSIX VM feature; see the Medium-risk entry
   below. Self-contained, QEMU-testable, no hardware. ~300 LOC.

3. **COW Step 3 -- wc/shutdown post-promotion EPERM** -- efficiency win; see
   the Medium-risk entry below. One focused session, ~30 LOC + tracing.

4. **RPi4 SMP bring-up** -- re-enable SMP=4 (`settings-rpi4.cmake`
   `KernelMaxNumNodes` 1 -> 4). v0.4.135 fell back to single-core because the
   elfloader spin-table secondary-core bring-up hangs at the firmware->kernel
   handoff. First make the elfloader print on the RPi4 UART, then per-core
   boot trace. Highest risk, hardware-gated (needs the Pi + serial). See
   `docs/SEL4_DEVInvestigation.md` item E and `feedback_rpi4_boot`; also
   tracked in HANDOVER's active table.

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
