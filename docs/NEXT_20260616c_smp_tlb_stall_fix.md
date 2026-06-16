# NEXT (seed): RPi4 remote-TLBI stall — CAUSE FOUND + KERNEL FIX (Stage K) — v0.4.257

Self-contained handoff. Repo `~/Desktop/github_repos/AIOS`, branch `main`. Kernel source is the
sibling `~/Desktop/github_repos/seL4` (symlinked `deps/kernel`). Read `HANDOVER.md` + `MEMORY.md`
+ `[[project_stall_hunt]]` first.

## TL;DR

The residual RPi4 spawn-storm freeze (`project_stall_hunt`) is **FIXED on real HW**. Root cause
**confirmed** = the seL4 remote TLB shootdown (`doRemoteInvalidateTranslationSingle`) broadcasting
to **all** cores and hanging in `ipi_wait()` on **quiesced idle cores 1-3**. The fix: a per-ASID
core-residency bitmap so a teardown unmap IPIs only the cores that actually ran that vspace. Under
the current core-0 pinning the remote mask is always empty → **no IPI, no `ipi_wait`, no stall**.

## Evidence (real Pi, host-timed `scripts/netstall.py` over netconsole)

`sleep N; echo` = teardown-after-idle (the sleep quiesces cores 1-3, the echo's exit-unmap is the
stall candidate); the command wall-time is the detector (mini-UART too lossy for serial).

- **Baseline (broadcast shootdown, build 2515): 6/16 trials froze 33-66s** (clean N×10.8s quanta).
- **Core-warmer A/B** (`/proc/corewarm.1` busies cores 1-3): moderate freezes 6/16 → **0** — first
  proof the idle-core *state* drives it (the cause).
- **TLB fix (build 2518): 0 moderate freezes across 30+ trials** (vs 6/16 baseline; p≈4e-7 if
  unchanged). System verified responsive during the run (2nd-connection probe). The remaining
  sporadic ">83s deaths" are **intermittent netconsole transport wedges, NOT system freezes** —
  no unmasked broadcast shootdown remains in the non-hyp teardown path (verified), and a clean
  12/12 run had zero.

## The fix (Stage K) — in the seL4 kernel (captured in `deps/patches/seL4-kernel.patch`)

`src/arch/arm/64/kernel/vspace.c` + `include/arch/arm/arch/machine/tlb.h`:
- `armKSASIDResidency[2048]` (uint8 bitmap per ASID; bounded to the low ASIDs AIOS uses, out-of-
  range → safe full broadcast). Updated under the big kernel lock (non-MCS) → no atomics.
- **Set** the current core's bit in `setVMRoot` + `setVMRootForFlush` (right after
  `armv_contextSwitch`) — BEFORE the core can fill a TLB entry, so the bitmap is always a
  **superset** of cores holding entries → shootdowns never miss a core (correct), only skip cores
  that provably hold none.
- **Mask** `invalidateTLBByASID` / `invalidateTLBByASIDVA` (the teardown shootdowns) with
  `aios_asid_residency(asid)` via new `invalidateTranslation{Single,ASID}Mask(.., mask)` helpers.
  `doRemoteMaskOp` already drops self + skips an empty mask, so {core0} → no IPI.
- **Reset** residency in `deleteASID` (after the flush) so a reused ASID starts clean.

Correct under multi-core too: when work distributes to cores 1-3, residency legitimately grows and
the shootdown targets exactly the cores that ran each vspace. **CAVEAT for Stage S:** before any
user thread runs off core 0, residency MUST also be set on the **fastpath** vspace-switch (it does
not go through setVMRoot). The single choke point is `setCurrentUserVSpaceRoot(ttbr)` (machine.h) —
extract `asid = ttbr>>48` there. Until then the fix is correct ONLY because all threads are
core-0-pinned (the unmapping core always does its local invalidate; no other core has user entries).

## Verification

- **QEMU (build 2518, smp=4): no correctness regression** — `smp_qemu_test` 7/7 (pipeline ceiling
  30, pipe-storm x12 exact data), `net_socket` 8/8. The masked shootdown is inert on QEMU (all user
  work on core 0 → remote mask empty → only the local invalidate, same as before).
- **HW (build 2518, flashed via pi_flash, sha-verified):** stall gone (above). Pi at 192.168.0.8.

## State at handoff

- **Pi runs v0.4.257 build 2518** (the TLB-fix kernel; ALSO carries the inert core-warmer).
- Kernel change is in the **seL4 working tree** (uncommitted, matching the idle.S pattern) AND
  captured in `deps/patches/seL4-kernel.patch` (regenerated to include it). `disk/kernel8.img`
  untouched (cube rollback). Diagnostic kernels on disk: `kernel8_v257_{stallrecover,corewarm,
  tlbfix}.img`.
- AIOS-side (committed): `src/servers/core_warm.c` (diagnostic, inert until `/proc/corewarm.1`),
  `scripts/netstall.py` + `scripts/serwatch.py` (reusable stall probes), boot/procfs/CMake wiring.

## NEXT — Stage S: schedule user processes across cores 0-3 (Option D)

The fix unblocks real SMP use. `SetAffinity(child, core)` round-robin in exec_server/pipe_server
after spawn (keep root servers + allocation on core 0 — the allocator is lock-free/core-0-only).
FIRST add the fastpath residency hook (above) so multi-core teardown stays correct. QEMU-verify
the "second SSH" corruption test stays green, then HW. See the investigation in this session's
workflow + `[[project_stall_hunt]]`.
