# SEED PROMPT -- RPi4 idle-teardown stall: session 9 (the cure is exhausted; mitigation robustness next)

Paste the SEED PROMPT block at the bottom into a fresh session. Everything above it is grounding. The
stall is a **MAJOR OPEN CONCERN -- never frame it as solved/concluded** ([[feedback_stall_open_concern]]);
the circumvention + MVD-1 are mitigation, NOT a cure.

---

## DEFINITIVE STATE (what session 8 established -- do NOT re-derive)

**The freeze:** a ~32.4s (deterministic to the ms: 32399ms) whole-system freeze on the RPi4/BCM2711
(Cortex-A72, AArch64, non-hyp, SMP=4, all user threads pinned to core 0). It is core 0 CLOCKED-BUT-WEDGED
on a SINGLE fabric-dependent instruction (PMU INST_RETIRED~=0 / `iovf=0` = retires nothing for the whole
32.4s = one stuck instruction; tiny BUS_ACCESS = a clean wait on a PARKED fabric). It is
POSITION-INDEPENDENT -- caught at 4 deterministic code sites (slowpath syscall handler, exit->entry
transition, IRQ handler, ctx-switch path): "the first fabric/DVM-completion op core 0 issues after the SCB
fabric quiesces during idle," wherever core 0 is. NOT a removable fixed-PC instruction. The whole cluster
freezes because core 0 holds the Big Kernel Lock (CLH lock) during the wedge, so cores 1-3 take the timer
IRQ, enter the kernel, and spin in `clh_lock_acquire` for the full 32.4s (PMU `iovf=1` on them = derivative).

