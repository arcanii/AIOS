# NEXT: Source-B stall is teardown-after-idle (retention/fabric), NOT PCIe/GENET -- 2026-06-13

Supersedes NEXT_20260612_vl805_dma_stall.md. That doc's VL805/keyboard-DMA
theory is DEAD. This session re-baselined the 32.4s whole-system freeze hunt
from scratch with a compile-time bisect gate + a standardized soak harness,
and the picture changed completely.

## What the freeze IS (re-confirmed)

A process-TEARDOWN unmap stalls the whole system: the kernel `unmapPage`
`dsb; tlbi vae1; dsb` (IRQs off) blocks for an exact multiple of ~10.8s
(32.4 = 3x, 43.2 = 4x, 64.8 = 6x, 76 = 7x observed), freezing every thread
incl. the timer tick and the [tlbi] hammer. 10.8s x N == the BCM2711 UBUS
timeout territory (v0.4.213 measured the PCIe RC's UBUS_TIMEOUT @0x40a8
default 0x80000 ticks = 32.4s @ ~16.2kHz). Leading model: the leading `dsb`
waits on an outstanding Device-memory write that has wedged on the SoC
interconnect, and only "completes" when the fabric times it out at 10.8s x N.

## What this session RULED OUT (all HW A/B, zero USB devices)

- **PCIe / VL805 / USB: INNOCENT.** V1 = PCIE_PROBE_LEVEL=0 (PCIe core never
  touched). STILL DIRTY under load. Kills the entire v0.4.221 theory. The
  keyboard was at most a frequency amplifier. (V2a/b/c link/fw/bus-master
  bisect variants built but unnecessary.)
- **GENET / network DMA: INNOCENT.** V4 = boot_net_init disabled (GENET driver
  never inits; board serial-only). STILL DIRTY (76s stall under serial spawn
  storm). Confirmed GENET off: .250 silent, no :2323 anywhere.
- **eMMC completion path: not involved.** /proc/cachestats emmc_timeout_retries
  + _fails stay 0 across every dirty soak (V0, V4).
- **Mini-UART print VOLUME: probably not it (inconclusive).** V3-quiet
  (pipe/cow LOG_INFO->WARN, ~4 fewer UART lines/spawn) still stalled (in `pre`);
  L1 was clean but the run was stopped early. Not a clean refute; revisit if
  needed for a full L3 count.
- **Core count: MITIGATES, does not cure.** V5 = KernelMaxNumNodes=4 is clean
  at idle/boot/light-load but DIRTY under a heavy spawn-storm (L3: 11 quanta).
  4-core raises the threshold; the freeze still lurks. (NOTE A72 has no
  A53-style CPU/L2 dynamic-retention fields -- the old "WFI retention" framing
  is mis-attributed; the no-WFI idle on core 0 does NOT prevent this.)

## The behavioral signature (the new central clue)

**Teardown-AFTER-IDLE stalls; back-to-back teardowns are clean.** V4 serial
soak: chunk 1 (first burst after the board sat idle at the login prompt) =
76s stall; chunks 2-10 (1.5s gaps, no real idle) = 0.0s each. Same shape
everywhere: idle a while, then the first teardown burst eats a 10.8s x N
quantum; sustained back-to-back teardown does not. This is the ORIGINAL
Source-A mechanism -- so A and B are ONE bug: a quiescence/retention state
that engages while the core/cluster/fabric is idle and stalls the first
unmap `dsb` on wake-up. Load only modulates how often "idle then teardown"
occurs.

Evidence (V4 serial_soak, GENET off, 1-core): chunk 1 after the board sat
idle at the login prompt = 76.1s (7x quantum, 2 SLOW probe lines
32414/32407ms); chunks 2-10 run back-to-back (1.5s gaps) = 0.0s each, clean.
idle_dep_test.py corroborated qualitatively (the first burst after the login
idle stalled past its 240s cap) but the graduated-idle sweep (2/5/10/20/40s)
was abandoned -- 240s/burst is too slow and lossy mini-UART can drop the
"D20" sentinel and read as a false timeout. For a clean graduated sweep,
detect via a counter rather than a serial sentinel, or shorten the cap.

## Tools shipped this session (committed)

- src/plat/rpi4/pcie_brcmstb.c: PCIE_AB_STOP {1,2,3} compile gate (link /
  +fw / +bus-master), default 0 = no-op. PCIE_PROBE_LEVEL=0 = full PCIe off.
- scripts/sercap.py: timestamped read-only serial capture (the detector;
  [tlbi] alive 30s beat + [pipe]/[reap]/[tlbi] SLOW lines, which are printf so
  they survive LOG_LEVEL changes).
- scripts/sourceb_soak.py: standardized netconsole load (echo RTs / 1.5MB
  pushes / 20-spawn chunks / idle) + capture-based CLEAN/DIRTY verdict.
