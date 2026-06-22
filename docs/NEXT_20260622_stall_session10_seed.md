# SEED PROMPT -- RPi4 stall: session 10 (the attributed MECHANISM is overturned; re-localize in AIOS)

Paste the SEED PROMPT block at the bottom into a fresh session. Everything above it is grounding. The stall
is a **MAJOR OPEN CONCERN -- never frame it as solved/concluded** ([[feedback_stall_open_concern]]). Session 9
did NOT cure it; it FALSIFIED the leading hypothesis, which is real progress but leaves the freeze unsolved.

---

## SESSION 9 HEADLINE -- the stall's attributed mechanism is OVERTURNED by experiment

Built bare-metal + Linux reproduction harnesses and tested the long-held mechanism directly. **A broadcast
inner-shareable TLBI DVM-Sync (`tlbi vmalle1is/alle2is/vae2is; dsb sy`) AND a cold cacheable DRAM load,
issued AFTER idle of 10-240s, complete in 0ms on this exact Pi4 -- under BOTH minimal bare-metal (EL2) AND
full Linux/RPi OS (EL1), busy-spin idle AND genuine WFI idle, the TLBI even in the timer hard-IRQ wake
context (== AIOS's kernel-exit -> idle -> IRQ-wake -> first-fabric-op path). Nothing hangs.** Detail +
trial logs: `docs/NEXT_20260622_linux_uboot_stall_experiments.md`; memory [[project_stall_not_dvm_idle]].

**=> The ~32.4s freeze is NOT "the BCM2711 SCB quiesces during idle and the first fabric/DVM op after idle
hangs."** That op, done deliberately after real idle, is fine here. **The clincher is logic, not one test:
AIOS has MORE ARM activity during its "idle" (prio-200 servers yield-spinning -> BKL + scheduler coherent
traffic) than Linux's WFI idle, yet AIOS hangs and Linux does not.** If the trigger were "no ARM traffic ->
SCB quiesces -> next op hangs," the quieter Linux idle would hang MORE, not less. So:
- The trigger is **NOT idle-driven SCB quiescence.** It is **AIOS/seL4-environment-specific, not a context-
  free silicon property.**
- This re-frames the keep-warm refutations (they fought a mechanism that doesn't hold) AND the [STAGECP]/PMU
  localization: "the first fabric op after idle" is real, but it is **NOT a generic fabric op** -- it's a
  specific seL4 code path / MMU-ASID state / interaction the experiments could not isolate.

## OTHER SESSION-9 DELIVERABLES
- **Board RECOVERED.** The Pi's `/bin/netconsole` was a corrupt 4095-byte truncated binary (prior aborted
  deploy) -> getty flap + stall storm + undeployable. Recovered via **sshd:2222 SFTP direct-put** (the
  netconsole deploy channel was down); deployed the committed netconsole listener fix BYTE-EXACT (sha
  da907d2e). Playbook: [[project_board_recovery_sftp]].
- **pi_deploy `__get` over-read FIXED + pushed** (commit `8311601`): `get()` read 6 trailing prompt bytes
  ("aios# ") past the declared length -> spurious "sha mismatch". Now reads exactly N. (This bug caused a
  long red-herring during the recovery.)
- **Reusable infrastructure (KEEP):** `experiments/e1_repro/` (standalone bare-metal `kernel8.img` for this
  Pi -- 4-core spin-table bring-up, mirrors AIOS SMPEN/L2ACTLR/MMU; `build.sh` + `mkbootimg.py`);
  `experiments/e3_linux/` (RPi OS modules: `e3_dvm_test.c` stop_machine, `e3_wfi.c` hrtimer hard-IRQ wake).

## BOARD / DUAL-BOOT STATE
- **Same physical Pi4, two SDs (swap to switch OS):** the AIOS SD (v0.4.289, recovered, netconsole fix live)
  and a SPARE SD with **Raspberry Pi OS** (kernel 6.18, IP **192.168.0.8**). The Pi currently has the RPi OS
  SD in it (left running E3). To resume AIOS work, power off and swap to the AIOS SD.
- **RPi OS access:** serial console is OFF by default -> read via `dmesg`/ssh. **Keyless ssh works** (an
  ed25519 key was added to `bryan@.../authorized_keys`; user `bryan`, passwordless sudo). Plain
  `ssh bryan@192.168.0.8 'cmd'` is reliable; expect-over-ssh password automation was flaky. The E3 modules
  live in `~/e3` (built against kernel 6.18 headers).
- **WATCHDOG:** systemd arms the bcm2835 hw watchdog at RuntimeWatchdogSec=1min -> any >60s freeze RESETS
  the Pi. It was DISABLED via a drop-in (`/etc/systemd/system.conf.d/99-nowatchdog.conf`,
  `RuntimeWatchdogSec=0` + `daemon-reexec`). Re-enable / clean up RPi OS if done with E3 (rm the drop-in).
- E1 swap on the spare SD: `cp config.txt.rpios/kernel8.img.rpios -> config.txt/kernel8.img` restores RPi OS;
  the reverse (the e1_repro kernel8.img + its config.txt) boots the bare-metal reproducer.

## NEXT LEADS (ranked; pick with Bryan)
1. **RE-LOCALIZE the wedge in AIOS, register-exact** (the deferred s8 Phase-2, now mandatory). With "a
   deliberate fabric op after idle does NOT hang" as a hard constraint, the [STAGECP] window (kernel-exit ->
   ...32.4s... -> re-entry) must be bisected to the EXACT instruction with register-safe asm checkpoints
   bracketing `restore_user_context`'s `ldp`/`eret` + a `schedule()`/`activateThread()` checkpoint + PMU
   L2D_CACHE_REFILL/MEM_ACCESS. Goal: identify what the wedge ACTUALLY is (it is not a generic DVM-Sync/cold
   load). Needs the AIOS SD + a gentle HW session (sercap FIRST). This is the "what IS it" investigation.
