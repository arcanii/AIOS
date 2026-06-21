# ASID-generation TLB recycling -- IMPLEMENTATION design (session 7, 2026-06-21)

## IMPLEMENTED (session 7, v0.4.277) -- QEMU-regression + host-validated; HW A/B PENDING (Bryan flashes)

The full ASID-gen recycler is IN, gated `AIOS_ASID_GEN` (non-hyp/Pi only; identity on the
hyp/QEMU build). Went straight to the UNIFIED no-flush model (steps 2+3+4 at once) per the
adversarial review: `invalidateTLBByASID`/`...VA` themselves now do `aios_asid_gen_invalidate`
(abandon the hw binding + reload-if-current, NO `tlbi`) instead of a flush, so deleteASID,
deleteASIDPool, unmapPageTable, unmapPage (self + non-current), and the remap path ALL get the
cure with minimal surgery. The ONLY `tlbi` left is the generation-wrap `tlbi vmalle1`.
- Files (deps/kernel, captured in deps/patches/seL4-kernel.patch): `tlb.h` (gate+macro+extern),
  `vspace.c` (resolver `aios_hwasid_for`/`aios_hwasid_clear`/`aios_current_user_asid` +
  `aios_asid_gen_invalidate` + the 2 contextSwitch sites + the 2 invalidate funcs + unmapPage +
  inert `aios_flush_pending_asid`), `fastpath.h` (the 3rd contextSwitch site), `errata.c`
  ([ASIDGEN] TCR_EL1.AS boot probe).
- Review fixes folded: M1 (mooted -- no per-asid flush at all), M2 (32-bit gen, no recycle),
  M3 (fastpath re-resolves; tlb.h already on its include path), M4 (clear-first => step2==step3),
  M5 (self-munmap INLINE reload, BKL-held, before user resume).
- DEFERRED (coresched-only, provably moot under core-0 pinning; host-test-proven needed for
  coresched): S1 wrap reserved-asid, S2 fresh-asid peer-visibility, atomic residency RMW.
- Verification: scripts/asidgen_host_test.c 4/4 (single-core 0 corruptions / ~15.9k wraps; buggy
  modes caught); build-rpi4 + build-04 clean; QEMU gate (regression -- ASID-gen is identity on
  hyp). **HW stall A/B is the next step (Bryan flashes):** pingmon + netstall --idle 30, expect
  freezes -> 0; confirm ACTIVE via serial `[ASIDGEN] assigns= gen= wraps=` climbing + NO
  `[TLBISTALL]`. A keyboard makes stalls frequent (good A/B) but kills the int-IN EP -- baseline
  keyboard-OFF first.

The original staged plan + the full invariant follow (kept for reference / the deferred items).

---

