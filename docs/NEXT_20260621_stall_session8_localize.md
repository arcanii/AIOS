# Stall hunt session 8 -- pinpoint the idle->wake wedged instruction (v0.4.286)

The RPi4 ~32.4s idle-teardown freeze stays a **MAJOR OPEN CONCERN** ([[feedback_stall_open_concern]]).
This session sharpens the session-7 localization (the freeze is the IDLE->WAKE window, not teardown)
toward the EXACT wedged instruction, and resolves the open "wedged-CPU vs IRQ-not-delivered" question.

## What session 7 actually captured (re-read of the serial, not just the summary)

`/tmp/sercap_idle.log` (session 7, ASID-gen ON, keyboard off, `netstall --idle 30`):

```
[STAGECP] core=0 prev=11 this=11 dur=64784ms hb_lag=18446744073709551615ms   <- core 0
[STAGECP] core=2 prev=11 this=10 dur=64797ms hb_lag=64795ms                   <- idle core
[STAGECP] core=3 prev=11 this=10 dur=64792ms hb_lag=6479(garbled)             <- idle core
[STAGECP] core=0 prev=11 this=11 dur=32397ms hb_lag=18446744073709551615ms   <- core 0
[STAGECP] core=2 prev=11 this=10 dur=32405ms hb_lag=32403ms
[STAGECP] core=3 prev=11 this=10 dur=32411ms hb_lag=32409ms
```

Two facts the one-line memory summary smoothed over:

1. **Core 0 is `prev=11 this=11`, NOT `prev=11 this=10`.** Only the IDLE cores (2,3) are `11->10`.
   So core 0 (the BKL holder, the actually-wedged core) goes kernel-exit -> ... 32.4s ... ->
   kernel-exit again, with **NO IRQ entry (stage 10) in between** -- it re-enters the kernel via a
   SYSCALL/fastpath/fault, not an interrupt. The idle cores' `11->10` is DERIVATIVE: they take the
   timer IRQ at freeze-start and block in `c_handle_interrupt`'s `NODE_LOCK_IRQ` on the BKL core 0
   holds for the whole 32.4s (`hb_lag ~= dur`).

2. **Core 0's `hb_lag` is garbage (`0xFFFFFFFFFFFFFFFF` = -1).** Core 0 NEVER runs `idle.S` (it is
   saturated by prio-200 yield-spinning servers -- session 4), so `aios_core_heartbeat[0]` is always
   0 and the idle heartbeat tells us NOTHING about core 0. The decisive "is core 0 wedged?" signal
   was missing.

The window between the two core-0 stage-11s was **uninstrumented**: there was no checkpoint at kernel
ENTRY, and fastpath round-trips skip stage 11 entirely (they exit via `fastpath_restore`). So `11->11`
just means "two slowpath exits 32.4s apart, everything between invisible."

## The v0.4.286 instrumentation (this session)

Three additions, all behind `AIOS_TEARDOWN_CHECKPOINTS` (machine.h, =1):

- **Stage 9 = kernel ENTRY** (`arch_c_entry_hook`, `include/arch/arm/arch/kernel/traps.h`): fires on
  EVERY entry (syscall/fastpath/fault/IRQ), AArch64-guarded.
- **Stage 12 = fastpath EXIT** (`fastpath_restore`, `.../mode/fastpath/fastpath.h`): mirrors stage 11
  so fastpath round-trips are no longer invisible.
- **Per-interval PMU deltas + real core-0 heartbeat** (`aios_checkpoint`, `errata.c`): each checkpoint
  snapshots INST_RETIRED / BUS_ACCESS / CPU_CYCLES (PMEVCNTR0/1/4) and, on a >=5s gap, prints the
  delta over the interval. Adds `aios_cp_kentry_t[core]` (stamped at stage 9) reported as `kent_lag` =
  ms since this core's last kernel entry -- a liveness mark for core 0 (which never idles). Renamed the
  idle-heartbeat lag to `idle_lag` for clarity.

New `[STAGECP]` line:
```
[STAGECP] core=C prev=P this=S dur=Dms idle_lag=Ims kent_lag=Kms pmu=[inst=N bus=B cyc=Y]
```

