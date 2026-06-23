# Session-14 PREWARM confirmation -- PRE-REGISTERED protocol (written before any board run)

Goal: CONFIRM or REFUTE that `AIOS_FABRIC_PREWARM` (pre-NODE_LOCK cold-fabric touch at each seL4
kernel entry, commit 0a4d045) prevents the ~32.4s idle->wake core-0 wedge. Session 13 saw 0 wedges
with prewarm vs ~6 without over ~14 cycles. Treat as UNCONFIRMED; demand extraordinary proof.

## The measurement (serial-independent)
`/proc/laststall` reports `total=N` = `g_wd_stalls`, incremented by the PURE-USERSPACE core-1
watchdog (timer-masked, survives the BKL stall) whenever core 0's heartbeat ages past
`WD_STALL_MS=9000ms`. A real wedge is ~32.4s, far above 9s -> clean detection. The counter counts
ANY ~32s core-0 freeze regardless of wedge type or BKL state, so:
  - laststall total stays 0  <=> core 0 never froze  <=> prewarm PREVENTED the wedge.
  - laststall total climbs    <=> core 0 froze ~32s   <=> wedge occurred.
This distinguishes PREVENTION (the cure claim) from mere relocate/confine (a confined wedge still
freezes core 0 pre-lock -> still counted). Session-13's 0 means prevention, not confinement.

## Drive = one idle->teardown cycle
The wedge is teardown-after-idle (calm board => zero wedges; the trigger is a process teardown that
is the first fabric op after a ~30s quiet window). One cycle:
  1. idle T seconds with NO netconsole connection (board quiet -> BCM2711 SCB fabric parks).
  2. connect to :2323 (forks a shell), `cat /proc/laststall` (reads the running total), disconnect.
  3. the disconnect's shell teardown-after-idle is the trigger; its outcome shows up in the NEXT
     read. N reads = N triggers; final read - first read = wedges observed.

## Confounds and how this protocol controls each
- C1 builds differ in more than prewarm: the A/B/A flips ONLY `AIOS_FABRIC_PREWARM` 0/1 in the SAME
  source tree and rebuilds -> single variable. (Build number cosmetically increments; irrelevant.)
- C2 non-deterministic trigger: drive N>=20 cycles per arm. At the s13 ~43% base rate, 0/20 by
  chance has p=(0.57)^20 ~= 2e-5. The OFF arm is a POSITIVE CONTROL: it MUST produce wedges, else my
  drive does not trigger teardowns and the ON=0 result is meaningless.
- C3 monotonic drift (thermal/time/board age): A/B/A (ON, OFF, ON) -- if drift not prewarm caused
  ON=0, the middle OFF arm would also read 0. A real prewarm effect needs OFF>>0 between two ON~=0.
- C4 detector broken (false 0): the OFF arm proves the SAME detector reports wedges; sercap captures
  `[WDOG]`/`[STAGECP]`/`[reap]` independently as a cross-check; verify watchdog enabled=1 in preflight.
- C5 confine worker perturbs the wedge rate: PRIMARY A/B/A runs with the worker DISARMED (it is not
  needed for the laststall count and its BKL hammering could change the rate). A separate worker-ARMED
  run reproduces s13 exactly and adds the confinement signal.

## Procedure (in order)
0. Preflight: sercap to /tmp/s14_sercap.log (background, one reader). Read /proc/version,
   /proc/watchdog (enabled=1, hwdog status), /proc/laststall (baseline total), /proc/confine.
1. ARM-A0 (board is already on prewarm-ON build 2896): drive N=20, idle T=35s, worker DISARMED.
   Expect delta ~= 0.
2. OFF-B: set AIOS_FABRIC_PREWARM 0 in errata.c, regen patch, ninja -C build-rpi4, FULL QEMU gate
   (smp 4/5 baseline + socket 8/8), pi_flash.py --build, verify /proc/version. Drive N=20, T=35s,
   worker DISARMED. Expect delta >> 0 (positive control, ~8-9 at 43%). If 0 -> drive is broken, STOP
   and fix before trusting anything.
3. ON-A1: set AIOS_FABRIC_PREWARM 1, regen patch, rebuild, QEMU gate, flash, verify. Drive N=20,
   T=35s, worker DISARMED. Expect delta ~= 0.
4. SOAK (ON kernel still flashed): longer idle T=90s, N>=15. Expect total stays 0.
5. WORKER-ARMED confirmation (ON kernel): arm /proc/confine.2 (verify ticks climb), drive, confirm
   0 wedges AND replicate s13.
6. PERF: measure the dsb-sy-per-slowpath-entry cost (syscall round-trip latency ON vs OFF); if a
   lighter barrier (dsb ish, or no dsb) still prevents the wedge, prefer it.

## HARDENING after adversarial red-team (session 14, before any flash)
The kernel already compiles `AIOS_TEARDOWN_CHECKPOINTS=1` (machine.h:208) -> `aios_checkpoint`
prints `[STAGECP] core prev this dur=...ms` on ANY inter-stage gap >=5000ms (errata.c:499), below
the 9000ms watchdog threshold. So sercap is a SECOND, INDEPENDENT, real-time oracle:
  - PRIMARY = sercap: `[STAGECP]` (>=5s gaps, with prev/this = wedge TYPE), `[WDOG] STALLED/recovered`
    (dur), `[reap] destroy=...ms`. Immune to the off-by-one (real-time), catches sub-9s partial
    relocations the counter misses, and disaggregates wedge type (idle->wake prev=9 this=11; teardown
    prev=13 this=9; relocated-into-prewarm prev=14 this=9).
  - SECONDARY = /proc/laststall total: serial-independent backstop if the bump-sensitive FTDI drops.
This closes red-team confounds #1 (prevent-vs-relocate: a relocated 32s hang would STILL print a
[STAGECP] gap AND freeze the core-0 hb -> laststall++; only genuine prevention shows NEITHER), #2
(sub-threshold: [STAGECP]>=5s catches 5-9s hangs), #4 (off-by-one: sercap is real-time), #5
(entry-path: prev/this disaggregation). Deliberately NOT adding a g_wd_minor_stalls kernel counter --
that would perturb the ONLY variable under test; sercap gives the same signal externally.
Remaining live amendments: #3 OFF-arm robustness -- watch sercap during OFF-B; if <2 wedge events in
the first ~10 cycles, raise idle T to 60s and re-run. #6 detector smoke test -- OFF-B IS the positive
control; its wedges prove both oracles fire.

## Decision rule (pre-committed, sharpened by red-team)
PRIMARY metric per arm = count of distinct wedge events in sercap ([WDOG] STALLED or [STAGECP]
dur>=9000ms), cross-checked by /proc/laststall total delta. Both must agree (or sercap dropped).
- CONFIRMED (strong) iff: A0 ~= 0 AND B >> 0 (>=5 wedges, drive validated) AND A1 ~= 0, with the
  OFF/ON contrast holding under the SAME detector and drive. Soak strengthens.
- REFUTED iff: B ~= 0 (drive cannot reproduce wedges -> s13 was not a real contrast) OR A0/A1 show
  wedges comparable to B (prewarm does not prevent).
- INCONCLUSIVE otherwise (e.g. B small/noisy) -> increase N or T, do not conclude.
Even if CONFIRMED, validate broadly (workloads, power cycles) before declaring a cure -- 12 sessions
of premature cures.