- scripts/serial_soak.py: serial-only load+detect (for GENET-off variants).
- scripts/idle_dep_test.py: graduated-idle -> timed teardown burst (proves
  idle-dependence directly).
- scripts/pi_flash.py: now scan-based Pi discovery (:2323), robust to the
  DHCP churn (.8/.127/.197/.250) and ICMP-only decoy hosts.
- scripts/netrx_b3_repro.py + pcap_tcp_dump.py: the B3 packet-capture work.
- Variant kernels on disk: disk/kernel8-V{0,1,2a,2b,2c,3,4,5}-*.img (builds
  2107-2116, all v0.4.227).

## MECHANISM PINNED + cure attempt 1 refuted (2026-06-13, builds 2120/2122)

- **D1 (build 2120) split-DSB diag:** instrumented invalidateLocalTLB_VAASID
  (`dsb; tlbi vae1; dsb; isb`) to time each half. Result, 3x consistent:
  **`lead_dsb=0ms  tlbi+trail_dsb=32399ms`.** The leading drain is INSTANT --
  there is NO wedged Device-memory write -- and the entire 32.4s is the TLBI
  COMPLETING. (Bounding a device/bus WRITE timeout would have been wrong.)
- **D2 (build 2122) dsb-scope cure REFUTED:** changed the trailing `dsb sy` ->
  `dsb nsh` (architecturally sufficient for a non-shareable `tlbi vae1`).
  It hangs the SAME 32399ms. So the completion-barrier scope is irrelevant;
  the `tlbi vae1` itself generates a fabric/DVM transaction that hangs when the
  interconnect has quiesced (idle), and ANY dsb after it waits. Reverted.
- The fixed 32399ms == 0x80000 ticks == a UBUS-class timeout (same default
  v0.4.213 measured at the PCIe RC's UBUS_TIMEOUT 0xFD500000+0x40a8). But that
  register is the PCIe RC's port; the TLBI/DVM hits a DIFFERENT fabric port's
  timeout (ARM/coherency side), whose register AIOS does not map and the
  BCM2711 TRM does not publicly document.

## NEXT (the cure) -- three candidate paths, by risk

1. **Keep the cluster/SCU/fabric awake (lowest risk, partly proven).** nodes=4
   (V5) was CLEAN at idle/boot/light-load because 4 yield-looping cores keep the
   cluster from quiescing -> the TLBI DVM completes fast. On nodes=1, cores 1-3
   sit in the armstub WFE spin-table and quiesce the cluster. Options: (a) ship
   nodes=4 as the practical fix and separately chase its heavy-load residual
   (which may be the SMP IPI/remote-TLBI path, a different sub-bug); (b) on
   nodes=1, release cores 1-3 into a spin (not WFE) -- needs elfloader/armstub
   work. Re-run D1's split-DSB diag under nodes=4 to confirm trail~0 at idle.
2. **Find + bound the ARM-fabric UBUS/DVM timeout register (medium risk).** Not
   the PCIe 0x40a8. Candidates: a SCB/GISB-arbiter timeout, or an ARM-control
   (0xFD000000 region) fabric register. Needs the bcm2711 TRM / Linux
   drivers/bus/brcmstb_gisb.c + empirical MMIO probing. Risk: a timeout that
   ERRORS the transaction instead of completing it could turn the hang into an
   SError. EL0 MMIO (no kernel brick), recoverable by reflash.
3. **L2/SCU clock-gate / retention disable via L2CTLR_EL1 / L2ECTLR_EL1 /
   CPUECTLR_EL1 (highest risk).** Needs a KERNEL-EL1 probe (EL0 root task can't
   MRS these); a faulting/EL3-trapped access at boot bricks until reflash.
   Read FIRST (print), never blind-write.

Diag kernels: disk/kernel8-D1-tlbisplit.img (2120, split-DSB), D2-dsbnsh-cure
(2122, refuted). The split-DSB instrumentation ([KUTLBI] print) lives in
deps/kernel machine.h + vspace.c -- UNTRACKED (capture into deps/patches or it
is lost on a deps reset); remove before shipping a release kernel.

## HARDWARE STATE / RECOVERY (important)

The Pi is on **V4-genetoff (build 2115): GENET OFF, serial-only, NO network.**
It CANNOT be flashed over the network (fatswap needs a netconsole push). To
restore a networked kernel you must **physically reflash the card** (balenaEtcher
disk/sdcard-rpi4.img, or swap kernel8.img) OR re-flash to a GENET-on variant
once the card is reachable again. Keep KernelMaxNumNodes=1 (Bryan's call:
SMP=4 only after a real cure). Serial: /dev/cu.usbserial-0001 @115200, one
reader (stop sercap before driving load).