2. **Fork-exhaustion auto-recovery** (carried from the session-9 seed, NOT done -- the session went to
   recovery + experiments). Highest-value MITIGATION: wire up the DEAD `reap_check()` (`src/process/reap.c:113`,
   zero call sites) as a sweep-on-shortage + retry in `do_fork` (mirror the `reclaim_orphaned_pipes()` retry
   at pipe_server.c:1324), + add a **getty crash-loop circuit-breaker** (getty respawns a crashing service
   forever -> a corrupt binary self-sustains a stall storm; this session's recovery exposed it). Confirmed:
   exit is a deliberate fault (`posix_proc.c:30`) so reaps are always TRIGGERED; the wedge is pipe_server
   starvation or a real per-fork leak, not lost faults. /proc slot/zombie/VKA counter to observe a storm.
3. **E4 -- AIOS-vs-Linux fabric/clock/power-domain register diff** (now that the stall is OS-specific). The
   target is fuzzier post-overturn, but `clk_summary` + `pm_genpd_summary` + the A72 IMP-DEF regs diffed
   AIOS-vs-Linux could surface a config AIOS sets that Linux doesn't. Lower priority than (1).
4. **Documented-but-blocked cures** only if the picture changes: fine-grained-locking BKL removal [2+yr];
   a working coresched [blocked]; a VPU-firmware knob [none found].

## METHOD / DISCIPLINE (hard-won)
- The stall stays a MAJOR OPEN CONCERN -- never conclude it ([[feedback_stall_open_concern]]).
- AIOS HW: `scripts/sercap.py /tmp/x.log` (serial) BEFORE any stall test; ONE serial reader; do NOT
  over-probe netconsole (fork-exhausts -> power-cycle); wait for pi_flash banner-PASS; gold A/B = pingmon +
  `netstall.py --idle 30 --trials 10`; full QEMU gate before every flash; seL4 -> deps/patches/seL4-kernel.patch.
- RPi OS HW: keyless ssh + passwordless sudo; DISABLE the bcm2835 watchdog before any freeze test (else
  reset). A stop_machine CNTVCT busy-spin is NOT WFI (won't quiesce the SCB) -- the e3_wfi hrtimer is faithful.
- Commit on `main`; Bryan pushes. Session-9 experiment files (docs/NEXT_20260622_*, experiments/e1_repro/,
  experiments/e3_linux/) are UNCOMMITTED (new); the pi_deploy fix is committed+pushed (8311601).

---

## >>> SEED PROMPT (paste this) <<<

Continue the RPi4 STALL work. This is a MAJOR OPEN CONCERN -- NEVER frame it as solved/concluded (memory
[[feedback_stall_open_concern]]).

READ FIRST: docs/NEXT_20260622_stall_session10_seed.md (full state above), then HANDOVER.md (CURRENT STATE,
top), then memories [[project_stall_not_dvm_idle]] (session-9 headline) + [[project_stall_hunt]] +
[[feedback_stall_open_concern]] + [[project_board_recovery_sftp]].

SETTLED (session 9, do NOT re-derive): the ~32.4s freeze's attributed mechanism is OVERTURNED by direct
experiment. A broadcast TLBI DVM-Sync (tlbi vmalle1is/alle2is/vae2is; dsb) AND a cold cacheable load, issued
AFTER idle of 10-240s, complete in 0ms on this exact Pi4 -- under minimal bare-metal (EL2) AND full Linux
(EL1), busy-spin AND genuine WFI idle, the TLBI even in the timer hard-IRQ wake context. So the freeze is NOT
"the SCB quiesces during idle and the first fabric/DVM op after idle hangs" -- the clincher: AIOS has MORE ARM
activity during idle than Linux's WFI idle yet AIOS hangs and Linux does not (backwards from the no-traffic
-> quiesce model). The stall is AIOS/seL4-environment-specific, NOT a context-free silicon property; the
[STAGECP]/PMU "first fabric op after idle" is real but is NOT a generic fabric op. Infra built:
experiments/e1_repro (bare-metal kernel8.img) + experiments/e3_linux (modules). Dual-boot Pi: AIOS SD +
spare RPi OS SD (192.168.0.8, keyless ssh, bcm2835 watchdog disabled, ~/e3 modules); the Pi currently runs
RPi OS -- swap to the AIOS SD to resume AIOS work.

DO (ranked; pick with Bryan): (1) RE-LOCALIZE the wedge in AIOS register-exact (the deferred Phase-2) -- with
"a deliberate fabric op after idle does not hang" as a hard constraint, bracket restore_user_context's
ldp/eret + schedule()/activateThread() with register-safe asm checkpoints + add L2D_CACHE_REFILL/MEM_ACCESS
to the PMU print, to find what the wedge ACTUALLY is. (2) FORK-EXHAUSTION auto-recovery (carried over, NOT
done): wire the dead reap_check() (reap.c:113) as sweep-on-shortage+retry in do_fork + a getty crash-loop
circuit-breaker + a /proc slot/zombie/VKA counter -- the highest-value mitigation. (3) E4 AIOS-vs-Linux
register/clock/power-domain diff. (4) documented-but-blocked cures only if the picture changes.

METHOD: stall = MAJOR OPEN CONCERN, never conclude. AIOS HW: sercap BEFORE any stall test; don't over-probe
netconsole; wait for pi_flash banner-PASS; gold A/B = pingmon + netstall --idle 30 --trials 10; full QEMU
gate before every flash; seL4 -> deps/patches/seL4-kernel.patch. RPi OS HW: keyless ssh + passwordless sudo;
DISABLE the bcm2835 watchdog before freeze tests. Commit on main; Bryan pushes. Session-9 experiment files
are uncommitted (Bryan: commit/push experiments/ + docs/NEXT_20260622_* when ready). KEEP: ASID-gen +
coresched S1/S2 + watchdog-default-on + sibling-timer-mask + [STAGECP]/[DSBSTALL]/[TLBISTALL] profilers +
MVD-1 + the (default-off) keep-warm A/B knobs + experiments/e1_repro + experiments/e3_linux.
