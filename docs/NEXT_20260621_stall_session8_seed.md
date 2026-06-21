# SEED PROMPT -- RPi4 idle-teardown stall: continue the search for answers / mitigation (session 8)

Paste the SEED PROMPT block below into a fresh session. Everything above it is the grounding the
prompt refers to. The stall is a **MAJOR OPEN CONCERN -- never frame it as concluded/solved**
([[feedback_stall_open_concern]]); mitigation (MVD-1 watchdog) is NOT a cure.

---

## DEFINITIVE STATE (what session 7 established -- do NOT re-derive)

**The freeze:** a ~32.4s (deterministic to the ms: 32399ms) WHOLE-SYSTEM freeze on the RPi4 /
BCM2711 (Cortex-A72, AArch64, non-hyp, SMP=4, all user threads pinned to core 0). It is the BCM2711
SCB-fabric DVM-Complete wait that hangs when the fabric quiesces after idle; core 0 holds the Big
Kernel Lock during it so the whole box is unresponsive. PMU: core 0 CLOCKED-BUT-WEDGED on ONE
instruction (a `dsb`), a clean wait on a parked fabric.

**THE BIG SESSION-7 RESULT -- the multi-session "the teardown `tlbi` is the cause" PREMISE IS
OVERTURNED. The freeze is the IDLE->WAKE transition, NOT process teardown.** Proven by a checkpoint
breadcrumb (`[STAGECP]`, errata.c `aios_checkpoint` + per-stage calls): EVERY freeze is
`prev=11 this=10 dur=~32400ms` = the 32.4s sits BETWEEN kernel-exit (`restore_user_context`, stage
11) and the next IRQ entry (`c_handle_interrupt`, stage 10) -- the idle->wake window -- with NO
teardown stage (1-6: unmapPage/unmapPageTable) around it. CLUSTER-WIDE: cores 2,3 (idle-only) show
`hb_lag ~= dur` (idle-loop heartbeat stale the whole freeze; they block on the BKL while core 0
wedges). idle.S has no wedging instruction (yield + CNTPCT + per-core str + branch).

**REFUTED as the cause (all HW, this session + prior):** the teardown `tlbi` (ASID-gen removed it ->
ZERO [TLBISTALL], stall rate UNCHANGED 5-7/10); the teardown cache-clean `dc cvau`
(AIOS_TEARDOWN_NO_CLEAN skip -> rate unchanged); the context-switch `dsb` ([DSBSTALL] probe on
setCurrentUserVSpaceRoot stayed SILENT). PLUS every prior lever: all CPU keep-warms (GPLEV0, GENET
MDIO = Linux's PHY_POLL, V3D DMA, beacon), every clock/voltage (core_freq, over_voltage, L2ACTLR /
CPUACTLR clock-force), every A72/TLBI-ISA variant (local vs broadcast tlbi, dsb sy vs nsh, SMPEN
cores 1-3, DVM-broadcast-disable). => **fabric-quiescence FUNDAMENTAL at idle->wake; NOT a removable
seL4 instruction.**

**Mitigation posture (shipped, HW-verified):** MVD-1 watchdog + PM HW-watchdog auto-reset
DEFAULT-ON (v0.4.284, `5d4c56d`): timer-masked pure-userspace core-1 watchdog detects + reports each
freeze out-of-band on the mini-UART, and a TOTAL wedge auto-resets the SoC in ~63s. The box no
longer goes silently dark. `/proc/watchdog`, `/proc/watchdog.0` / `.hwdog.0` to disable.

**Coresched coupling (session-7 HW finding):** UNPIN is BLOCKED by the stall. Enabling
`/proc/coresched.1` (round-robins user threads to cores 1-3 via aios_assign_core) reliably WEDGES
the Pi (reproducible, clean board) -- distributing work to secondaries exposes them to the same
cluster-wide idle->wake freeze + widens residency (teardowns IPI peers). ASID-gen coresched safety
S1 (reserved-asid at wrap) + S2 (peer-resident masked-tlbi) are committed + QEMU/host/review-
validated groundwork, but coresched is impractical until the stall is addressed. DO NOT enable
/proc/coresched.1 on the Pi without serial capture running.

**ASID-gen status:** built + gated `AIOS_ASID_GEN` (non-hyp/Pi only; QEMU build is HYP -> the gate
is regression-only; QEMU CANNOT exercise the ASID-gen path). Host-validated (scripts/
asidgen_host_test.c 4/4). It removed the teardown tlbi (real, kept) but is NEUTRAL on the stall.

## CONCRETE NEXT LEADS (ranked)

1. **Pinpoint the EXACT wedged instruction in the idle->wake window** (the satisfying close to the
   localization). Add `aios_checkpoint` calls BETWEEN stage 11 (restore_user_context) and stage 10
   (c_handle_interrupt): in the eret asm path, the EL1 exception vector (traps.S, before
   c_handle_interrupt), and c_entry_hook(). A big gap prev=11->X localizes to eret/idle/vector; X->10
   to c_entry_hook. Run `sercap` FIRST, then `netstall --idle 30`.
