# ASID-generation TLB recycling -- the architectural cure for the RPi4 idle-teardown stall

**Status: SCOPED, not started (2026-06-21). This is THE remaining cure for the stall
(a MAJOR OPEN CONCERN -- see [[feedback_stall_open_concern]], do NOT call it concluded).**
Every ISA-level lever is exhausted; this is the one direction left, and it is correctness-
critical seL4 surgery. Multi-session.

---

## 1. The problem this eliminates

The ~32.4s freeze is ONE `tlbi <op>; dsb` whose `dsb` waits on a DVM-Sync that the quiesced
BCM2711 SCB fabric answers in ~32.4s (a fixed SoC-level timeout, deterministic to the ms). The
A72 emits that DVM-Sync from ANY `tlbi;dsb` -- proven independent of:
- TLBI scope: local `tlbi vae1` (default) vs broadcast `tlbi vae1is` -- both froze (2026-06-19).
- DSB scope: `dsb sy` (default) vs `dsb nsh` -- both froze 32399ms (build 2122 "D2", 2026-06-13;
  docs/NEXT_20260613). The TRM "DVM-Sync generated regardless" clause holds empirically.
- Every keep-warm / clock / voltage / register lever (sessions 2-4, [[project_stall_hunt]]).

**=> No `tlbi`/`dsb` VARIANT dodges it. The only cure is to not ISSUE the teardown `tlbi` at all.**

The two `tlbi` sites that actually fire on the hot path (both stall):
- **`deleteASID`** ([vspace.c:1237](../deps/kernel/src/arch/arm/64/kernel/vspace.c)) ->
  `invalidateTLBByASID` ([:1089](../deps/kernel/src/arch/arm/64/kernel/vspace.c)) ->
  `invalidateTranslationASIDMask` -> local `tlbi aside1` ([machine.h:265]) + masked remote IPI.
  One per dying vspace.
- **`unmapPage` self-munmap** (`asid == current`): a per-page `tlbi vae1` issued EAGERLY (the
  running thread would alias the freed VA). The live stall capture this session was exactly this:
  `[TLBISTALL] asid=1 va=0x117ce000 pa=0xf8222000` -- a root (asid 1) self-munmap of an anon page.
  The lazy-TLB defers NON-current-asid page flushes ([vspace.c:867]); self-munmaps it cannot defer.

## 2. Why ASID-generation cures it (the standard OS technique)

Today seL4's logical ASID (in the VSpace cap, `capVSMappedASID`) == the HARDWARE ASID written to
`TTBR0_EL1[63:48]` by `armv_contextSwitch`. A freed ASID number is reused by a future vspace, so
`deleteASID` MUST `tlbi aside1` first or the new vspace would hit the dead one's stale entries.

**Decouple them.** Give each hardware ASID a GENERATION. Recycle hardware ASIDs without flushing;
flush the whole TLB ONCE only when the hardware-ASID space wraps (rare, and during active context
switching when the fabric is WARM -- not the after-idle cold path that hangs). Invariant: within a
generation, each hardware ASID is bound to AT MOST ONE live vspace, so no stale hits are possible.

This is the Linux/xv6 PCID/ASID model. It removes EVERY hot-path `tlbi`:
- **`deleteASID`: no flush.** The dead vspace's hardware ASID + its stale TLB entries are harmless
  -- nothing live uses that hardware ASID until a generation wrap does one full flush first.
- **Deferred per-page flushes (lazy-TLB, non-current asid): dropped.** Same reasoning -- harmless.
- **Self-munmap (current asid): assign the running vspace a FRESH hardware ASID** (bump its gen
  entry, reload `TTBR0` with an `isb` -- NO `tlbi`/`dsb`) instead of `tlbi vae1`. Stale old-ASID
  entries are harmless; the thread re-walks under the fresh ASID (cold-TLB cost, acceptable on the
  already-slow spawn/teardown paths). See the open design question in section 5.

## 3. Design

Parallel table keyed by seL4 logical ASID (bound to the low ASIDs AIOS uses, like the lazy-TLB's
`AIOS_LAZYTLB_NASIDS=2048`; out-of-range -> eager-flush fallback, never generation-managed):

```c
typedef struct { uint16_t hw_asid; uint16_t gen; } hwasid_ent_t;
static hwasid_ent_t armKSHwAsid[AIOS_HWASID_NASIDS];   /* seL4 asid -> (hw_asid, gen) */
static uint16_t g_hwasid_gen  = 1;                     /* current generation */
static uint16_t g_hwasid_next = 1;                     /* next free hw asid (0 = reserved) */
#define HWASID_MAX  ((1u << ASID_BITS_HW) - 1)         /* check TCR_EL1.AS: 8- or 16-bit */
```

Resolve-or-assign (call from `armv_contextSwitch` / the `setVMRoot` path, [vspace.c:912]):
```c
static uint16_t hwasid_for(asid_t sel4_asid) {
    hwasid_ent_t *e = &armKSHwAsid[sel4_asid];
    if (e->gen != g_hwasid_gen) {
        if (g_hwasid_next > HWASID_MAX) {            /* wrap: ONE full flush (rare, warm fabric) */
            g_hwasid_gen++;
            invalidateLocalTLB();                    /* tlbi vmalle1; dsb  + IPI/broadcast to peers */
            g_hwasid_next = 1;
        }
        e->hw_asid = g_hwasid_next++;
        e->gen     = g_hwasid_gen;
    }
    return e->hw_asid;
}
```
Then `armv_contextSwitch(vspaceRoot, hwasid_for(asid))` -- the HARDWARE asid goes to TTBR, the
seL4 asid stays the logical identity everywhere else (caps, asid_map, residency).