Stage map: 1-4 unmapPage, 5-6 unmapPageTable, 7 ctx-switch dsb (`setCurrentUserVSpaceRoot`),
**9 kernel entry**, 10 IRQ entry, 11 slowpath exit, **12 fastpath exit**.

## Interpretation matrix (what the HW capture will tell us)

For the **wedged core (core 0)**, the `(prev -> this)` pair localizes the REGION, and `inst` says
whether it is one stuck instruction:

| `prev -> this` at the >5s gap | kent_lag | meaning |
|---|---|---|
| `9 -> 11` or `9 -> 12` | `~= dur` | wedge is INSIDE the kernel handler (entry -> exit) |
| `11 -> 9` or `12 -> 9` | n/a (just stamped) | wedge is BEFORE entry: the previous exit's eret asm / user EL0 / the exception vector |
| `9 -> 10` | `~= 0` | wedge between a syscall exit and an IRQ (rare) |

PMU delta over the interval (dominated by the ~32.4s wedge):
- **`inst ~= 0`** => core was CLOCKED but WEDGED on ONE instruction (a barrier) -- *not*
  "IRQ-not-delivered while the CPU loops" (that would retire billions). Resolves seed lead #2.
- **`inst` large** => the core was LOOPING/active for 32s (then it is a scheduling / IRQ-delivery
  issue, not a CPU wedge) -- this would overturn the "wedged on a dsb" model.
- **`bus ~= 0`** => a clean wait on a PARKED fabric (vs large = a retry storm).
- `cyc` wraps ~7x over 32s on a 1GHz core -> noise; only meaningful on short intervals.

Expected separation of PRIMARY vs DERIVATIVE: the idle cores (2,3) spin in `clh_lock_acquire` on the
BKL, so they should show **`inst` large** (spinning), while core 0 shows **`inst ~= 0`** (the one
genuine wedge). That contrast is itself a confirmation.

If `9 -> 11/12` (wedge in handler): the next session adds checkpoints around the handler's barriers
(the only fabric ops left in a non-teardown round-trip beyond the already-silent stage-7 dsb and the
ASID-gen-removed tlbi). If `11/12 -> 9` (wedge before entry): the next session adds an ASM checkpoint
in the `traps.S` vector (at `kernel_enter`, before the branch to `c_handle_*`) to split eret-asm vs
the exception vector itself.

## HW A/B procedure (board is netconsole-wedged from the session-7 coresched test)

1. **POWER-CYCLE the Pi** (physical). It is alive on ICMP at 192.168.0.8 but the netconsole/shell is
   wedged (session-7 coresched test); the hwdog will NOT auto-reset it because core 0 still serves
   ICMP (the watchdog keeps petting).
2. **Start serial capture FIRST** (one reader only): `python3 scripts/sercap.py /tmp/sercap_s8.log`
3. Flash + verify banner: `python3 scripts/pi_flash.py --build` (builds build-rpi4, swaps kernel8,
   reboots, verifies `/proc/version` matches the freshly-built kernel8 -- v0.4.286, build number
   stamped at build time). WAIT for banner-PASS.
4. Gold A/B (ride stalls): in one terminal `python3 /tmp/pingmon.py` (ICMP GAP detector); in
   another `python3 -u scripts/netstall.py --idle 30 --trials 10 --label s8`.
5. Grep the capture: `grep STAGECP /tmp/sercap_s8.log`. Read off core 0's `(prev->this)` + `inst` +
   `kent_lag` per the matrix above.

DISCIPLINE: netconsole wedges under churn -> drive gently, never overlap a deploy with a test;
power-cycle to recover. The [STAGECP] print fires only AFTER the wedge (fabric warm again), so it does
not contaminate the measured op. The instrumentation touches no external bus (sysreg reads +
per-core cacheable stores) so it cannot warm the fabric / mask the stall.

## HW RESULT (v0.4.286 build 2784, flashed + tested 2026-06-21 -- DECISIVE)

`netstall --idle 30 --trials 10` (keyboard out) = **6/10 STALLED** (worst residual 65.6s, 1 conn-death;
trials 8-9 reconnect-failed = netconsole wedged by the churn) -- the historical ~5-7/10 rate.
`pingmon` caught **10 real whole-system ICMP GAPs** (32.5s singles, 65s doubles, one **130.0s quad**) --
these are TRUE whole-box freezes (ICMP stops), not netconsole-only deaths.

