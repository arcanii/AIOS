# Session-14 PREWARM confirmation -- RESULTS LOG (live)

Goal: confirm/refute that `AIOS_FABRIC_PREWARM` prevents the ~32.4s idle->wake core-0 wedge.
Protocol: docs/s14_prewarm_protocol.md. Two oracles: `/proc/laststall` total (counter, >=9000ms) +
sercap `[STAGECP] core=0` (kernel checkpoint, >=5000ms gap, real-time, reports wedge type).

## Board / builds
- ON  baseline already on board at session start: build 2896 (v0.4.294, prewarm ON).
- OFF arm: build 2899 (AIOS_FABRIC_PREWARM 0, scratch+dsb compiled OUT -- verified in build-rpi4
  kernel_all.c). QEMU gate 4/5 (baseline). Flashed + /proc/version verified.

## Arms run
| arm   | kernel | prewarm | worker | idle T | cycles | core-0 wedges | notes |
|-------|--------|---------|--------|--------|--------|---------------|-------|
| A0    | 2896   | ON      | off    | 35s    | 20     | **0**         | both oracles 0 |
| OFF-B | 2899   | OFF     | off    | 35s    | ~16*   | **1**         | prev=9 this=11 cluster-freeze |
| OFF-B2| 2899   | OFF     | on(c2) | 35s    | 20     | **3**         | both types; bursty at the end |
| A1    | 2901   | ON      | on(c2) | 35s    | 20     | **0**         | matched to OFF-B2; both oracles 0 |

### VERDICT (matched A/B/A, reflash-based, single variable = AIOS_FABRIC_PREWARM):
- ON aggregate: **0 / 40 cycles** (A0 worker-off + A1 worker-on), both oracles, 0 wedges.
- OFF aggregate: **4 wedges / ~36 cycles** (both worker modes), BOTH wedge types, ~32.4-32.5s each.
- Matched pairs both directionally clean: worker-off A0 0/20 vs OFF-B 1; worker-on A1 0/20 vs OFF-B2 3.
- => PREWARM LEAD CONFIRMED **directionally + reproducibly + mechanistically** (the reflash A/B flips
  the wedge on/off with nothing but the prewarm define; the OFF kernel readily wedges where the ON
  kernel stays clean across 40 cycles incl. the worker-on burst-prone tail).
- => NOT yet statistically DECISIVE: today's OFF rate ~10% and BURSTY (vs s13 43%), so 0/40 ON is
  suggestive (naive p~0.016, but clustered wedges weaken it). A decisive verdict needs a reproducible
  HIGH-rate OFF regime with ON=0 against it (longer idle T, or the rapid-reconnect burst).
- Board left healthy on build 2901 (prewarm ON), worker disarmed, watchdog+hwdog on, laststall=0.

### PERF (from code, not separately benchmarked):
The prewarm is OFF the IPC fastpath (c_handle_fastpath_call has NO prewarm); it runs only at the 4
slowpath/IRQ entries. Cost per slowpath entry = 8 L1-resident reads + 1 dsb sy (~tens of ns when the
fabric is warm). Hot-path IPC is unaffected by design. The only large cost is the one-time post-idle
fabric wake it deliberately absorbs. A lighter barrier (dsb ish / no dsb) was NOT tested this session.

\* OFF-B stopped at ~cycle 16 to switch to worker-armed.

### OFF kernel (2899) -- all 4 wedges (laststall total reached 4), BOTH types reproduced:
1. 16:27:00  prev=9  this=11  dur=32380ms  (OFF-B, worker off) -- CLUSTER-FREEZE
2. 16:32:42  prev=13 this=14  dur=32398ms  worker advanced=681114 (OFF-B2) -- CONFINED teardown
3. 16:41:56  prev=9  this=11  dur=32501ms  worker advanced=1299 (OFF-B2) -- CLUSTER-FREEZE
4. 16:42:28  prev=13 this=14  dur=32468ms  worker advanced=682553 (OFF-B2) -- CONFINED teardown
=> the s13 SPLIT is reproduced: cluster-freeze (prev=9 this=11, worker frozen) vs confined
   teardown (prev=13 this=14, worker full-rate). pc=0x4a1e70 (cluster) / 0x450f48 (confined),
   stable across wedges.

## HONEST STATISTICS (the load-bearing caveat)
- ON so far: A0 = 0/20. (A1 pending.)
- OFF: 4 wedges over ~41 teardown-triggering connects ~= **10%** -- FAR below s13's 43%.
- If A1 = 0/20 -> ON aggregate 0/40. Naive binomial: P(0/40 | rate 10%, prewarm inert) ~= 0.9^40
  ~= 0.016. BUT the OFF wedges are BURSTY/CLUSTERED (3 of 4 in a 90s window of rapid reconnects),
  so they are NOT 4 independent draws -- effective independent episodes ~= 2. That inflates the
  true p-value well above 0.016. => the result is SUGGESTIVE + directionally consistent + mechanistically
  coherent, NOT statistically DECISIVE at today's low/non-stationary rate.
- The non-stationarity itself is a finding: the wedge rate varies a lot run-to-run (s13 43%, today
  ~10%), and clusters. A DECISIVE verdict needs a reproducible HIGH-rate regime (try longer idle T,
  or the rapid-reconnect burst pattern) with ON=0 against it. Logged for the review.

## Key wedge capture (OFF-B, build 2899, prewarm OFF) -- the canonical wedge reproduced:
```
16:27:00 [WDOG] core0 STALLED ... stalls=1
16:27:23 [STAGECP] core=0 prev=9 this=11 dur=32380ms idle_lag=-1 kent_lag=32380ms
         pmu=[inst=1805 iovf=0 bus=167 ...] bc=[s=11 pc=0x4a1e70 ...] allidle=[-1,866367,0,0]
16:27:23 [WDOG] core0 recovered after 32419ms
laststall: last_dur=32419ms total=1
```
=> prev=9 this=11 = the IDLE->WAKE cluster-freeze wedge (in-kernel, post-NODE_LOCK), EXACTLY the
   type the prewarm targets. Confirms: (a) my drive reproduces the real wedge, (b) the detector
   fires, (c) build 2899 (prewarm OFF) is genuinely wedge-prone.

## FINDING (mid-experiment): the wedge RATE is drive-condition-sensitive.
At T=35s worker-OFF the OFF kernel wedged only ~1/16 (~6%), far below s13's ~43%. The s13 runs had
the confine worker ARMED (continuous core-2 syscalls -> cross-core BKL/coherency traffic). Hypothesis:
the worker's cross-core coherency churn raises the idle->wake wedge rate (sets up the DVM-sync-on-
quiesced-fabric hang). OFF-B2 tests this (worker armed). The matched ON arm (A1) will use IDENTICAL
conditions (worker armed, same T) so the comparison is fair. A0 (worker off) stays a bonus datapoint.

(updated as arms complete)
