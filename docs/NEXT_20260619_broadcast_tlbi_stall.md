# NEXT (seed) 2026-06-19 -- broadcast-TLBI A/B is DONE (REFUTED); the stall hunt continues

## RESULT (2026-06-19): broadcast TLBI is REFUTED -- NOT the cure
Ran the A/B on build 2617 (broadcast `tlbi vae1is`) with the gold detector. The freeze
STILL happens: **pingmon GAP #1 = 33.3s (ping-confirmed whole-system freeze) + netstall
trial-0 severe stall (105s NO-PROMPT / conn-death) in the FIRST 2 trials.** A cure needed
0 freezes; it froze almost immediately. So the ~33s stall is **INDEPENDENT of the TLBI
scope** (local `vae1` vs broadcast `vae1is`) -- the hypothesis (local path cold-gates, the
broadcast path stays warm) is wrong. The freeze mechanism is deeper than the TLBI variant.

Reverted: `deps/kernel/.../machine.h` `AIOS_TLBI_BROADCAST` is commented again (back to
local `vae1`), with a REFUTED note in-place. The Pi still RUNS the broadcast kernel (build
2617) -- it is HARMLESS (identical stall behaviour to local), so reverting the board is
cosmetic; flash `disk/kernel8_v264.img` (the committed local baseline) to put it back on a
committed kernel when convenient. No version bump (the experiment is discarded).

## THE IMMEDIATE TASK is now the remaining candidates below (see "remaining stall candidates").

---

## (historical) THE TASK WAS: run the broadcast-TLBI A/B (now done -- see RESULT above)

The RPi4 ~33s idle-teardown TLBI/DVM freeze (the long-standing residual, ~2.5% of teardown-after-idle)
has had every other lever ruled out: A72-register config, L2-clock force (B/B+ refuted, B+ harmful),
and the BCM2711 SCB/UBUS fabric-timeout register (DEAD END -- no such reg, two primary-source passes;
0x80000@16.2kHz matches no real clock -- see [[project_stall_ubus_deadend]] +
docs/NEXT_20260619_ubus_register_deadend.md).

The strongest remaining clue: **Linux on this exact Pi4 never has the freeze, and Linux SMP uses the
inner-shareable BROADCAST `tlbi vae1is`, while AIOS used LOCAL `tlbi vae1`** (core-0 pinning makes local
sufficient for correctness). Hypothesis: the broadcast DVM path stays warm from constant 4-core coherency
traffic, while the local-only path cold-gates after idle and its trailing `dsb` hangs to the ~33s timeout.

**Candidate C (broadcast TLBI) is ENABLED + BUILT + GATE-GREEN + FLASHED + RUNNING:**
- `deps/kernel/include/arch/arm/arch/64/mode/machine.h`: `#define AIOS_TLBI_BROADCAST 1` (uncommented).
  `invalidateLocalTLB_VAASID` now emits `tlbi vae1is` (objdump-verified: 3x vae1is, 0 local vae1).
- Pi runs it NOW: **v0.4.264 build 2617** (banner-verified). Baseline (local TLBI) = build 2615 /
  `disk/kernel8_v264.img`. Broadcast = `disk/kernel8_v264_bcast.img`.
- QEMU gate GREEN on the broadcast kernel: smp 7/7, shmring 26/26, socket 8/8, netd 10/10.
- The kernel change is in the SIBLING seL4 tree, UNCOMMITTED (it is an experiment).

### DO THIS:
1. Confirm the box is healthy: `python3 scripts/aios_nc.py --host 192.168.0.8 "cat /proc/version"`
   (expect build 2617) + `echo ok`.
2. Run the A/B with the gold freeze detector:
   - `python3 -u /tmp/pingmon.py &` (recreate it if gone -- see project_stall_hunt; pings .8, logs GAP>4s)
   - `python3 -u scripts/netstall.py --host 192.168.0.8 --idle 30 --trials 60`
   - A ping GAP coinciding with a netstall stall = a REAL ~33s whole-system freeze.
