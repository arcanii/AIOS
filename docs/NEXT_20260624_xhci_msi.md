# NEXT: xHCI MSI (lead #3) -- interrupt-driven event consumption (VL805 keyboard stall)

The remaining VL805-stall hypothesis after **lead #1 (cacheable DMA) FAILED on HW** (commits
42641f0 + cf9e310, [[project_usb_kbd_dma_stall]]): the ~10.8s quanta come from the masked
POLLING cadence hitting the A72 `dsb;tlbi;dsb` window. Linux uses **MSI** (interrupt-driven),
and our boot log even says `xHCI IRQ: brcmstb MSI not yet wired -- polling`. Switching event
consumption from masked-poll to MSI changes WHEN/HOW the CPU touches the controller -- not the
(correct, deliberate) non-cacheable memory attribute.

## What is ALREADY built (consumption side -- ready)
- `xhci_setup_irq()` (src/usb/xhci.c): allocs a notification, `simple_get_IRQ_handler(&simple,
  irq, path)`, `seL4_IRQHandler_SetNotification`, `Ack`. Gated on `plat_pcie_xhci_irq() >= 0`.
- Driver thread (xhci.c ~1300): when `xhci_irq_mode` (set by `/proc/xhci.irq.1`) and the ntfn is
  bound, it `seL4_Wait`s on the IRQ + re-Acks instead of yield-spinning. `/proc/xhci.irq.0` reverts.
- `/proc/xhci` shows `irq: mode=N bound=N num=N count=N` -- `count` climbing = MSIs are arriving.
- So the ONLY missing piece is the IRQ SOURCE: `plat_pcie_xhci_irq()` returns -1, and the brcmstb
  RC MSI + the VL805 MSI capability are never programmed.

## The exact recipe (Linux raspberrypi/linux rpi-5.10.y drivers/pci/controller/pcie-brcmstb.c)
BCM2711 is a **legacy** chip (hw_rev < 0x0303): MSIs live in the CPU INTR2 register, bits [31:24].

RC MSI registers (offsets from the PCIe-CORE base `rd/wr` already use in pcie_brcmstb.c):
```
PCIE_MISC_MSI_BAR_CONFIG_LO  0x4044
PCIE_MISC_MSI_BAR_CONFIG_HI  0x4048
PCIE_MISC_MSI_DATA_CONFIG    0x404c
PCIE_INTR2_CPU_BASE          0x4300   ; legacy MSI INTR2 block (NOT the 0x4500 block AIOS masks today)
  MSI_INT_STATUS   = 0x4300 (+0x0)
  MSI_INT_CLR      = 0x4308 (+0x8)
  MSI_INT_MASK_SET = 0x4310 (+0x10)
  MSI_INT_MASK_CLR = 0x4314 (+0x14)
```
Values for BCM2711 (legacy, 8 MSIs):
```
target_addr = BRCM_MSI_TARGET_ADDR_LT_4GB = 0x0fffffffc   ; RC_BAR2 is the low window -> LT_4GB
DATA_CONFIG = PCIE_MISC_MSI_DATA_CONFIG_VAL_8 = 0xfff86540
legacy_shift = 24  -> the 8 MSI bits are [31:24]; vector v -> INTR2 bit (24+v)
```
brcm_msi_set_regs() sequence (RC side):
```
wr(0xff000000, MSI_INT_MASK_CLR=0x4314);   // GENMASK(31,24): unmask the 8 legacy MSI bits
wr(0xff000000, MSI_INT_CLR=0x4308);        // clear pending
wr((lower32(target)|1)=0xfffffffd, MSI_BAR_CONFIG_LO=0x4044);
wr(upper32(target)=0x0,             MSI_BAR_CONFIG_HI=0x4048);
wr(0xfff86540,                      MSI_DATA_CONFIG=0x404c);
```
ISR/ack:
```
status = rd(MSI_INT_STATUS=0x4300);        // bits [31:24] = pending MSI vectors
... handle ...
wr(1 << (24+vector), MSI_INT_CLR=0x4308);  // ack the serviced vector(s)
```
NOTE: pcie_bringup currently MASKS the 0x4500 block (MSI_INTR2_CLR=0x4508/MASK_SET=0x4510). That is
the NEWER-chip MSI block; harmless for BCM2711 but DO NOT rely on it. Use the 0x4300 CPU-INTR2 block.

## Program the VL805 MSI capability (config space -- AIOS already has ECAM access)
The device sends MSI as a memory write to `target_addr` with a data value the RC maps to an INTR2
bit. Program the VL805's standard MSI capability (PCI cap ID 0x05) via the ECAM
(hw_info.pcie_ecam_paddr; pcie_brcmstb.c already reads VL805 config for BAR sizing):
1. Walk the capability list (config 0x34 -> cap ptr; follow next-ptr) to find cap ID 0x05.
2. Message Address (cap+0x4) = lower32(target_addr) = 0xfffffffc; if 64-bit-capable (MC bit 7),
   Message Address Upper (cap+0x8) = 0, and Message Data is at cap+0xC (else cap+0x8).
3. Message Data = the MSI data for vector 0. For the brcm legacy controller the data low bits
   select the vector; with VAL_8 the controller expects data == (0x6540 & ...) | vector -- VERIFY
   on HW: start with the value Linux composes (msg.data for hwirq 0) and confirm INTR2 bit 24 fires.
