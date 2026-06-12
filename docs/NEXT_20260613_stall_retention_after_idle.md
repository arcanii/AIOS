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

## NEXT (the cure)

1. **Confirm the fabric/retention mechanism + find the knob.** A72 CPUECTLR_EL1
   / L2CTLR_EL1 are EL1 -- the EL0 root task cannot read them, so this needs a
   KERNEL probe (MRS at boot/EL2, print, try clearing any retention/clock-gate
   field; CPUECTLR may be EL3-locked on Pi4 -> may need armstub/config.txt).
2. **Bound the non-PCIe UBUS/fabric timeout** like v0.4.213 did for the PCIe RC
   (0x40a8 -> 0x1000). With PCIe AND GENET ruled out, the wedging master is the
   mini-UART (AUX) or the main-peripheral/VPU bus -- find that bus's timeout
   register (mine Linux/U-Boot bcm2711). Bounding it turns the 32s freeze into
   a sub-ms blip regardless of load -- the most promising CURE.
3. Optional: a console-suppressed variant to settle the mini-UART question,
   but it needs a non-UART detector (hard on a GENET-off board).

## HARDWARE STATE / RECOVERY (important)

The Pi is on **V4-genetoff (build 2115): GENET OFF, serial-only, NO network.**
It CANNOT be flashed over the network (fatswap needs a netconsole push). To
restore a networked kernel you must **physically reflash the card** (balenaEtcher
disk/sdcard-rpi4.img, or swap kernel8.img) OR re-flash to a GENET-on variant
once the card is reachable again. Keep KernelMaxNumNodes=1 (Bryan's call:
SMP=4 only after a real cure). Serial: /dev/cu.usbserial-0001 @115200, one
reader (stop sercap before driving load).