**THE CURE IS EXHAUSTED ON EVERY SOFTWARE-REACHABLE FRONT (session 8's headline):**
- **Prevention (keep the fabric warm) -- DEAD.** ALL traffic-based keep-warms refuted on HW: external
  non-coherent DMA (a self-looping BCM2711 DMA, continuous DRAM traffic, `src/servers/dma_warm.c`,
  /proc/dmawarm); COHERENT cache-miss reads (core 1 reading a >L2 buffer at 5.3 GB/s, `fabwarm` mode 3,
  /proc/fabwarm.3); Device-MMIO reads (fabwarm modes 1/2); cacheable cross-core RMW (corewarm). Each was
  ARMED + CONFIRMED RUNNING and the stall rate stayed = baseline (~6/10). Crucially, core 1's 5.3 GB/s of
  coherent reads did NOT keep core 0's path warm -> the quiesce is the **DVM/coherency-COMPLETION path**
  (not a data path -- data traffic doesn't exercise it), and it is either core-0-specific or deeper than
  any A72-issued op. All keep-warms are KEPT default-OFF as documented A/B knobs.
- **Bound the 32.4s timeout (lead #4) -- DEAD.** Deep research (web + source) found NO ARM-side register:
  ARM_LOCAL (0xFF800000) has none (AXI_QUIET_TIME@+0x30 is a quiesce DETECTOR, max ~0.87s; AXIERRIRQ_EN
  only fires the L2 error IRQ AT t=32.4s). The GISB arbiter (the prime suspect, Linux brcmstb_gisb.c
  ARB_TIMER) is Set-Top-Box-only, NOT on BCM2711 (no DT node; probing a guessed base = SError). The 32.4s
  is CLOCK-INDEPENDENT (identical at all CPU/fabric clocks) = a fixed internal SILICON force-complete in
  the SCB/VideoCore fabric, VPU-firmware-set, not software-programmable. (The PCIe `UBUS_TIMEOUT`
  0xFD50_40A8 default 0x80000==32.4s, AIOS bounds to 0x1000, is the proven exemplar but PCIe-block-ONLY.)
- **User-responsiveness through the freeze -- BLOCKED / INFEASIBLE.** coresched (unpin to cores 1-3)
  wedges the Pi (distributing work exposes more cores to the per-core freeze). Fine-grained locking (remove
  the BKL) = a 2+yr Isabelle proof rewrite.

**WHAT WORKS / STANDS (session 8 deliverables, all on `main`, Bryan pushes):**
- **Decisive instruction-level LOCALIZATION** -- stage-9 (kernel entry) + stage-12 (fastpath exit)
  checkpoints + per-interval PMU OVERFLOW flags (`iovf`/`bovf`) + a real core-0 heartbeat (`kent_lag`) in
  `aios_checkpoint` (deps/kernel errata.c). `[STAGECP]` line:
  `core=C prev=P this=S dur=Dms idle_lag=Ims kent_lag=Kms pmu=[inst=N iovf=0/1 bus=B bovf=0/1 cyc=Y] allidle=[..]`.
- **The SIBLING-TIMER-MASK circumvention -- WORKS (v0.4.289, `AIOS_SIBLING_TIMER_MASK` in deps/kernel
  boot.c).** Extends MVD-1's timer-mask from core 1 to ALL secondaries -> with no tick, cores 1,2,3 stay in
  idle.S during a core-0 wedge, never take a timer IRQ, never block on the BKL. HW: 5/5 core-0 wedges had
  ZERO core=2/core=3 `[STAGECP]` lines (vs ALWAYS-present in 30+ prior wedges) -> the BKL cascade is broken;
  the cluster survives a 1-core wedge. (A correct CLH trylock-bailout is NOT cleanly possible -- a FIFO-queue
  waiter can't leave mid-wait -- so masking-out-of-contention is the right mechanism.) HONEST SCOPE: this
  buys CLUSTER-SURVIVABILITY (cores 1-3 + IRQ-driven services stay live), NOT user-responsiveness (all user
  work + the net stack are pinned to core 0, which still wedges 32.4s). SAFE only under core-0 pinning; MUST
  be 0 if coresched is ever used (cores 1-3 would lose preemption).
- **MVD-1** (timer-masked core-1 watchdog + core-0 heartbeat + PM hwdog auto-reset, default-ON) = the
  user-responsiveness CEILING: survive + report out-of-band + auto-reset a total wedge.

## SESSION-8 LATE: netconsole mitigation (lead #5) -- partial + a key insight
- **Committed (`5a929d1`): netconsole self-recovers its listening socket** -- after N consecutive accept()
  failures it rebuilds the listener (close + re-socket/bind/listen via `open_listener()`), and on a
  persistent failure exits so getty's supervisor respawns it. Also fixes a latent busy-spin (the old
  `if(cfd<0) continue;` looped with no nap, pinning core 0). **NOT yet live on the board** -- the deploy
  was incomplete (see below). It is in `build-04/sbase/netconsole` (built via `scripts/aios-cc`).
- **KEY INSIGHT (likely re-frames lead #5): the real "netconsole wedges under churn" mode is probably
  FORK-EXHAUSTION, not listener corruption.** Driving netconsole hard (each command forks a `dash`) +
  a deploy pushed the box past the graceful fork ceiling and it did NOT reap back: every command returned
  `[netcon: fork failed]`, persistently, and a fork-exhausted box CANNOT reboot itself (reboot needs fork)
  -> needed a physical power-cycle. So the listener-rebuild fix addresses a DIFFERENT mode; the churn-wedge
  is the box running out of fork capacity (forks not reaping after a connection storm). **The high-value
  mitigation target is the fork-reaping path under churn**, not the listener.

## NEXT LEADS (ranked)
1. **Re-deploy the netconsole listener fix + make it live** (committed but not on the board):
   `python3 scripts/pi_deploy.py build-04/sbase/netconsole /bin/netconsole --reboot` (works on a FRESH
   board; do NOT over-probe netconsole first -- the probing itself fork-exhausts it). Then ONE smoke-test.
2. **Investigate FORK-EXHAUSTION under churn (the real netconsole-churn-wedge).** Why don't forks reap
   back after a connection storm? Trace the reaper / waitpid / the per-process slot free path (the v0.4.263
   graceful fork ceiling makes it FAIL gracefully but it should RECOVER as forks reap -- it didn't). A
   fork-capacity auto-recovery (or fixing a fork/slot leak) would cure the churn-wedge that costs
   power-cycles. THIS is the highest-value mitigation.
3. **Stop the BKL cascade for IRQ-driven services too** (extend the circumvention): with the sibling
   timer-mask, cores 1-3 survive a wedge -- verify that an IRQ-driven secondary service (if any) actually
   stays responsive, to bank "cluster survives" as a real reliability property.
4. **Documented-but-blocked cures** (only if the fabric situation changes): fine-grained-locking BKL
   removal [infeasible]; a working coresched [blocked by the per-core fabric stall]; a VPU-firmware/config.txt
   fabric-timeout knob [none found publicly].

## METHOD / DISCIPLINE (hard-won, session 8)
- **Run `scripts/sercap.py /tmp/x.log` (serial) BEFORE any HW stall test** -- `[STAGECP]`/`[WDOG]` are
  serial-only. ONE serial reader.
- **Do NOT over-probe netconsole** -- each command forks a `dash`; rapid/repeated probing fork-exhausts the
  box (-> `[netcon: fork failed]` -> power-cycle). Use a HELD connection (`scripts/aios_nc.py`), drive
  gently, and pace probes.
- **Wait for `pi_flash.py --build` banner-PASS before testing**; never overlap a deploy with a test (a
  stall mid-push can wedge it; pi_flash/pi_deploy are atomic so the on-card image stays intact).
- Gold stall A/B = `pingmon` (ICMP GAP detector) + `netstall.py --idle 30 --trials 10`. The `[STAGECP]`
  `allidle=[c0,c1,c2,c3]` field is the cluster-survival oracle (siblings ~=0 = alive, ~=dur = BKL-blocked).
- Full QEMU gate (smp/shmring/socket on build-04; netd rebuilds build-netd) before every flash --
  green-equivalent baseline = smp 4/5 (W fork-correct + a host-load prompt-timeout shed), shmring 25/26,
  socket 8/8, netd 10/10. seL4 changes -> `deps/patches/seL4-kernel.patch` (regen `git -C deps/kernel diff
  > deps/patches/seL4-kernel.patch`). Commit on `main`; Bryan pushes.
- Board: **v0.4.289 build 2811 at 192.168.0.8** (freshly power-cycled; sibling-timer-mask ON, watchdog +
  hwdog ON; keep-warms default-OFF; the netconsole listener-fix is NOT yet live -- re-deploy it first).
  DHCP can bounce .8/.250/.197 -- ARP-sweep MAC dc:a6:32:1c:2e:e1 if .8 is dark.

---

## >>> SEED PROMPT (paste this) <<<

Continue the RPi4 idle-teardown STALL work. This is a MAJOR OPEN CONCERN -- NEVER frame it as
solved/concluded (memory [[feedback_stall_open_concern]]).

READ FIRST: docs/NEXT_20260621_stall_session9_seed.md (full state above), then HANDOVER.md (CURRENT STATE
session-8, top), then memory [[project_stall_hunt]] + [[feedback_stall_open_concern]].

SETTLED (do NOT re-derive): the ~32.4s freeze is core 0 clocked-but-wedged on a single fabric/DVM-completion
instruction after the BCM2711 SCB fabric quiesces during idle, POSITION-INDEPENDENT (4 sites), CLOCK-
INDEPENDENT (a fixed silicon force-complete, not a software-bounded timeout). The CURE is EXHAUSTED on every
software-reachable front: prevention dead (ALL keep-warms refuted -- DMA, 5.3GB/s coherent, Device,
cacheable; the quiesce is the DVM/coherency-completion path, no A72 traffic prevents it), timeout-bound dead
(no ARM-side register; GISB refuted; SCB/VPU-internal), user-responsiveness blocked (coresched wedges the
Pi) / infeasible (BKL removal = 2+yr proof rewrite). WHAT STANDS: decisive instruction-level localization;
the SIBLING-TIMER-MASK circumvention (v0.4.289, AIOS_SIBLING_TIMER_MASK) which makes the CLUSTER survive a
1-core wedge (cores 1-3 stay in idle.S, no BKL cascade) but NOT the box (all work pinned to core 0); and
MVD-1 (survive/report/auto-reset) as the user-responsiveness ceiling.

DO (ranked; pick with Bryan): (1) re-deploy the committed netconsole listener self-recovery fix
(`scripts/pi_deploy.py build-04/sbase/netconsole /bin/netconsole --reboot` on a FRESH board; don't
over-probe netconsole first -- it fork-exhausts) + ONE smoke-test. (2) INVESTIGATE FORK-EXHAUSTION under
churn -- the likely REAL "netconsole wedges under churn" mode: forks don't reap back after a connection
storm (-> `[netcon: fork failed]`, persistent, power-cycle needed); trace the reaper/waitpid/slot-free path
+ add a fork-capacity auto-recovery (the highest-value mitigation). (3) bank "cluster survives" as a
reliability property. (4) documented-but-blocked cures only if the fabric situation changes.

METHOD: sercap BEFORE any HW test; do NOT over-probe netconsole (pace it, use aios_nc.py held connections);
wait for pi_flash banner-PASS; gold A/B = pingmon + netstall --idle 30 --trials 10 ([STAGECP] allidle field
= cluster-survival oracle); full QEMU gate before every flash (smp 4/5 / shmring 25/26 / socket 8/8 / netd
10/10 baseline); seL4 changes -> deps/patches/seL4-kernel.patch; commit on main, Bryan pushes. Board
v0.4.289 build 2811 at 192.168.0.8 (freshly power-cycled; sibling-timer-mask ON, watchdog+hwdog ON; the
netconsole fix is NOT yet live -- re-deploy it first). KEEP: ASID-gen + coresched S1/S2 + watchdog-default-on
+ sibling-timer-mask + the [STAGECP]/[DSBSTALL]/[TLBISTALL] profilers + MVD-1 + the (default-off) keep-warm
A/B knobs (dma_warm, fabwarm modes 1-3).