Changes, file by file (all in the sibling seL4 tree, captured via deps/patches/seL4-kernel.patch):
- **vspace.c**: the table + `hwasid_for`; `setVMRoot`/`armv_contextSwitch` use the hw asid; gut
  `invalidateTLBByASID` in `deleteASID` to a no-op (just clear `armKSHwAsid[asid]`); make the
  lazy-TLB consume (`aios_flush_pending_asid`) a generation-bump instead of a `tlbi aside1`.
- **unmapPage self-munmap path**: replace the eager `tlbi vae1` with a "mark current vspace for a
  fresh hw asid" (deferred to syscall-return / next setVMRoot) -- see open question.
- **machine.h**: keep `invalidateLocalTLB_ASID` for the fallback/wrap path only.
- Keep the residency mask + masked remote shootdown (they become the per-core full-flush IPI for
  the generation wrap, not per-teardown).

## 4. Why this is the cure, not another mitigation

`deleteASID` + deferred-flush + self-munmap all stop issuing `tlbi`. The ONLY `tlbi` left is the
generation-wrap `tlbi vmalle1`, which fires once per `HWASID_MAX` new-vspace activations AND only
while the system is actively context-switching (warm fabric). The stall is SPECIFICALLY the first
`tlbi` after idle; the wrap flush is the opposite regime. (If 8-bit ASIDs make wraps too frequent,
confirm `TCR_EL1.AS=1` for 16-bit -> wraps are astronomically rare.)

## 5. Open design questions for the implementation session

1. **Self-munmap sequencing.** Switching the current vspace's hw asid mid-syscall: do it lazily
   (mark dirty, swap on syscall-return before user resumes) so a batch of unmaps costs ONE asid,
   not one-per-page. Must guarantee no user instruction runs between the unmap and the swap that
   could alias the freed VA (it cannot -- we are in the kernel on the BKL until return). Verify the
   fastpath restore path also honours a pending swap.
2. **Multicore.** A vspace resident on >1 core (coresched): a fresh-asid swap on core 0 must be
   seen by cores 1-3. The residency mask already tracks this; the swap marks the asid dirty and the
   masked IPI makes peers re-resolve on their next switch-in. Confirm no window where two cores run
   the same vspace under different hw asids with live shared TLB state.
3. **Generation-wrap flush on a cold fabric.** Low-probability but possible: a wrap that lands right
   after idle could itself stall 32s once. Acceptable (rare) but measure. Mitigation if needed: at
   the wrap, the watchdog (MVD-1) is already there to survive it.
4. **asid_map / cap interplay.** The seL4 asid is still the cap identity; only TTBR changes. Confirm
   `findVSpaceForASID`, SMMU bind, and the hyp VMID path (all in invalidateTLBByASID) are untouched
   or correctly bypassed (AIOS is non-hyp, non-SMMU -- the `#else` branch at [vspace.c:1106]).

## 6. Correctness + verification (this is the scary part)

- **Invariant to hold**: at any instant, no two distinct live vspaces share a hardware ASID, AND
  no hardware ASID is reused before a full TLB flush since its last binding. A bug = silent stale
  TLB hit = one process reading/writing another's memory = security hole. This MUST be got right.
- **seL4 is Isabelle-verified; this change diverges from the proof** (the proof models flush-on-
  delete). Re-verifying is a multi-year effort and out of scope -- so this ships as a TESTED-not-
  PROVEN research divergence. That is a real cost (it trades away the verification that is half the
  reason to use seL4); Bryan's call to accept it for the stall cure. Document it loudly.
- **Test plan**:
  - Correctness soak: heavy spawn/teardown + shared-memory + coresched stress; a stale-hit would
    show as data corruption. Add a kernel self-check (debug build): on assign, assert the hw asid
    is not currently bound to another live vspace in this generation.
  - Force frequent wraps (set `HWASID_MAX` tiny, e.g. 8) in a debug build to exercise the wrap path
    + prove no corruption across many generations.
  - HW stall A/B: `pingmon` + `netstall.py --idle 30` -- expect freezes -> 0 (the gold method,
    [[project_stall_hunt]]). VALIDITY: the kernel must report it is ACTIVE (e.g. a `/proc` counter of
    asid assignments + generation + wraps, and `tlbi` count -> ~0 on teardown) -- not just "deployed".
  - Full QEMU gate (smp/shmring/socket/netd) before every flash; SMP correctness is the key risk.

## 7. First steps (new session)

1. Read this + [[project_stall_hunt]] + [[feedback_stall_open_concern]] + the live capture in
   docs/HANDOVER (session 6). Confirm `TCR_EL1.AS` (8 vs 16-bit hw ASID) via the boot probe.
2. Build the parallel hw-asid table + `hwasid_for`; wire `setVMRoot`/`armv_contextSwitch` to it,
   leaving `deleteASID` flushing for now. Gate behind `AIOS_ASID_GEN`. QEMU smp gate green first.
3. Flip `deleteASID` to no-flush + lazy-TLB-consume to gen-bump. Re-gate.
4. Tackle the self-munmap swap (the hard part). Re-gate.
5. Add the debug stale-hit assertion + the wrap-stress test. Then the HW stall A/B.
Existing levers to keep: residency mask, masked shootdown, lazy-TLB scaffolding, MVD-1 watchdog,
`/proc/freezes` + the TLBISTALL profiler (the A/B metric).
