# Stall session 10 -- register-exact RE-LOCALIZATION instrumentation (v0.4.290)

The ~32.4s freeze is a MAJOR OPEN CONCERN -- never "solved" ([[feedback_stall_open_concern]]).
Session 9 OVERTURNED the attributed mechanism (a deliberate broadcast DVM-Sync / cold D-load after
genuine WFI idle = 0ms on this exact Pi4, bare-metal EL2 AND Linux EL1). So the wedge is
AIOS/seL4-environment-specific and is NOT a generic fabric op. This session builds the instrumentation
to find what it ACTUALLY is -- the missing datum is **the wedging instruction + its address**.

## What changed (built + QEMU-gated; NOT yet HW-tested)

All behind `AIOS_TEARDOWN_CHECKPOINTS` (=1). Pure instrumentation -- no logic/lock/timing change.
Touches only `errata.c`, `c_traps.c`, `fastpath.h`. Builds: build-rpi4 (Pi, build 2819, **v0.4.290**)
+ build-04 (QEMU). Gate GREEN at baseline: smp 4/5, net_socket 8/8, netd 10/10, shmring 25/26.

Two additions, both surfacing data the prior `[STAGECP]` line could not:

1. **PMU expansion** -- the NATURE of the wedged instruction, MEASURED not inferred. `aios_pmu_init`
   (errata.c) already armed MEM_ACCESS (cnt2) + L2D_CACHE_REFILL (cnt3) but the checkpoint never read
   them; CPU_CYCLES is event-cnt4 so it HAS an overflow bit we never read. Added cnt5 = L1I_CACHE_REFILL
   (the I-side counter). The checkpoint now prints `mem/movf l2/l2ovf l1i/iiovf cyc/cyovf`.
2. **Address breadcrumb** (`aios_breadcrumb`, errata.c) -- recorded at each kernel-EXIT (stage 11
   slowpath / stage 12 fastpath) JUST BEFORE the `eret` asm: `regs` (the TCB ctx base the `ldp` block
   restores = D-side cold-load candidate), `pc` (= registers[NextIP] = the user PC the `eret` returns
   into = I-side cold-FETCH candidate), `ttbr` (TTBR0_EL1 = user vspace + hw ASID[63:48]). A wedge in
   the exit->entry window leaves these stale = the exact pre-wedge state; the next entry's `[STAGECP]`
   prints them.

### New `[STAGECP]` line
```
[STAGECP] core=C prev=P this=S dur=Dms idle_lag=Ims kent_lag=Kms
  pmu=[inst=N iovf=0/1 bus=B bovf=0/1 mem=M movf=0/1 l2=L l2ovf=0/1 l1i=I iiovf=0/1 cyc=Y cyovf=0/1]
  bc=[s=BS regs=0xR pc=0xP ttbr=0xT] allidle=[..]
```

## HW procedure (GENTLE -- board hammered in prior sessions)