2. **Wedged-CPU vs IRQ-not-delivered.** Fix the `hb_lag` printf (the %d-on-64bit cosmetic bug masks
   core 0's value) OR have the breadcrumb read core 0's idle heartbeat: hb_lag~=dur => core 0 wedged
   on an instruction; hb_lag~=small => core 0 busy-idling fine but the TIMER IRQ never fires for
   32.4s (a GIC/timer/fabric IRQ-DELIVERY freeze). This is the key remaining mechanism question.
3. **Clean idle-isolation test** (does idle ALONE freeze, no teardown?). The pingmon-only test is
   FLAWED (its 1s ICMP prevents the 30s+ idle needed for quiescence). Instead: a single `netstall`-
   style trial that idles 30s then does ONE NON-teardown op (e.g. an in-server /proc read that does
   not spawn/exit a process), timed; OR an on-board test thread that idles then touches the fabric.
   If idle+non-teardown-wake freezes -> idle->wake confirmed independent of teardown.
4. **Fabric-level / external angles** (uncharted, speculative -- prior analysis says CPU-side cannot
   warm the SCB DVM path; only a COHERENT external bus-master could): a GPU/V3D or DMA-engine
   heartbeat that issues COHERENT (DVM-domain) traffic during idle; an EL2/VideoCore-firmware fabric-
   retention knob; re-mine the BCM2711 SCB/UBUS timeout register (none writable found, but the source
   of the fixed ~32.4s is still undocumented). Treat as research, not a quick win.
5. **Mitigation polish:** the netconsole WEDGES under connection churn / after a stall (needs a
   power-cycle to recover) -- a watchdog-driven netconsole/getty recovery would make the box truly
   unattended. Tune WD_STALL_MS / the hwdog timeout if useful.

## METHOD / DISCIPLINE (hard-won)
- **Run `scripts/sercap.py /tmp/x.log` (serial capture) BEFORE any HW stall/coresched test** -- the
  [ASIDGEN-BUG]/[STAGECP]/[TLBISTALL]/[DSBSTALL]/[WDOG] signals are serial-only. ONE serial reader.
- **Wait for `pi_flash.py --build` banner-PASS before testing** -- never overlap a deploy with a test
  (it churns the netconsole + a mid-deploy stall drags it out -> wedge). Gold A/B = `pingmon`
  (ICMP GAP detector) + `netstall.py --idle 30` with `--timeout 75` to ride stalls.
- **Netconsole wedges under churn** -> drive gently; recover with a power-cycle.
- QEMU gate (smp/shmring/socket/netd on build-04) before every flash = REGRESSION only for ASID-gen
  (build-04 is HYP). Commit on `main`; Bryan pushes. Board: v0.4.285 build 2774 at 192.168.0.8
  (netconsole-wedged after the coresched test -> power-cycle for a clean board).

---

## >>> SEED PROMPT (paste this) <<<

Continue the RPi4 idle-teardown STALL hunt -- searching for ANSWERS and/or better MITIGATION. This
is a MAJOR OPEN CONCERN; NEVER frame it as concluded/solved (memory [[feedback_stall_open_concern]]).

READ FIRST: docs/NEXT_20260621_stall_session8_seed.md (full state above), then HANDOVER.md (CURRENT
STATE session-7, top), then memory [[project_stall_hunt]] + [[project_asid_generation]] +
[[feedback_stall_open_concern]].

SETTLED (do NOT re-derive): the ~32.4s freeze is LOCALIZED to the IDLE->WAKE window (kernel-exit ->
next IRQ-entry, `[STAGECP] prev=11 this=10`), CLUSTER-WIDE, NOT process teardown. The "teardown tlbi
is the cause" premise is REFUTED (ASID-gen removed the tlbi, stall unchanged; cache-clean removal +
context-switch dsb also refuted). It is fabric-quiescence-FUNDAMENTAL (BCM2711 SCB DVM-Complete after
idle), not a removable seL4 instruction. MVD-1 watchdog + hwdog auto-reset ship DEFAULT-ON
(survive+report+auto-recover). Coresched/unpin is BLOCKED by the stall (enabling /proc/coresched.1
wedges the Pi).

DO (ranked; pick with Bryan): (1) PINPOINT the exact wedged instruction -- add aios_checkpoint stages
between restore_user_context (11) and c_handle_interrupt (10): the eret asm, the EL1 exception
vector (traps.S), c_entry_hook. (2) Resolve wedged-CPU-vs-IRQ-not-delivered (fix the hb_lag printf /
read core 0's idle heartbeat during the freeze). (3) A CLEAN idle-isolation test (idle 30s then ONE
non-teardown op -- the pingmon-only test is flawed). (4) Research the fabric/external angles (a
COHERENT external bus-master heartbeat; an EL2/firmware retention knob; the undocumented ~32.4s SCB
timeout). (5) Mitigation polish (watchdog-driven netconsole/getty recovery so the box is truly
unattended).

METHOD: run sercap (serial) BEFORE any HW test; wait for pi_flash banner-PASS before testing; gold
A/B = pingmon + netstall --idle 30 (--timeout 75 to ride stalls); netconsole wedges under churn
(power-cycle to recover); full QEMU gate before every flash (regression-only for the non-hyp ASID-gen
path); commit on main, Bryan pushes. Board v0.4.285 at 192.168.0.8 -- POWER-CYCLE it first (it is
netconsole-wedged from the session-7 coresched test). seL4 changes go in deps/patches/
seL4-kernel.patch; KEEP the ASID-gen + S1/S2 + watchdog-default-on + the [STAGECP]/[DSBSTALL]/
[TLBISTALL] profilers + MVD-1.
