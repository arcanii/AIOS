# NEXT (seed): close the RPi4 idle-teardown TLBI stall via A72 CPUECTLR/cluster config (the Linux gap)

Self-contained handoff for a FRESH session. Repo `~/Desktop/github_repos/AIOS`, branch `main`. Kernel
source is the sibling `~/Desktop/github_repos/seL4` (symlinked `deps/kernel`). Read `HANDOVER.md` (top) +
the `[[project_stall_hunt]]` memory + the BACKLOG.md entry "RPi4 idle-teardown TLBI/DVM stall" FIRST --
they have the full narrowing; do NOT re-derive it.

## The goal

Eliminate the residual RPi4 idle-teardown TLBI/DVM stall: ~8% (2/24 on `netstall.py --idle 8`) of
idle-then-process-teardown sequences freeze the WHOLE system 33-66s (= N x 10.8s; 32.4s == 0x80000 ticks
@ ~16.2kHz = the BCM2711 UBUS-timeout class). IRQs are off in the kernel teardown unmap, so everything
stops. Pi currently runs **v0.4.261 build 2596** at 192.168.0.8 (the masked TLB shootdown 32dbc39 already
cut it 6/16 -> ~2/24; light/normal use is mostly fine).

## The KEY framing (Bryan, 2026-06-17): it is NOT a BCM2711 HW limitation

**Linux on the SAME Pi4 does TLBIs, core idle, and process teardown with ZERO 32.4s freezes.** So
AIOS/seL4 (or the armstub/firmware it relies on) is MISSING A72/cluster config that Linux does. The fix
is to find that gap by COMPARING AIOS-vs-Linux, then A/B it -- NOT to mine a timeout register blind or
accept it.

## The prime suspect (this session): A72 implementation-defined control registers AIOS never sets

**Grep-proven**: NOTHING in the AIOS boot path sets the A72 IMP-DEF control regs --
`grep -rn "CPUECTLR|L2CTLR|CPUACTLR|SMPEN" deps/kernel deps/seL4_tools/elfloader-tool hw/rpi4` = ZERO
hits. So AIOS relies ENTIRELY on the armstub for `CPUECTLR_EL1` / `L2CTLR_EL1` / `CPUACTLR_EL1`, and
leaves them at whatever the RPi armstub set (likely reset-ish). Linux explicitly configures these (A72
errata workarounds + prefetch + cluster/retention) in its arm64 cpu-setup + errata code.