**Core 0 (the wedged core) was caught at FOUR distinct code sites, every one `iovf=0`** (clocked but
wedged on ONE instruction, retired far less than 2^32 over the whole 32.4s) with a tiny `bus` delta
(`bovf=0` -- a clean wait on a PARKED fabric, not a retry storm). The inst-count is DETERMINISTIC per site:

| site | prev->this | inst (exact) | bus | where the wedge fell |
|---|---|---|---|---|
| A | 9 -> 11 | 1786 | ~120-184 | inside a slowpath syscall handler (no vspace switch, no teardown) |
| B | 11 -> 9 | 174 | ~13-21 | the syscall exit->entry transition (restore_user_context eret asm / vector) |
| C | 10 -> 11 | 366 | ~17 | an IRQ handler (no switch) |
| D | 10 -> 7 | 525 | ~5 | an IRQ handler that did a ctx switch -- wedge BEFORE setVMRoot's dsb (no `[DSBSTALL]`) |

The **idle siblings (cores 2,3)** are always `prev=11 this=9 iovf=1` (counter OVERFLOWED = they retired
billions = **spinning on the BKL** core 0 holds, `bus=1`) -- purely DERIVATIVE. The PMU overflow flag
(the review-driven hardening) cleanly separates the ONE genuinely-wedged core (`iovf=0`) from the
siblings spinning on its lock (`iovf=1`) -- without it the masked 32-bit deltas would have been ambiguous.

**VERDICT (now proven at the instruction level, not by elimination):** the freeze is
FABRIC-QUIESCENCE-FUNDAMENTAL. It is NOT one removable seL4 instruction at a fixed PC -- it is *whichever*
fabric-dependent instruction core 0 issues FIRST after the BCM2711 SCB fabric parks, and it lands wherever
core 0 happens to be (syscall handler / IRQ handler / ctx-switch path / eret-vector transition -- 4 sites
observed). This is exactly why every prior single-instruction-removal lever failed (tlbi, cache-clean,
ctx-switch dsb): removing one site's op just moves the wedge to the next fabric op core 0 reaches.
- Resolves lead #2 definitively: **wedged CPU on one instruction, NOT "IRQ-not-delivered while looping"**
  (`iovf=0`, inst 174-1786 over 32.4s). The timer DOES fire (siblings take it and block on the BKL);
  core 0 is wedged with IRQs masked in the kernel.
- Completes lead #1: the wedge is localized to "the first post-quiescence fabric op," position-independent.