Companion to `docs/NEXT_20260621_asid_generation.md` (the scope/why). This is the
grounded, code-exact implementation plan + the correctness invariant. THE STALL is a
MAJOR OPEN CONCERN, never concluded ([[feedback_stall_open_concern]]); this is the one
architectural cure left, and it is correctness-critical seL4 surgery that DIVERGES from
the Isabelle proof (ships tested-not-proven -- Bryan's call).

---

## 0. Grounding discovered this session (changes the plan)

1. **The QEMU build is HYP; the Pi build is non-HYP.** `build-04/kernel/gen_config.h`
   has `CONFIG_ARM_HYPERVISOR_SUPPORT=1`; `build-rpi4` does NOT. Consequences:
   - The AIOS residency-mask + lazy-TLB code lives in the `#else` (non-hyp) branch of
     `invalidateTLBByASID`/`...VA` (vspace.c:1105/1127). **That code, and ALL ASID-gen
     code, compiles + runs ONLY on the Pi.** On QEMU the kernel takes the hyp VMID path
     (seL4's own `getHWASID`/`storeHWASID` stage-2 VMID recycler in `armv/tlb.h`).
   - **=> The QEMU gate cannot exercise ASID-gen.** It is a REGRESSION gate (kernel
     builds; the hyp path is untouched; the whole system still works). ASID-gen must be
     verified by (a) a HOST unit test of the algorithm and (b) the HW stall A/B on the Pi.
   - **=> AIOS_ASID_GEN is gated `&& !CONFIG_ARM_HYPERVISOR_SUPPORT`.** On the hyp build
     the `AIOS_HWASID(a)` macro is identity `(a)` so armv_contextSwitch's existing
     `asid = getHWASID(asid)` is NOT double-translated.

2. **Hardware ASIDs are 16-bit.** Elfloader `mmu.S:73` sets `TCR_ASID16=(1<<36)` in
   TCR_EL1 (the kernel inherits it; non-hyp never rewrites TCR_EL1). So
   `AIOS_HWASID_MAX = 0xFFFF` => generation wraps are astronomically rare under normal
   load. A boot probe will CONFIRM AS=1 on silicon (step 1).

3. **seL4 logical asids are 16-bit too** (`ASID_BITS = asidHighBits+asidLowBits`, 7+9).
   The AIOS low-asid tables (residency, lazy-TLB) bound to `*_NASIDS = 2048`; ASID-gen
   uses the same bound (`AIOS_HWASID_NASIDS = 2048`), identity-fallback above it.

---

## 1. The invariant (the whole correctness argument)

> **INV:** At any instant, every hardware ASID value that may tag a LIVE TLB entry is
> bound to AT MOST ONE live vspace, AND a hardware ASID is never handed to a second
> vspace until a full TLB flush has happened since its previous binding.

If INV holds, no `tlbi` is ever needed on teardown/unmap of a *recyclable* vspace: the
dead/changed vspace's stale entries are tagged with a hardware ASID that nothing live
uses, and they are wiped by the one full flush at the next generation wrap before that
hw value is reused.

How each rule preserves INV:
- **Monotonic assignment within a generation.** `g_hwasid_next` only increases until a
  wrap. Each `sel4_asid` that needs a hw asid this generation gets the next free value.
  No hw value is handed out twice in one generation. (deleteASID does NOT recycle the hw
  value back into `g_hwasid_next` -- it is abandoned/leaked until the wrap.)
- **Generation wrap.** When `g_hwasid_next > AIOS_HWASID_MAX`: bump the generation, do
  ONE full `tlbi vmalle1` (+ residency-masked remote), reset `g_hwasid_next = 1`. Every
  prior `armKSHwAsid[*].gen` is now stale, so every vspace re-resolves to a fresh hw asid
  on its next use. The flush wipes ALL old-gen entries before any value is reused. (Wrap
  reserved-asid subtlety for the *currently-running* vspace: see section 5, open Q.)
- **Live-vspace mapping reduction (unmapPage / unmapPageTable of a vspace that will run
  again).** The vspace keeps existing, so it would run again under its SAME hw asid and
  could hit the just-removed mapping's stale TLB entry. Cure: when a page/PT is unmapped
  from a still-live vspace, INVALIDATE that sel4_asid's hw binding (`aios_hwasid_clear`,
  sets `.gen=0`). Its next `aios_hwasid_for` returns a FRESH hw asid; it runs clean; the
  old stale entries are abandoned (wiped at wrap). For the CURRENTLY-RUNNING vspace this
  must take effect before the thread resumes user mode (self-munmap path, section 4).
- **deleteASID (dead vspace).** No flush; `aios_hwasid_clear(asid)`. A future vspace that
  reuses that sel4_asid number calls `aios_hwasid_for`, sees `.gen` stale, gets a FRESH hw
  asid -- never the dead one's. Dead entries abandoned until wrap. INV holds.

NB the existing **lazy-TLB** ("defer non-current unmap, flush aside1 at switch-in") and
ASID-gen are two solutions to the SAME problem (a live non-current vspace's stale entry).
Under ASID-gen, lazy-TLB's "flush at switch-in" becomes "fresh hw asid at switch-in" --
i.e. `aios_hwasid_clear` instead of `aios_asid_set_pending_flush`. They must not both run;
ASID-gen supersedes lazy-TLB when enabled (section 3/4).

---

## 2. Data structures + the resolver (vspace.c, gated `#if AIOS_ASID_GEN`)

```c
#define AIOS_HWASID_NASIDS 2048
typedef struct { uint16_t hw_asid; uint16_t gen; } aios_hwasid_ent_t;
static aios_hwasid_ent_t armKSHwAsid[AIOS_HWASID_NASIDS];   /* sel4 asid -> (hw, gen) */
static uint16_t g_hwasid_gen  = 1;   /* current generation (never 0) */
static uint16_t g_hwasid_next = 1;   /* next free hw asid this gen (0 reserved = "none") */
#ifndef AIOS_HWASID_MAX
#define AIOS_HWASID_MAX 0xFFFFu      /* TCR_EL1.AS=1 (confirmed). Override tiny for wrap-stress. */
#endif

/* validity-signature counters (exported for the /proc TLBI profiler) */
word_t aios_hwasid_assigns = 0, aios_hwasid_gen_now = 1, aios_hwasid_wraps = 0;

word_t aios_hwasid_for(word_t sel4_asid)             /* assign-or-resolve; TTBR write path */
{
    if (unlikely(sel4_asid == 0 || sel4_asid >= AIOS_HWASID_NASIDS))
        return sel4_asid;                            /* 0 = global; out-of-range = identity */
    aios_hwasid_ent_t *e = &armKSHwAsid[sel4_asid];
    if (e->gen != g_hwasid_gen) {
        if (unlikely(g_hwasid_next > AIOS_HWASID_MAX)) {        /* wrap (rare, warm fabric) */
            if (++g_hwasid_gen == 0) g_hwasid_gen = 1;
            invalidateLocalTLB();
            SMP_COND_STATEMENT(doRemoteInvalidateTranslationAll(aios_resident_cores_mask));
            g_hwasid_next = 1;
            aios_hwasid_wraps++; aios_hwasid_gen_now = g_hwasid_gen;
        }
        e->hw_asid = g_hwasid_next++;
        e->gen     = g_hwasid_gen;
        aios_hwasid_assigns++;
    }
    return e->hw_asid;
}

static inline word_t aios_hwasid_current(word_t sel4_asid)   /* flush path: 0 = no live entries */
{
    if (likely(sel4_asid != 0 && sel4_asid < AIOS_HWASID_NASIDS)) {
        aios_hwasid_ent_t *e = &armKSHwAsid[sel4_asid];
        return (e->gen == g_hwasid_gen) ? e->hw_asid : 0;
    }
    return sel4_asid;                                /* identity fallback (>=NASIDS) */
}

static inline void aios_hwasid_clear(word_t sel4_asid)       /* abandon binding -> fresh next use */
{
    if (likely(sel4_asid < AIOS_HWASID_NASIDS)) armKSHwAsid[sel4_asid].gen = 0;
}
```

Macro (in tlb.h, with the gate):
```c
#if defined(CONFIG_ARCH_AARCH64) && !defined(CONFIG_ARM_HYPERVISOR_SUPPORT)
#define AIOS_ASID_GEN 1
word_t aios_hwasid_for(word_t asid);               /* defined in vspace.c (unity-build order) */
#define AIOS_HWASID(a) aios_hwasid_for(a)
#else
#define AIOS_HWASID(a) (a)
#endif
```

---

## 3. Per-call-site changes

**TTBR write (3 sites) -- pass the HW asid; keep residency/lazy keyed on sel4 asid:**
- `setVMRoot` (vspace.c:912): `armv_contextSwitch(vspaceRoot, AIOS_HWASID(asid));`
- `setVMRootForFlush` (vspace.c:932): `armv_contextSwitch(vspace, AIOS_HWASID(asid));`
- fastpath `switchToThread_fp` (fastpath.h:40): `armv_contextSwitch_HWASID(vroot, AIOS_HWASID(asid));`
  (`aios_flush_pending_asid(asid)` + `aios_mark_asid_residency(asid)` keep the sel4 asid.)

**Flush translation (non-hyp `#else` branches only):**
- `invalidateTLBByASID` (vspace.c:1106): translate `asid -> hw = aios_hwasid_current(asid)`;
  if `hw==0` return (nothing live to flush); else
  `invalidateTranslationASIDMask(hw, residency-mask)`.
- `invalidateTLBByASIDVA` (vspace.c:1128): same; `invalidateTranslationSingleMask((hw<<48)|..)`.

**Stepwise enable (each its own QEMU regression gate + commit):**
- **STEP 2 (this session):** all of the above, but `deleteASID`/`deleteASIDPool` STILL call
  `invalidateTLBByASID` (now flushing the correct hw asid). Behavior-preserving indirection.
  Also call `aios_hwasid_clear(asid)` in deleteASID after the flush (so the dead sel4_asid
  reuses fresh). Net stall behavior unchanged; proves the plumbing.
- **STEP 3:** `deleteASID`/`deleteASIDPool` -> NO flush, just `aios_hwasid_clear`. Lazy-TLB:
  `unmapPage` non-current -> `aios_hwasid_clear(asid)` instead of `aios_asid_set_pending_flush`;
  `aios_flush_pending_asid` becomes inert under ASID_GEN. Kills the deleteASID `aside1` stall.
- **STEP 4 (hard):** self-munmap (`unmapPage` current asid) + `unmapPageTable` current:
  instead of the eager `tlbi vae1`, `aios_hwasid_clear(currentAsid)` + force a TTBR reload
  with a fresh hw asid before returning to user. Mechanism: clear, then on the syscall-return
  path the next `setVMRoot`/fastpath resolves a fresh hw asid (re-`armv_contextSwitch`). Must
  guarantee NO user instruction runs between the unmap and the reload (true: BKL held in-kernel
  until return) AND that a multi-page unmap costs ONE fresh asid, not one per page (clear is
  idempotent until the reload consumes it). Kills the DOMINANT (self-munmap) stall.

---

## 4. Self-munmap sequencing (step 4 detail)

The running thread's vspace = `ksCurThread`'s vspace, asid `A`, hw asid `H` in TTBR0. A
self-munmap removes a page; `H`'s TLB may still hold it. We must make the thread resume
under a fresh hw asid `H'` (no stale `H` entry consulted) WITHOUT a `tlbi`.

- On the unmap: `aios_hwasid_clear(A)` (abandon `H`). Set a per-cpu/per-thread "vspace
  dirty -> reload before resume" flag (or simply rely on the unconditional `setVMRoot` that
  the unmap invocation's return path already performs -- VERIFY which return paths run
  `setVMRoot`; the slowpath does, the fastpath restore must be checked).
- Before user resume: `armv_contextSwitch(vroot, AIOS_HWASID(A))` -> `aios_hwasid_for(A)`
  sees `.gen=0`, assigns fresh `H'`, writes TTBR0, `isb`. The thread now translates under
  `H'` with an empty TLB for its (small) working set -- cold-TLB re-walk cost, acceptable
  on the already-slow spawn/teardown path. NO `tlbi`, NO `dsb`-to-fabric, NO stall.
- Idempotency: many unmaps in one syscall each `aios_hwasid_clear(A)` (cheap, sets gen=0);
  the single reload at return assigns ONE `H'`. Good.

Open: confirm EVERY path that returns to user after a self-unmap reaches a TTBR reload
(setVMRoot or fastpath switch). If a path can resume on the SAME H without reload, INV is
violated. The conservative belt-and-braces: in `unmapPage`/`unmapPageTable`, when the asid
is current, ALSO do the reload immediately (`armv_contextSwitch(curRoot, fresh)`), not just
clear -- self-contained, no reliance on the return path. (Costs nothing extra; the thread
is about to run.)

---

## 5. Open correctness questions (MUST be closed before HW ships / declared cured)

1. **Wrap + currently-running vspace (reserved-asid).** At a wrap the running vspace's
   hw asid `H` is in TTBR0; the full flush wipes its entries, but the CPU keeps running
   under `H` and refills entries tagged `H` AFTER the flush, until the next context switch
   reassigns it. If `g_hwasid_next` climbs back to `H` (same generation) and hands `H` to
   another vspace before this one is switched out, stale hit. Under DEFAULT core-0 pinning
   this is benign (we wrap while in-kernel mid-context-switch, about to load the NEW
   vspace's fresh asid; the old one isn't running). For coresched/SMP, adopt Linux's fix:
   at wrap, RESERVE every currently-active hw asid (per core) so it is not reissued this
   generation, and re-stamp the active vspace's entry into the new gen with the same value.
   With 16-bit asids wraps ~never happen in practice, but the wrap-stress test (tiny
   HWASID_MAX) MUST exercise this and pass.
2. **Multicore/coresched.** A vspace resident on >1 core: a fresh-asid swap on core 0 must
   be observed by the others before they run it again. The residency mask already widens
   for genuine coresched; the swap (clear) makes peers re-resolve on their next switch-in.
   Confirm no window where two cores run the same vspace under different hw asids with live
   shared writable TLB state. (Default config: all user threads pinned to core 0 -> moot.)
3. **asid_map / cap identity untouched.** The seL4 asid stays the cap identity
   (`capVSMappedASID`, asid_map, findVSpaceForASID, residency). ONLY the TTBR0[63:48] field
   and the operand of the remaining `tlbi`s change. Confirm SMMU (off) + hyp VMID (off on
   Pi) paths are untouched.
4. **`tlbi` operand width.** `invalidateTranslationASIDLocal(hw_asid)` issues `tlbi aside1,
   x` with `x = hw_asid << 48`. With 16-bit hw asids this is correct (asid in [63:48]).

## 6. Verification plan

- **Host unit test** (`scripts/asidgen_host_test.c` or similar): replicate the resolver +
  table, drive spawn/teardown/unmap/coresched sequences + tiny-`HWASID_MAX` wrap storms;
  ASSERT INV after every op (no two live sel4_asids share a hw asid in the current gen; a
  reused value only after a flush; clear-then-resolve yields a fresh value). This is the
  PRIMARY algorithm proof (QEMU can't run the non-hyp path).
- **Debug kernel assertion** (CONFIG_DEBUG_BUILD): on assign in `aios_hwasid_for`, scan
  that the new hw value isn't currently bound to a different live sel4_asid in this gen
  (O(NASIDS), debug-only) -> catches an INV break on real HW immediately.
- **QEMU regression gate** (smp/shmring/socket/netd on build-04): no regression (identity
  on hyp). smp 4/5 + shmring 25/26 host-load sheds OK.
- **HW stall A/B** (Pi, Bryan flashes): `pingmon` + `netstall.py --idle 30`; expect freezes
  -> 0. VALIDITY signature: `/proc` shows `asid-gen: assigns=N gen=G wraps=W` climbing +
  the TLBISTALL `tlbi` count -> ~0 on teardown (proves the path is ACTIVE, not just built).
  A keyboard attached makes stalls frequent (good for the A/B) but kills the int-IN EP;
  test keyboard-OFF first for a clean baseline.
