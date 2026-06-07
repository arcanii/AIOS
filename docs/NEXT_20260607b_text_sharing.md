# NEXT 2026-06-07b -- Phase 2: share read-only .text across same-binary procs

Seed for a focused session. The previous arc (SMP + process capacity + ELF
demand-text) is DONE and HW-verified through **v0.4.182**. This is the next
footprint lever. Read `HANDOVER.md` + the `project_proc_capacity` memory first.

## TL;DR -- the one thing to build
The root keeps a per-boot cache `{binary path -> its read-only .text frames,
loaded + I-cache-unified ONCE}`. At `exec`, instead of giving each process its
OWN copy of the code (eager load, or v0.4.181 demand-text), MAP THE SHARED FRAMES
read-only into the child at the .text vaddr. 8 concurrent `seq` -> **one** ~75-page
copy of seq's code, not 8. Saves the dominant per-proc cost (RAM/frames), which
lifts the concurrent-process ceiling further (the table is already the limit on
RPi4's 4 GB; this lets it go much higher).

## Why this, and why now
- Per-proc RESIDENT footprint is the wall above the table (morecore is ALREADY
  demand-paged: `BSS lazy pages=1580`; the CNode is negligible). v0.4.181
  demand-text cut it to "executed code only" -- but every proc still has its OWN
  copy. Same-binary procs (a pipeline storm of `seq`/`wc`) duplicate identical
  read-only code N times.
- Sharing collapses that to ONE physical copy. Read-only code is the ideal thing
  to share (immutable, no COW needed).

## Design sketch
1. **`text_cache[]`** in the root (file-local in `pipe_server.c`, near
   `file_vmas`): each entry = `{ char path[128]; uintptr_t vaddr; int npages;
   vka_object_t frames[npages]  (or a small cap array); int refcount; }`.
   Size it for the distinct binaries in flight (~16-32 entries).
2. **At exec** (both `exec_server.c` AND the `pipe_server.c` pipe-exec path -- the
   two `sel4utils_elf_load` + demand-BSS sites, same as `setup_demand_text`):
   for the read-only (R+X) ELF segment:
   - Look up `path` in `text_cache`. **MISS:** allocate `npages` frames, fill each
     from the executable file (`vfs_pread`, like `handle_file_mmap_fault`), then
     `seL4_ARM_Page_Unify_Instruction` each ONCE (I-cache), store in the cache,
     refcount=0. **HIT:** reuse.
   - Map the cached frames READ-ONLY + executable into the child's vspace at the
     segment vaddr (`CNode_Copy` each master frame cap into a fresh root slot,
     `vspace_map_pages_at_vaddr` with `seL4_CanRead` only). refcount++.
   - This REPLACES `setup_demand_text` for the read-only segment (you either share
     OR demand-page, not both). Keep demand-BSS + the writable .data as-is.
3. **Teardown** (proc exit / re-exec -- mirror `clear_file_vmas`): unmap + delete
   the child's frame-cap COPIES (the vspace tear-down already does this), and
   refcount--. Do NOT free the master frames while refcount>0. Free a cache entry
   (its master frames) on refcount==0 only under memory pressure, or just keep it
   (per-boot cache; reboot clears it).

## Gotchas (the ones that will bite)
- **I-cache: unify the master frames ONCE at cache-fill time.** Because the shared
  frames are READ-ONLY and already at PoU, a new proc that maps them executes
  correctly with NO per-proc unify (its I-cache for that vaddr is cold -> fetches
  from PoU). This is CLEANER than demand-text (which had to unify per proc). But
  do NOT forget the one-time unify at fill -- it is the v0.4.182 lesson: code
  loaded via DATA writes MUST be unified, and **QEMU (a53) will not catch a
  missing one** -- only the real A72 will. Test on HW.
- **Map read-only.** `seL4_CanRead` only (executable comes from
  Default_VMAttributes, which excludes ExecuteNever). A writable mapping of shared
  code = one proc corrupts all. 
- **Caps are NOT shared, frames are.** Each child mapping needs its own
  `CNode_Copy` of the master frame cap (a frame cap maps into one vspace per cap).
  So N procs x M pages = N*M cap copies, but only M physical frames. RAM (the
  dominant cost) is saved; cap/allocman-slot pressure is not -- watch the pool.
- **Cache staleness on push-deploy.** If a binary is updated (pushed + the proc
  re-exec'd) the cache is stale. Per-boot cache + `reboot` after a push (the
  normal deploy flow) sidesteps it; if you cache across a live push, invalidate by
  path on write. Simplest: clear the whole text_cache on any fs write to /bin, or
  just rely on reboot.
- **fork:** `do_fork` eager-reloads the ELF today; leave it (it already shares RO
  pages parent->child via `fork_share_region`). Phase 2 is the EXEC path. (A later
  step could route fork through the cache too.)
- **Boot servers load from the CPIO** (`spawn_util.c`), NOT these exec paths --
  leave them eager (do not share/demand-page them; they are pre-fs).

## How to test
- **QEMU first** (correctness, ceiling): `python3 scripts/smp_qemu_test.py` (must
  stay 7/7). Then push the fork-width higher -- with sharing, the ceiling should
  climb well past 30 because same-binary procs stop duplicating code. The
  `/tmp/smp_ceiling.py`-style probe (set `MAX_ACTIVE_PROCS` high, watch the serial
  for the wall) measures it.
- **HW is mandatory** (the I-cache lesson): flash + verify on the Pi -- do the
  disk-exec'd services come up, and do pipelines run? Use `/tmp/verify_182.py`
  (find Pi on .8 OR .127, probe netconsole/sshd, run a pipeline). QEMU passing is
  necessary but NOT sufficient for any code-loading path.

## Conventions
- QEMU-test before HW; deploy userspace by push, flash only for root-task changes
  (this IS a root-task change -> flash, at a milestone).
- No apostrophes in C comments. Commit only when asked. version.h -> 0.4.183 when
  this lands. Commit msgs end with:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

## Fallback
If sharing proves too invasive, demand-text (v0.4.182) already shipped the bulk of
the win and is HW-verified -- this is purely additive. Revert is just removing the
cache lookup + restoring `setup_demand_text` for the RO segment.