1. Power off; swap RPi-OS SD -> the **AIOS SD**. Power on.
2. `scripts/sercap.py /tmp/sercap_s10.log` FIRST (ONE serial reader). Wait for boot.
3. `scripts/pi_flash.py --build` (flashes build-rpi4, v0.4.290 build 2819). Wait for the banner PASS +
   confirm `v0.4.290` on the serial banner (NOT 289 -- that's the A/B identity check).
4. `scripts/pingmon.py` in one terminal (whole-system ICMP gap oracle).
5. `scripts/netstall.py --idle 30 --trials 10` -- the gold provocation. Do NOT over-probe netconsole
   (it fork-exhausts -> power-cycle). Use aios_nc.py held connections if hand-driving.
6. Collect every `[STAGECP] core=0 ... dur~32400` line from `/tmp/sercap_s10.log`. 3-5 wedges is plenty.
   Cross-check `/proc/laststall` + pingmon GAP count.

## INTERPRETATION MATRIX (read the core=0 wedge line, dur~32400)

**Step 1 -- clocked or gated? (the foundational new bit)**
- `cyovf=1` (cycle counter wrapped; a clocked core at >=600MHz wraps ~5-8x over 32.4s) =>
  **CLOCKED, pipeline stalled on ONE instruction** = a genuine memory/bus wedge. (Expected.)
- `cyovf=0` AND `cyc` small => the core was **CLOCK-GATED / in retention** during the wedge = NOT a
  bus stall. This OVERTURNS "wedged on a fabric op": the core itself stopped clocking (an involuntary
  WFI/retention/power event). Pivot to power-domain/WFI analysis (E4) -- a totally different cure axis.

**Step 2 -- (if cyovf=1) the NATURE of the instruction.** Confirm `iovf=0` (wedged on one inst), then:

| Signature (core=0 wedge) | Wedged instruction is | Notes |
|---|---|---|
| `mem>=1 l2=1 l1i=0` | a **cold D-side LOAD** mid-linefill | the s8 "cold-load" hypothesis. Address = `bc.regs` (if site B). The repros tested this and it did NOT hang -- so what's cold here that wasn't there? |
| `mem=0 l1i=1 l2=0/1` | a **cold I-side FETCH** mid-linefill | the NEW candidate the EL2/D-load repros could NOT test (e.g. the user PC after `eret`). Address = `bc.pc` (if site B). |
| `mem=0 l1i=0 l2=0` | a **barrier** (dsb/DVM) waiting on completion | but a deliberate barrier after idle does NOT hang (s9) => something about AIOS's ASID/TTBR context (`bc.ttbr`) matters. |
| `bus>=1 bovf=0` + above | one bus txn that hung | corroborates "parked fabric, one transaction." |

**Step 3 -- the ADDRESS (bc).** `bc` is the wedge address **only for site B** (`prev=11/12 this=9` =
the exit->eret->user-fetch window). For in-kernel wedges (`prev=9 this=11/12` = site A, or the IRQ/
ctx-switch sites C/D) `bc` is STALE (the prior exit) -- use PMU nature only there; getting the in-kernel
address needs in-handler breadcrumbs (a v2 if site A dominates).
- site B + D-load signature => the wedge is the TCB-restore `ldp` from `bc.regs`. Is `bc.regs` a normal
  kernel-heap DRAM addr? (If so: why is a kernel TCB line cold after a tiny idle loop -> cache lost it?)
- site B + I-fetch signature => the wedge is the first USER instruction fetch at `bc.pc`. Decode
  `bc.ttbr[63:48]` for the ASID (which process); `bc.pc` is its user text page.

## What each outcome unlocks (the cure axis)
- **Cold I-FETCH at `bc.pc`** -> a never-tested class. Cure candidates: prefetch/lock the user entry
  page, or a `bc.pc`-targeted I-side keep-warm; and a sharpened repro (E1 EL1&0 + EL0 excursion +
  cold I-fetch) becomes decisive.
- **Cold D-LOAD at `bc.regs`** -> if it's a TCB line going cold over idle, the cache is losing state
  during idle (retention/clean) -> a TCB-line keep-warm or pinning; re-examine what evicts it.
- **Barrier** -> re-opens "AIOS ASID/TTBR context" -- diff `bc.ttbr` vs a non-wedging exit.
- **Gated (cyovf=0)** -> not a memory wedge at all; the cure axis is power/clock domains (E4), not
  keep-warm.

## Honest scope
This is the "WHAT IS IT" investigation, not a cure. It gets the instruction + (for site B) the address
that 9 sessions never had. The stall STAYS a MAJOR OPEN CONCERN. Keep all prior mitigations
(sibling-timer-mask, MVD-1 watchdog, ASID-gen). Commit on `main`; Bryan pushes. seL4 diff regenerated
into `deps/patches/seL4-kernel.patch`.

---

# HW RESULT (2026-06-22) -- DECISIVE: it is the I-SIDE (cold user instruction fetch)

## Run 1 -- v0.4.290 (instrumentation): 3/3 stalls identical, site B, clocked, deterministic
`netstall --idle 30 --trials 10` = 3/10 STALLED; ping = 88 ICMP timeouts (independent confirm). All 3
`[STAGECP] core=0` lines IDENTICAL in structure (raw: `experiments/s10_capture/stagecp_v0.4.290.txt`):
- **`prev=11 this=9`** (SITE B) every time; **`cyovf=1`** (CLOCKED, not gated); **`iovf=0` + `bus~9-29`**
  (one instruction, parked fabric);
- **deterministic addresses**: `bc.regs=0xffffff80fb959c00` (TCB) AND `bc.pc=0x44c574` (user resume PC)
  in ALL 3 -- only the hw ASID rotates (ASID-gen). So within site B it is the SAME exit->resume
  transition every stall, not "position-independent."
- **`l1i` (23-36) >> `l2` (1-4)** -- I-side refills dominate D-side ~9:1.
Narrowed to two candidates the aggregate PMU could not separate: D-side TCB `ldp` vs I-side user fetch.

## Run 2 -- v0.4.291 (the "stage-13 splitter"): D-side EXONERATED, I-side CONFIRMED
Added, in `restore_user_context` after stage 11: pre-warm the TCB context lines (PAN-safe kernel reads),
then `aios_checkpoint(13)`. This splits the site-B window at the TCB restore. Result (raw:
`experiments/s10_capture/stagecp_v0.4.291_splitter.txt`): **5 stalls, ALL `prev=13 this=9` (I-side); ZERO
`prev=11 this=13` (D-side).**
- The wedge falls AFTER the TCB pre-warm + stage 13 -> NOT the TCB `ldp`.
- **`l2=0`** in the clean samples (pre-warming the TCB drove D-refills to zero -> proves the earlier
  `l2=1-4` WAS the TCB restore, and that no D-side fill hangs). `l1i` still 28-38 (I-side).
- `bc.pc=0x44c574`, `cyovf=1`, `iovf=0` all hold. One sample was a DOUBLE quantum (`dur=64798ms`) and
  3 netstall trials were multi-quantum conn-deaths (~105s) -- the wedge can repeat (matches the
  documented "multi-quantum" observation).

## CONCLUSION
**The ~32.4s wedge is the cold L1I/L2 INSTRUCTION-cache refill of the user resume line (containing PC
`0x44c574`), on the `eret` -> first-user-fetch path, after idle.** Clocked-but-stalled, deterministic,
sometimes multi-quantum. The TCB-restore `ldp` (D-side) is exonerated (0/5 with the splitter).

**Why this is consistent with the s9 overturn (and completes it):** the bare-metal/Linux repros timed
cold DATA loads + DVM ops after idle (0ms) but NEVER a cold INSTRUCTION fetch after idle. The wedge is
specifically the **I-fetch path** through the parked fabric -- the one access class never exercised.
New datapoint from the splitter itself: the TCB **D-access issued immediately before the I-fetch did NOT
wake the fabric for the I-fetch** (it still hung) -> D-traffic-right-before does not help the I-path
(consistent with the keep-warm refutations; the I-fetch path has its own quiescence).

## NEXT (the cure axis, now targeted -- pick with Bryan)
1. **Silicon-vs-AIOS for the I-fetch (sharpen E1):** extend `experiments/e1_repro` to evict an
   INSTRUCTION line, idle (WFI), then FETCH it (jump to it). If it reproduces ~32.4s bare-metal -> the
   I-fetch-through-parked-fabric is a silicon property -> cure = prevent the I-line eviction or keep the
   I-path warm. If not -> AIOS-specific (next item).
2. **Eviction hypothesis (AIOS-specific, testable):** the prio-200 yield-spinning servers' cache
   footprint may EVICT the user resume code during the 30s idle (Linux has no such idle churn) -> the
   resume fetch is cold -> hits the parked fabric. Test: reduce server cache pressure / pin or lock the
   resume page in L2; or correlate l1i-refill vs idle duration. This connects the s9 clincher ("AIOS
   has more idle activity than Linux") to a CONCRETE mechanism (cache eviction, not quiescence-prevention).
3. **I-side warm/prefetch before `eret`** (lower odds -- a D-touch right before did not help; an
   I-specific PRFM/PLI or `IC`-by-VA of `bc.pc` is a different path worth one test).

The stall STAYS a MAJOR OPEN CONCERN ([[feedback_stall_open_concern]]) -- this LOCALIZES it
register-exact (the session goal), it does not cure it.

---

# CURE ATTEMPTS (2026-06-22, all HW-tested) -- both viable classes REFUTED

## TEST 1 -- bare-metal cold I-FETCH (silicon-vs-AIOS): INCONCLUSIVE (0ms)
`experiments/e1_repro` extended to evict a line, idle 30s (busy-spin, 4-core EL2), then JUMP to it
(cold I-fetch) vs LOAD it (D-load control). **Result: ALL 0ms -- no hang** (raw:
`experiments/e1_repro/trial5_cold_ifetch.log`). So minimal bare-metal does not reproduce the wedge for
ANY op incl. the I-fetch -- consistent with s9 (minimal bare-metal likely never reaches the quiesced
fabric state the full AIOS/VC runtime hits; busy-spin may not park the SCB). Inconclusive on "pure
silicon"; a genuine-WFI cold-I-fetch (e1+timer-wake, or e3 Linux -- needs an RPi-OS re-flash) would
settle it. Does NOT block the cure: a warm resume line is a cache HIT regardless.

## KEY REFRAME -- AIOS has NO blocking sleep
Every "sleep" is a yield busy-wait (`nanosleep` -> yield; `serverstats`/`flush` `probe_sleep` ->
yield-spin loops; `ssh_*` nanosleep). So core 0 is NEVER idle -- the constant `seL4_Yield` syscalls
(-> kernel scheduler footprint cycling) + the heavy servers continuously EVICT blocked threads' resume
lines from L2 during the 30s idle. THAT is the eviction source. => "let core 0 idle" needs building
timer-notification blocking-sleep infra (a major project, and it would revert the no-WFI mitigation,
which this session's data suggests was counterproductive -- it never prevented the stall, it just
churns the cache).

## CURE B -- L2 keep-warm (v0.4.292 + v0.4.293 diag): REFUTED, and it won't converge
Idea: when a thread blocks, capture its resume-line PA (`AT S1E0R`, vspace current) into a ring; on
every kernel entry, refresh those PAs in the unified L2 via the physmap (vspace-independent) so the
resume FETCH hits L2 (no fabric). HW (v0.4.292): NO effect -- 0x44c574 still hung. The v0.4.293
`[IWARM]` diagnostic showed WHY, decisively:
- keep-warm RAN (touches 65M, faults=0, 475 captures of OTHER threads) but **`inring=0` for EVERY
  wedge incl. all 0x44c574** -- the wedging thread's resume line was NEVER captured.
- **ROOT CAUSE: netconsole blocks via the seL4 FASTPATH** (`fastpath_reply_recv` sets the blocked
  state directly), which **bypasses `setThreadState`** where the capture hook lives. So the one thread
  that wedges is never captured -> never kept warm.
- **2nd wedge site** appeared: `prev=9 this=11` at 0x49d3b0 -- an IN-KERNEL cold access (inst~1900),
  NOT a resume fetch. Keep-warm structurally can't address it.
- => the eviction is BROAD (resume lines AND in-kernel handler lines go cold). Even adding a fastpath
  capture hook (fixes 0x44c574 / site B) leaves site A + likely more -> **whack-a-mole, won't
  converge.** Raw: `experiments/s10_capture/cure_B_keepwarm_REFUTED_v0.4.292.txt`.
- Kept default-OFF (`AIOS_IWARM 0`, errata.c) as a documented A/B knob (like fabwarm/dma_warm), with
  the `[IWARM]` diagnostic intact.

## WHERE THE CURE STANDS -- both viable classes refuted
1. **Prevent the fabric parking** (every keep-warm, s4-8) -- REFUTED.
2. **Keep the at-risk lines warm** (s10) -- REFUTED (fastpath bypass + broad eviction -> whack-a-mole).
The only PRINCIPLED cure left: **stop the broad eviction by letting core 0 actually idle** -- i.e.
build timer-notification blocking-sleep so the servers wait instead of yield-spin, core 0 reaches a
quiet idle, nothing gets evicted, the resume fetch hits cache. MAJOR multi-area project (next session).
Until then: ACCEPT + the strong existing mitigation (sibling-timer-mask cluster-survival + MVD-1
watchdog/auto-reset). The stall is genuinely fabric/eviction-fundamental in AIOS's no-blocking-sleep
design. STAYS a MAJOR OPEN CONCERN. Board left on v0.4.294 (keep-warm OFF, all diagnostics kept).