**Important nuance -- SMPEN itself is probably ALREADY on**: the v0.4.261 SHM-ring cross-core test was
BYTE-EXACT (writer+reader on different A72 cores via coresched.1, cacheable-inner-shareable frame, sha256
== host). That REQUIRES cross-core cache coherency, which `CPUECTLR_EL1.SMPEN` (bit 6) enables. So
SMPEN-off is unlikely the cause. The suspect is the OTHER fields -- most likely a **cluster/L2 retention
or DVM/snoop config** left at reset that, even with the no-WFI idle, lets the fabric go "cold" so the
FIRST post-idle `tlbi vae1` DVM completion hangs to the UBUS timeout. ("teardown-after-idle stalls,
back-to-back clean" -> the first DVM txn warms a cold fabric.) This is the memory's original "proper fix"
direction (`[[project_stall_hunt]]`: "configure the A72 retention delays OFF at boot -- CPUECTLR/L2
retention fields").

## Plan

1. **PROBE the regs on AIOS.** Add a one-shot boot print (or `/proc/a72regs`) that reads `CPUECTLR_EL1`
   (`mrs x, S3_1_C15_C2_1`), `L2CTLR_EL1` (`S3_1_C11_C0_2`), `CPUACTLR_EL1` (`S3_1_C15_C2_0`) on EACH
   core (core 0 + a value pushed from cores 1-3 at their bringup). seL4 non-hyp runs at **EL1**; these are
   EL1 regs but the armstub (EL3) may have TRAPPED EL1 access (`ACTLR_EL3/EL2`) -> a read may UNDEF/abort.
   If it traps, the change must go at EL3 (armstub) / EL2 (elfloader) / `config.txt`, not the kernel.
   Easiest first read: from the kernel boot (core 0) print the three regs; wrap in a fault-survivable
   probe if unsure.

2. **COMPARE to Linux on the Pi4.** Boot Raspberry Pi OS (or any arm64 Linux) on a Pi4 and read what it
   programs: `CPUECTLR_EL1`/`L2CTLR_EL1` are not in sysfs, so either (a) read Linux's arm64
   `__cpu_setup`/errata (arch/arm64/mm/proc.S + arch/arm64/kernel/cpu_errata.c -- A72 errata 859971,
   {832075}, the SMPEN/prefetch setup) and the RPi armstub8.S source (raspberrypi/tools/armstubs), or
   (b) add a tiny Linux kernel module / `mrs` reader. Find the DELTA vs AIOS's values from step 1.

3. **A/B the delta.** Set the suspect field(s) to Linux's value (start with disabling cluster/L2
   retention / any DVM-affecting bit) at boot. WHERE depends on step 1's trap result:
   - EL1-writable -> in the kernel per-core init (there is NO existing CPUECTLR write; add one in the
     arm64 init path, guarded for the A72) + in the elfloader secondary `crt0.S` `secondary_startup` so
     cores 1-3 get it too.
   - EL1-trapped -> a custom armstub (config.txt `armstub=`; the RPi armstub8.S is small + buildable) or
     a `config.txt` knob, set on each core at EL3 before drop.
   Rebuild build-rpi4-netd -> `mkkernel8 --kernel build-rpi4-netd/images/aios_root-image-arm-bcm2711`
   -> `pi_flash.py --host 192.168.0.8`. Re-soak: `netstall.py --host 192.168.0.8 --trials 30 --idle 8`.

4. **Goal: 0/30 idle-teardown freezes.** If a CPUECTLR/cluster field cures it, that's the breakthrough
   (and it's the documented Linux gap). If not, the next backlog direction is the SCB/ARM-fabric UBUS
   timeout register (default 0x80000, NOT the PCIe RC's 0x40a8) -- mine Linux's brcmstb fabric setup.

## What's RULED OUT (do not re-investigate -- see [[project_stall_hunt]])

tlbi_probe keepalive (A/B redundant), idle-core DVM quiescence (corewarm A/B made it WORSE), the Stage-S
fastpath residency hook (code-proven inert under core-0 pinning), dsb-scope (`nsh` == `sy`), reducing the
teardown TLBI count (first-after-idle, not count-dependent). SMPEN-off (SHM-ring cross-core byte-exact).

## Tooling / discipline

`scripts/netstall.py --host 192.168.0.8 --trials 30 --idle 8` = the reconnect-robust idle-teardown probe
(N+0.3s clean, N+11..70s stalled). `/proc/corewarm.1` = the cores-1-3 busy A/B knob. netconsole wedges
under load -> drive 1-cmd-per-conn (`scripts/shmring_hw_recover.py` pattern), read /proc AFTER disarming.
Reboot = `reboot` over netconsole (BCM2711 watchdog, ~90s, .8/.250/.197, ARP `dc:a6:32:1c:2e:e1`). Full
pre-flash QEMU gate (netd 10/10, socket 8/8, smp 7/7, shmring 26/26) before any flash. Kernel changes go
in the sibling `~/Desktop/github_repos/seL4` working tree + capture into `deps/patches/seL4-kernel.patch`.
DON'T flash unless QEMU-gated; the stall is HW-only so a bad CPUECTLR write could brick -> keep the prior
good `disk/kernel8.img` + know the SD-reflash recovery (balenaEtcher `disk/sdcard-rpi4.img`).
