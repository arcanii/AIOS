# NEXT: VL805 inbound-DMA vs TLBI/DSB collision (Source B) -- 2026-06-12

The remaining half of the 32.4s freeze hunt (see CHANGELOG v0.4.221 and the
project_stall_hunt memory). Source A (WFI retention) is FIXED (kernel idle.S
yield). Source B remains: with PCIe up and a USB keyboard attached, the
VL805's periodic LS-keyboard split-transaction DMA collides with the A72's
`dsb; tlbi vae1; dsb` so that the kernel stalls in exact ~10.8s quanta
(typically 32.4s = 3x), IRQs off, freezing the whole system. A quantum also
kills the keyboard (32s unpolled = the LS-behind-TT wedge).

## Established facts (all HW-verified, 2026-06-12)

- Stalls bracket EXACTLY to the TLBI line inside kernel unmapPage (KUPHASE
  prints; lookup/PTE/cache phases always 0ms). Userspace and kernel brackets
  agree to the ms.
- Triggers ONLY with PCIe up AND the keyboard attached. Stalls stop the
  instant the keyboard unplugs (hammer thread runs clean indefinitely after
  the cc=36 unplug event). PCIe-off kernels are 100% clean with no-WFI idle.
- Immune to: KernelMaxNumNodes 1 vs 4, continuous TLBI traffic (the
  tlbi_probe hammer), UBUS_TIMEOUT (0x40a8) and RC_CONFIG_RETRY_TIMEOUT
  (0x405c) -- both rewritten to small values, no change. ubus default 0x80000.
- Frequency scales with the keyboard poll rate: 8ms polling = constant
  freezes; v0.4.221 clamps LS/FS to 32ms (1/4 the windows). The dial is in
  src/usb/xhci.c (search "v0.4.221 BCM2711 mitigation").
- 10.8s base quantum; stalls come in 1x/2x/3x/4x multiples. The same quanta
  for Source A and B suggests one shared interconnect-level bound.

## Why Linux does not hit this (the leads, in order)

1. **DMA target memory attributes.** Our xHCI DMA pool (event ring, transfer
   rings, report buffers -- src/usb/xhci.c xhci_dma_reserve) is mapped
   NON-CACHEABLE in the root task. The VL805 writes event TRBs into it every
   poll. Check what inbound attribute path those writes take (SCB) and what
   Linux uses (streaming DMA API + cache maintenance vs coherent pool
   placement). Experiment: move the pool to cacheable + explicit clean/inval
   (the GENET/HDMI lesson says SAME-attribute mappings matter) and see if the
   quanta stop.
2. **brcmstb inbound window (RC_BAR2) config.** Compare our pcie_brcmstb.c
   RC_BAR2/SCB0 sizing against Linux pcie-brcmstb.c for 2711 (burst size,
   SCB_ACCESS_EN, MAX_BURST -- we set 128B per U-Boot; Linux uses 128 for
   2711 too but check SCB0_SIZE encoding).
3. **MSI vs polling.** Linux uses MSI; we poll with the IRQ masked. The
   controller's interrupter behavior differs (ERDP write cadence, event ring
   full handling). Cheap test: consume events less often / batch ERDP writes.
4. **Forum/errata sweep**: "BCM2711 TLBI DVM hang", "A72 DVM PCIe deadlock",
   VL805 firmware versions (vl805 fw is loaded by the VC firmware; version
   differs by EEPROM/firmware release -- `vcgencmd otp_dump`-equivalent).

## Tools in the tree

- tlbi_probe.c: keepalive + the idle-system measurement probe (started from
  boot_services; prints [tlbi] SLOW / alive).
- [pipe]/[reap] >250ms probes (permanent). For deeper brackets, the v0.4.206-
  217 diag patches are in git history (KUPHASE/KUNMAP/sct/upg/vfs2).
- scripts: netconsole load test = 10-12x `echo` round-trips (32s+ = quantum);
  serial capture /tmp/sercap_run.py pattern (single reader, restart after
  every Pi power cycle -- the adapter drops).
- Deploy: scripts/mkkernel8.py + card swap; ALWAYS verify sha + banner
  (feedback_kernel8_deploy_verify; banner build number = ninja's N-1).

## Interim state shipped (v0.4.221)

no-WFI idle + single-core + 32ms keyboard polling + PCIe timeouts bounded.
Usable but a keyboard-attached quantum still occurs occasionally and wedges
the keyboard until reboot. For long keyboard-less netconsole/SSH sessions,
a PCIe-off build is completely stall-free.

## NEW DATA POINT (2026-06-12 evening, fatswap session, v0.4.222/223)

Quanta fire WITH NO KEYBOARD ATTACHED: a keyboard-unplugged boot logged
`[pipe] SLOW ... 32745ms / 97503ms / 98181ms` and
`[reap] SLOW ... destroy=129603ms / [pipe] SLOW label=5 129614ms` (1x/3x/4x
the 32.4s quantum). The "stalls stop the instant the keyboard unplugs" live
evidence from the v0.4.221 hunt does NOT generalize: either another USB/VL805
activity source (mouse? hub enumeration retries? xHCI periodic schedule with
no device?) or a non-USB trigger also collides with TLBI/DSB. Re-baseline the
A/B harness with ZERO USB devices and with PCIe fully off before trusting any
device-specific theory. Collateral (fixed v0.4.224): the quanta exposed eMMC
completion-timeout fall-throughs that silently corrupted/lost block I/O --
see CHANGELOG v0.4.224; /proc/cachestats emmc_timeout_* counters now measure
how often the data phase actually times out, which doubles as a cheap quantum
detector for this hunt.

## Also open (separate)

- fork=261ms/exec=314-970ms spawn cost (pipe-server occupancy).
- ~~FAT32 write driver for flash-over-network kernel swaps~~ SHIPPED
  v0.4.222-224 (fatswap; scripts/pi_flash.py; HW-verified incl cold boot).
  Kernel iterations for THIS hunt no longer need card pulls.
- Restore KernelMaxNumNodes=4 once Source B is fixed.