3. Interpret vs the local-TLBI baseline (~2.5% = ~1-2 per 60, 33s each):
   - **0 freezes / 60+** -> promising; extend to 120 trials (0/120 at 2.5% is ~5% by luck) to be confident.
   - **>=1 freeze / 60 (33s + ping GAP)** -> broadcast does NOT cure it; the local-vs-broadcast variant is
     irrelevant (the fabric cold-gates regardless). Done -- revert.
4. **Decision:**
   - CURES it (0/120-ish) -> KEEP: commit the kernel change (sibling seL4 tree) + capture into
     `deps/patches/seL4-kernel.patch` (`scripts/build_environment.sh --capture-patches`, or hand-edit) +
     bump version.h to 265 + update memory/HANDOVER. THIS WOULD BE THE STALL CURE.
   - Does NOT cure -> revert (comment `#define AIOS_TLBI_BROADCAST`), rebuild, flash back
     `disk/kernel8_v264.img` (local baseline), note the negative in project_stall_hunt.

## DRIVE THE PI WITH THE NEW TOOLS (do not repeat this session's mistakes)
- **`scripts/aios_nc.py`** -- robust HELD-connection netconsole driver. USE IT (one connection, many
  commands; 50s timeout rides one ~33s stall). DO NOT reconnect-per-command (picmd-style) -- that is the
  back-to-back-conn wedge pattern.
- **`scripts/pi_deploy.py <local> /bin/<name>`** -- ATOMIC disk-binary deploy (non-forking __put to .tmp
  + __get verify + atomic rename). USE IT for /bin deploys. NEVER `cp /tmp/x /bin/x` over netconsole and
  NEVER raw `__put` directly to a live `/bin` path -- a forked cp / a killed __put gets stall-killed
  mid-write -> PARTIAL binary -> getty crash-loops the corrupt service -> ~33s-stall storm + USB death +
  fork-pool exhaustion (this session burned ~1h on exactly this).
- Recovery if a service binary is corrupted: **SERIAL** `cp /tmp/<good> /bin/<name>` (serial rides stalls,
  no netconsole 30s SIGKILL). `/tmp` PERSISTS across reboot (it is ext2, not ramfs), so pushed binaries
  survive. If the box is fork-exhausted, only a power-cycle resets it (reboot itself needs to fork).
- Discipline: FULL QEMU gate (smp/shmring/socket/netd) before any flash; the ~33s stall makes the box
  fragile exactly when stressed, so keep netconsole driving gentle.

## IF BROADCAST FAILS -- remaining stall candidates (in order)
1. **Verify CPUECTLR.SMPEN on cores 1-3** (not just core 0 -- the boot probe only read core 0). If a
   secondary is not fully in the inner-shareable/DVM domain, core 0's tlbi completion hangs on it. Extend
   the errata.c aios_a72_probe to run on / read the secondaries.
2. **Deep AIOS-vs-Linux teardown/coherency diff** -- the cache-maintenance / barrier / armstub-EL3 setup
   Linux does around teardown that AIOS does not. The principled cure; multi-session RE.
3. **Eliminate the teardown TLBI** -- defer ASID invalidation to ASID-reuse time (box active, not idle)
   instead of per-page at exit. Kernel correctness-sensitive.
- Mitigations already shipped (keep): nodes=4, masked TLB shootdown, clock floor 600 (caps severity).

## SECONDARY (lower priority)
- `pi_deploy.py` clean end-to-end "DEPLOY OK" HW run (pending a quiet, non-stall-storming box -- its
  atomic design is proven, but it never got a clean run because the box was stall-storming).
- The stall observability option (a /proc counter of freeze occurrences) if you want to measure the real
  in-use rate.

## COMMITS this session (local on main; Bryan pushes)
3acb910 v0.4.262 (build-time + 1000MHz + floor + UBUS dead-end) ; caa2df8 (v262 HW-verify) ;
840ad4d v0.4.263 (graceful spawn) ; 2b7a362 (aios_nc.py) ; b391eca v0.4.264 (netconsole accept-pace) ;
819ba10 (pi_deploy.py). PLUS the broadcast-TLBI change in the sibling seL4 tree (UNCOMMITTED -- decide
via the A/B). Pi at 192.168.0.8 = v0.4.264 build 2617 (broadcast), 1000MHz, healthy.