**LEADING MECHANISM HYPOTHESIS (Phase-2 design workflow, high confidence both lenses): the wedge is a
COLD CACHEABLE DRAM LOAD, not a `dsb`.** After the ~30s idle the A72 working-set cache lines (the TCB
register context read by `restore_user_context`'s `ldp` chain; the scheduler/`ksCurThread`/ready-queue
lines read by `schedule()`/`activateThread()`; the kernel stack written by `kernel_enter`'s `stp`) go
cold; the FIRST such access after quiescence needs a DRAM fill THROUGH the parked SCB fabric, which hangs
to the fixed ~32.4s SCB/UBUS timeout. This UNIFIES the 4 sites (each touches a different cold line first)
and fits the PMU signature better than a barrier: `inst=0` (the load can't retire), `bus` small-but-nonzero
(ONE pending line-fill, not a DVM round-trip's ~0 and not a retry storm's large), fixed-timeout duration.
Crucial subtlety from the source trace: `aios_checkpoint(11)` is recorded JUST BEFORE the restore asm, so
a wedge on the TCB-restore `ldp` always presents as SITE B (`11->9`); SITE A (`9->11`) is the same
mechanism one stage earlier (a cold `schedule()`/`activateThread()` read before stage 11). This is why
EVERY prior lever failed: tlbi/cache-clean/ctx-switch-dsb removal cannot help a cold-DRAM-LOAD timeout.

**This SHARPENS the cure direction:** if it is a memory-fill timeout (not a DVM-barrier timeout), the
right keep-warm is a COHERENT external bus-master that periodically touches DRAM through the SCB during
idle (a DMA engine), keeping the memory fabric out of the deep-quiesce state -- NOT a CPU-side op (all
refuted) and NOT a dsb/tlbi change. The architectural cure (drop the BKL coupling so a wedged core 0 does
not freeze the whole cluster) is the other path. STILL A MAJOR OPEN CONCERN.

**Phase 2 (optional, ONE more shot -- CONFIRM load-vs-dsb):** bracket the restore-asm `ldp` chain with a
register-SAFE checkpoint (a new stage emitted from inside `restore_user_context`'s asm AFTER the `ldp`s
but BEFORE `eret`, saving/restoring the just-loaded GP regs around the call -- DELICATE, this path is
sacred) + a checkpoint in `schedule()`/`activateThread()`. If the SITE-B gap becomes `11->newstage` and
SITE-A becomes `9->schedstage`, the cold-LOAD hypothesis is CONFIRMED and the exact line is named. Adds
L2D_CACHE_REFILL/MEM_ACCESS to the [STAGECP] print (counters already armed) as corroboration. Needs a
power-cycle (netconsole wedged) + careful asm review. Does NOT change the mitigation posture -- it pins
the mechanism (memory-fill vs DVM-barrier), which is what tells us whether a coherent-DMA keep-warm could
work. Full per-site candidate analysis + checkpoint spec: workflow run `wf_5e364d17-60d`.

Board left on v0.4.286 build 2784, ALIVE on ICMP but netconsole-wedged from the test churn (power-cycle
to restore netconsole). Watchdog + hwdog default-on so it survives + auto-recovers the ongoing freezes.

## CURE ATTEMPT: autonomous DRAM-DMA keep-warm -- REFUTED (v0.4.287, HW-tested 2026-06-21)

The cold-DRAM-load hypothesis implied a cure: keep the SCB->memory path warm during idle with REAL DRAM
traffic (the one keep-warm class never tried -- all prior ones were CPU-side/DVM-path). Built
`src/servers/dma_warm.c`: a BCM2711 legacy-DMA channel with a SELF-LOOPING control block copying a
dedicated sub-1GB scratch buffer forever, autonomously in HW (no CPU kicks -> runs even while core 0 is
wedged). `/proc/dmawarm.1` arms, `.0` disarms; default-OFF. Picks an ARM-free channel from the VC
`GET_DMA_CHANNELS` mask (chose chan 6); 3-lens adversarially reviewed (the two "blocker/major" findings --
a claimed WAITS/PERMAP overlap and a cacheable-CB hazard -- were both FALSE POSITIVES, refuted against the
datasheet [PERMAP=20:16, WAITS=25:21, adjacent] and the non-cacheable mapping [last arg cacheable=0]).

**HW A/B RESULT -- REFUTED.** With the DMA armed + CONFIRMED RUNNING throughout (`active=1 err=0
dst0=0xa5a5a5a5`, self-loop CONBLK loaded, chan 6 priority 8), `netstall --idle 30 --trials 10` =
**6/10 STALLED -- IDENTICAL to the OFF baseline (6/10)**. pingmon caught 3 real ICMP GAPs (33.0/12.1/32.8s);
2 `[STAGECP]` core-0 freezes fired (same `prev=11 this=9 iovf=0` wedge signature). Continuous external DRAM
traffic makes ZERO difference to the stall rate.

**WHAT THIS PROVES (a sharp negative result):** the quiescence is NOT the shared SCB->memory-controller
path -- an external bus master keeps THAT busy with no effect. It is the **A72 cluster's OWN ACE/snoop
master interface** (ACINACTM, A72 TRM 2.4: the cluster idles its AXI master snoop interface when the
cluster's snoop traffic stops -- an A72 INPUT software cannot deassert). An external master (DMA on the
SCB) cannot keep the A72 cluster's port active, so core 0's FIRST external transaction after its own port
quiesced still wedges. The "cold-DRAM-load" is best understood as the A72's first external transaction
(load-fill or barrier) after ITS interface parked -- which is why the wedge floats across instructions/sites
but is always core 0's first post-idle fabric op. This eliminates the entire EXTERNAL-MASTER keep-warm class.

**Remaining cure directions (all uncertain):** (1) make the A72 CLUSTER keep its ACE port warm during idle
-- a CORE-0-side periodic external transaction (the variant never cleanly tested: fabwarm ran on core 1,
but core 0 is the core that wedges; though core-1 fabwarm Device-reads were ALSO refuted, suggesting the
quiescence may be deeper/VideoCore-side than the cluster ACE port). (2) The ARCHITECTURAL fix: drop the BKL
coupling so a wedged core 0 does not freeze the whole cluster (the only path that does not depend on
defeating the fabric quiescence). STILL A MAJOR OPEN CONCERN. `dma_warm.c` is KEPT default-OFF as a
documented A/B knob (like the refuted fabwarm/corewarm) -- it is the first proper external-DRAM-traffic test.

## CURE ATTEMPT 2: COHERENT cache-miss keep-warm -- ALSO REFUTED (v0.4.288, HW-tested 2026-06-21)

The DMA refutation pointed at the A72 cluster's OWN ACE/snoop interface, so the next lever was to make the
A72 cluster ITSELF issue continuous COHERENT transactions (which an external DMA cannot). Added `fabwarm`
**mode 3** (`src/servers/fabric_warm.c`, `/proc/fabwarm.3`): a core-1 thread cyclically reading a PRIVATE
2MB buffer (> the 1MB A72 L2) so every line MISSES -> a steady stream of COHERENT DRAM reads on the cluster
ACE master, with no cross-core DVM contention (distinct from the refuted Device-read modes 1/2 and the
cache-hitting corewarm). Ran with the watchdog parked (`/proc/watchdog.0`) so core 1 was free (both are
prio-1 on the timer-masked core 1 -- two equal-prio busy-loops cannot be time-sliced without a tick).

**HW A/B RESULT -- REFUTED.** Mode 3 verified running throughout (`iters` 8109->16136 in 3s pre-run, then
**741,918 full passes over the whole run = ~1.5 TB of coherent DRAM reads**, `sink` advancing). `netstall
--idle 30 --trials 10` = **5/10 STALLED -- SAME as baseline**; 7 `[STAGECP]` freezes (same `iovf=0` core-0
wedge), 6 pingmon GAPs. **Core 1 issuing 5.3 GB/s of continuous coherent reads did NOT keep core 0's path
warm.**

**WHAT THIS PROVES (the decisive negative):** the quiescence is NOT kept warm by ANOTHER core's traffic --
not even maximal COHERENT traffic. So it is either CORE-0-SPECIFIC (each core's request path to the SCB can
park independently; only that core's own traffic warms it) or DEEPER than any A72-issued transaction
(SCB/VideoCore-side). **The entire traffic-based prevention space is now exhausted: external DMA
(non-coherent), coherent cache-miss (core 1), Device-MMIO (core 1), cacheable cross-core RMW -- ALL
refuted.** Prevention from software is effectively dead.

=> The cure pivots to KERNEL-REDESIGN CIRCUMVENTION (stop a 1-core wedge from freezing the cluster), not
prevention. Leading options (design workflow `wf_8791f3fb-dc7`): (1) move the wedge OUTSIDE the BKL --
issue a deliberate fabric "wake" op at kernel entry BEFORE NODE_LOCK so the ~32.4s wedge happens with the
BKL free (cores 1-3 don't block) + pair with coresched so other cores keep serving; (2) try-lock + defer
in the IRQ path so siblings don't spin 32.4s on the BKL; (3) a CORE-0 timer-driven (sleeping, not
busy-loop) heartbeat -- the one prevention lead the mode-3 result still leaves open IF the quiescence is
core-0-specific; (4) full fine-grained locking [infeasible, 2+yr proof rewrite]. `fabwarm` mode 3 KEPT
default-OFF as a documented A/B knob. STILL A MAJOR OPEN CONCERN.

## Status
Built + HW-tested (v0.4.286 build 2784 localization; v0.4.287 build 2799 DMA-keep-warm REFUTED), QEMU gate
green-equivalent, kernel diff in
`deps/patches/seL4-kernel.patch`. KEPT: ASID-gen + coresched S1/S2 + watchdog default-on + the
[TLBISTALL]/[DSBSTALL]/[STAGECP] profilers (now with PMU overflow flags + kent_lag) + MVD-1. The freeze
is NOT cured -- this measurement DECISIVELY localized it (fabric-fundamental, position-independent).
