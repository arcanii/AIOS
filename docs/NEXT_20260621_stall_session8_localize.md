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

## Status
Built (v0.4.286), QEMU gate run, kernel diff in `deps/patches/seL4-kernel.patch`.
KEPT: ASID-gen + coresched S1/S2 + watchdog default-on + the [TLBISTALL]/[DSBSTALL]/[STAGECP]
profilers + MVD-1. The freeze is NOT cured -- this is a measurement step toward the architectural fix.