4. Message Control (cap+0x2): set MSI Enable (bit 0); Multiple Message Enable = 0 (1 vector).
5. Make sure the controller's INTE/interrupter-0 IE is enabled when in IRQ mode (the driver already
   leaves INTE off until IRQ mode -- enable interrupter-0 IE + USBCMD.INTE on entering IRQ mode).

## GIC SPI -> seL4 IRQ
BCM2711 DTB: GIC_SPI 147 = PCIe controller, **GIC_SPI 148 = the builtin MSI controller**. AIOS maps
SPI N -> seL4 IRQ **N+32** (boot_dtb.c:212). So `plat_pcie_xhci_irq()` should return **180** (148+32).
Add `hw_info.pcie_msi_irq` (parse the pcie node's msi interrupt from the DTB, like genet_irq) or
hardcode 180 for RPi4. Confirm seL4 will hand out IRQ 180 (`simple_get_IRQ_handler`) -- it must be in
the kernel's IRQ-control range (it is for SPIs; GENET=189-ish works the same way).

## Implementation steps (ordered, each HW-checkable via /proc/xhci)
1. pcie_brcmstb.c: add the MSI reg defines + `brcm_msi_setup()` (the RC sequence above). Add
   `plat_pcie_xhci_irq()` -> 180. Add the VL805 MSI-cap programming (config-space walk + writes).
2. xhci.c: when entering IRQ mode (`/proc/xhci.irq.1`), enable interrupter-0 IE + USBCMD.INTE (the
   controller only raises MSI with INTE on). On `.irq.0`, mask + revert to poll.
3. KEEP DEFAULT = POLL. Only `/proc/xhci.irq.1` arms MSI + blocks on the IRQ -> safe, reversible,
   A/B-able without reflashing for the toggle (one kernel, runtime switch).
4. Deploy kernel-only (pi_flash, flash-free, reversible). HW test:
   - `cat /proc/xhci.irq.1`, type on the keyboard -> `/proc/xhci` `irq: count` climbs (MSIs arrive),
     `key_events` climbs, `int_errs=0` (interrupt-driven keyboard works).
   - Run the TLBI load test WITH the keyboard attached -> do the ~10.8s quanta STOP vs poll mode?
     That is the lead-#3 verdict (`/proc/laststall total`).
   - If `count` stays 0: MSI not firing -> recheck the VL805 Message Data / target / INTR2 bit /
     the GIC SPI number (serial CONFIG_PRINTING helps here).

## FIRST HW TEST 2026-06-24 (build 2953) -- IMPLEMENTED + BINDS, but MSI does NOT fire yet
Implemented (commit e548719) + flashed build-rpi4. `/proc/xhci`: keyboard enumerates in poll mode
(kbd_ok=1, evt_deq=42), and the seL4 IRQ binds: `irq: bound=1 num=180`. Armed `/proc/xhci.irq.1`
(mode=1). On keypress: **count=0, key_events=0, evt_deq stuck at 42** -> the VL805's transfer events
hit the ring but NO MSI reached the driver, so it stayed blocked in seL4_Wait (keyboard unresponsive
until reboot; reboot -> poll -> kbd_ok=1, recovered). So: the seL4 side is fine; the MSI SOURCE is not
delivering. Localize NEXT (over netconsole -- no serial needed):
1. ADD OBSERVABILITY to /proc/xhci (read over netconsole): the brcmstb RC INTR2 STATUS (rd(0x4300)),
   the MSI cap offset + is64 + the read-back Message Control/Address/Data, and a count of times INTR2
   bit24 was seen set. This splits the failure cleanly:
   - INTR2 status bit24 SETS on keypress but count stays 0 -> the RC got the MSI; the GIC SPI / seL4
     IRQ-180 mapping is wrong (try the other SPI, or confirm 148 is the MSI SPI not 147).
   - INTR2 status bit24 NEVER sets -> the VL805 is not sending the MSI the RC recognizes -> the
     Message Data is wrong (my BRCM_MSI_DATA_MATCH=0x6540 guess) or the cap wasn't enabled / target
     addr wrong. Recheck Linux brcm_msi_compose_msg for the EXACT msg.data, and confirm the cap walk
     found + enabled MSI (Message Control bit0).
2. Sanity: is the xHCI interrupter actually asserting MSI? (INTE set by xhci_irq_enable; ERDP EHB
   cleared each event.) A controller-side miss would also give count=0 -- the INTR2-status probe
   distinguishes it (no INTR2 = nothing left the controller/device).
3. Serial (CONFIG_PRINTING) shows the "[pcie] xHCI MSI armed: cap@.. target=.. data=.." line -- use
   it if the /proc probe is inconclusive.

## Risks / notes
- HW-ONLY validatable (QEMU cannot model the brcmstb MSI controller). Iterate on the board (Bryan
  near it; a wrong setup just leaves count=0 / stays polling -- low wedge risk if default=poll).
- Spurious-MSI / shared-vector: the VL805 is one function, one vector -> simple.
- This does NOT touch the (correct) non-cacheable DMA mapping. Orthogonal to lead #1.
- Keep the 32ms poll clamp (v0.4.221) as the fallback if MSI also does not stop the quanta.
- The keyboard-LESS stall source is SEPARATE (prewarm-mitigated) -- MSI targets the keyboard-
  amplified Source B only. See [[feedback_stall_open_concern]] (never call the stall solved).
