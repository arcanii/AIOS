# SMPEN-on-secondaries probe -- FINDINGS (2026-06-19)

Candidate (1) of the remaining RPi4 idle-teardown TLBI/DVM stall hunt. Seed:
`docs/NEXT_20260619_broadcast_tlbi_stall.md` ("remaining stall candidates"). Memory:
`[[project_stall_hunt]]`.

## RESULT -- REFUTED: all four cores are fully in the DVM domain

Extended the A72 IMP-DEF boot probe to read `CPUECTLR_EL1` on the **secondary** cores
(the existing `aios_a72_probe` only ever ran on the boot core). HW serial on the real
Pi4 (`/tmp/aios_serial.log`, after a `pi_flash` reboot of build 2622):

```
[A72PROBE] MIDR=410fd083                          <- Cortex-A72 r0p3
[A72PROBE] CPUECTLR_EL1=40 SMPEN=1 RET=0          <- core 0 (existing probe)
[A72PROBE] L2ACTLR_EL1=c000010 DVMDIS8=0 DSBNODVM11=0
[A72PROBE] core=2 CPUECTLR_EL1=40 SMPEN=1 RET=0
[A72PROBE] core=1 CPUECTLR_EL1=40 SMPEN=1 RET=0
[A72PROBE] core=3 CPUECTLR_EL1=40 SMPEN=1 RET=0
```

**All four cores are identical: `CPUECTLR_EL1=0x40`, SMPEN=1, dynamic-retention[2:0]=0.**
Every secondary boots fully in the inner-shareable/DVM coherency domain, with retention
off. The cluster-level `L2ACTLR=0xc000010` confirms DVM broadcast ENABLED (DVMDIS[8]=0)
and DSB-DVM-sync ON (DSBNODVM[11]=0); bits [27]+[26] are the adopted candidate-B
L2-clock-force.

### Hypothesis (now disproven)

The boot probe in the 2026-06-17 session read SMPEN only on core 0. If a *secondary*
had come up with SMPEN=0 (or in retention) -- not fully in the DVM domain -- then core
0's teardown `tlbi` completion (DVM) could hang waiting on that non-participating core,
to the ~33s BCM2711 fabric timeout. That would have unified every observation (nodes=4
helps by keeping the SCU clocked; broadcast can't help if a core is outside the domain;
local doesn't help either).

The probe disproves it directly: **no secondary sits outside the domain.** The RPi
armstub's spin-table release sets `CPUECTLR=0x40` (SMPEN only) + `ACTLR_EL3` (un-traps
EL1 IMP-DEF access) on *every* core, and seL4's secondary init leaves CPUECTLR untouched.
So all four cores are correctly configured at the per-core ISA level.

### Consequence

This was the **last A72 per-core domain lever.** Combined with the earlier refutations --
register-config gap (probe, core 0), L2-logic clock (candidate B), full-cluster clock
(candidate B+, harmful), broadcast-vs-local TLBI (candidate C) -- **every A72/TLBI-ISA
lever is now ruled out on hardware.** The ~33s idle-teardown freeze is conclusively below
the A72 ISA: a **BCM2711 SoC-fabric / SCU / UBUS DVM-completion quiescence**, not anything
the A72 cluster registers control.

## Method (reusable)

* `errata.c aios_a72_probe_secondary(word_t core)` -- reads `CPUECTLR_EL1` under the
  fault-survivable temporary-EL1-vtable path (so a trapping MRS reports TRAPPED, not a
  halt). Prints `[A72PROBE] core=N CPUECTLR_EL1=... SMPEN=... RET=...`.
* Called from `boot.c try_init_kernel_secondary_core` immediately after `NODE_LOCK_SYS`,
  so it runs **serialized under the big kernel lock** -- the shared trap flag and the UART
  are race-free across the three secondaries, and the primary is spinning silently in
  `release_secondary_cpus()` for that whole window. (QEMU a53 confirms the path executes
  harmlessly: three `core=N ... (not A72)` lines, MIDR-guarded skip.)
* Observability: the live serial monitor
  (`aios_console.py monitor /dev/cu.usbserial-0001 --mirror /tmp/aios_serial.log`)
  captures the early-boot `[A72PROBE]` lines across a `pi_flash` reboot. Only the new
  kernel emits `core=N` lines, so `grep 'A72PROBE.*core='` is an unambiguous fresh-boot
  discriminator.

## Disposition

* Probe KEPT as a permanent all-core diagnostic (parallel to the core-0 probe). Captured
  into `deps/patches/seL4-kernel.patch`. NO version bump (v0.4.264; running build 2622 =
  the committed local-vae1 baseline + the all-core probe).
* No stall A/B run: this is a direct register refutation, not a fix to soak -- and it
  spares the stall-fragile box.

## NEXT -- only candidate (2) remains

**Deep AIOS-vs-Linux teardown/coherency-setup diff.** Linux on this exact Pi4 never
freezes. Find the cache-maintenance / barrier / armstub-EL3 / DVM-quiescence handling
Linux does around process teardown and core idle that AIOS/seL4 does not. Likely angles:
the armstub EL3 setup (SCR/ACTLR/CPU power), the Linux arm64 teardown TLBI+DSB sequence
vs seL4's, and whether Linux ever lets the SCU/fabric reach the quiescent state that AIOS
hits after idle. Multi-session RE; the principled cure. Mitigations already shipped and
kept (contain it to ~2.5% for normal use): nodes=4, masked TLB shootdown, clock floor 600.
