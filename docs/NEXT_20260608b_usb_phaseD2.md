# NEXT 2026-06-08b -- USB HID Phase D.2: map the VL805 xHCI BAR -> a key types on the Pi

Seed for a fresh session. Read with `HANDOVER.md`, the `project_usb_hid` memory, and
`docs/NEXT_20260608_usb_phase_d.md` ("FINAL STATUS" -- the D.1 saga + all 7 fixes).

## STATUS 2026-06-08 -- D.2 IMPLEMENTED + build-clean (HW test PENDING, UNCOMMITTED)

Code done; needs a Pi + serial. THREE files, all additive + crash-safe (version
stays 0.4.183 -- bump at the first keypress):
- **D.2a** `src/plat/rpi4/pcie_brcmstb.c`: `cfg_wr` + `program_xhci_bar()` -- sizes
  the VL805 BAR0 (64-bit aware), places it at PCI 0xC0000000 / CPU 0x6_00000000
  (the outbound window), enables EP mem+busmaster, sets `pcie_xhci_*`. Safe config ops.
- **D.2b is very likely NOT NEEDED (the big finding).** The "BAR is above the 4GB
  device-untyped top" was inferred from devices_gen.h, NOT the real ceiling.
  `CONFIG_PADDR_USER_DEVICE_TOP` = **2^44** (hyp OFF), and `create_untypeds`
  (deps/kernel boot.c:811) makes DEVICE untypeds up to 2^44 low->high before the 230
  cap -- 0x6_00000000 lands ~5 aligned splits past end-of-RAM (0xfc000000), well
  inside the cap. So `sel4platsupport_alloc_frame_at(0x600000000)` should already work
  with NO deps/kernel edit. A SAFE always-on diagnostic in `src/aios_root.c`
  (PLAT_RPI4, bootinfo read, no MMIO) prints whether the window IS a device untyped --
  it answers this from one serial boot. Only do the kernel edit if it says NOT COVERED.
- **Staged `PCIE_PROBE_LEVEL`** (pcie_brcmstb.c): 0 safe default; **1** = D.1 + D.2a
  BAR program + diagnostic (NO BAR read -> crash-safe; the working-tree default + the
  built `disk/kernel8.img`); **2** = also set `pcie_xhci_present` -> `xhci_init()` maps
  + drives the BAR (the keyboard; the BAR read is the one SError risk). `src/usb/xhci.c`
  map_bar cap 8->64 pages (VL805 BAR up to 64KB). Reset the #define to 0 before commit.
- **Verified:** build-rpi4 + build-04 clean; `xhci_key_qemu_test` PASS; `smp_qemu_test`
  7/7. `disk/kernel8.img` = level-1 confirm build (flash-free).
- **HW test (Bryan):** flash `disk/kernel8.img` (lvl 1, crash-safe), serial-capture.
  Expect `VL805 xHCI DETECTED` + `VL805 BAR0 -> PCI 0xc0000000 / CPU 0x600000000` +
  `[pcie] D.2 window 0x600000000 IS a device untyped ... NO kernel change needed`. If
  so -> set `PCIE_PROBE_LEVEL` 1->2, `ninja -C build-rpi4 && python3
  scripts/mkkernel8.py`, FAT-copy, boot -> a USB keyboard types -> bump 0.4.184 +
  commit. If "NOT COVERED" -> deps/kernel bcm2711 device-region edit first, then lvl 2.

---


## Where things are (D.1 DONE on real HW)
Phase D.1 is COMPLETE on the real RPi4: the brcmstb PCIe link trains AND the VL805
xHCI enumerates. Serial (PROBE_LEVEL=1, `src/plat/rpi4/pcie_brcmstb.c`):
```
[pcie] brcmstb rev=0x0304 PCIE_STATUS=0xb0 link=UP mode=RC
[pcie] SSC: ssc=1 pll_lock=1
[pcie] link: 5.0 Gbps x1 (lnksta=0x9012)
[pcie] bus1 dev0: VID=1106 PID=3483 class=0c0330
[pcie] VL805 xHCI DETECTED -- Phase D.1 link OK.
```
The shared xHCI driver (`src/usb/xhci.c`, Layers 2-5: controller + enumeration + HID
+ keymap + polling driver) is already written + QEMU-verified (a USB keyboard types
into the AIOS shell on QEMU; `python3 scripts/xhci_key_qemu_test.py`). The ONLY thing
left for a key to type on the Pi is making the VL805 xHCI BAR reachable, then running
`xhci_init()`. That is D.2.

## The one thing to build (D.2)
Map the VL805 xHCI BAR0 so `xhci_init()` runs on the Pi. Two parts:

### D.2a -- program the VL805 BAR (in pcie_brcmstb.c, after the VL805 is detected)
The VL805 is at bus 1 dev 0 fn 0 (config access works now via EXT_CFG -- cfg_rd, and
add a cfg_wr the same way: bus0 = direct wr(off); bus>=1 = wr(EXT_CFG_INDEX, idx);
*(EXT_CFG_DATA+off) = val). Standard BAR sizing on the VL805's BAR0 (config offset
0x10): write 0xffffffff, read back, ~mask -> size; place it at `hw_info.pcie_mmio_pci`
(0xC0000000) in the outbound window (we already program the window in set_outbound_win
-> CPU 0x6_00000000 -> PCI 0xC0000000); write the PCI addr into BAR0; enable
mem-space + bus-master in the VL805's command reg (config 0x04). Then set the globals
`src/usb/xhci.c`/`include/aios/pcie.h` expect:
  pcie_xhci_bar      = 0x600000000ULL + (bar_pci - 0xC0000000ULL);  /* CPU side */
  pcie_xhci_bar_size = <sized>;
  pcie_xhci_bus/dev/fn = 1/0/0;
  pcie_xhci_present  = 1;
`aios_root.c` already calls `xhci_init()` when `pcie_xhci_present`.

### D.2b -- seL4 KERNEL CHANGE: expose the PCIe window >4GB (the real blocker)
The BAR lives at CPU 0x6_00000000, ABOVE seL4 bcm2711's 4GB device-untyped top
(`build-rpi4/kernel/gen_headers/plat/machine/devices_gen.h` only has RAM <4GB + a few
device frames). So `sel4platsupport_alloc_frame_at(0x6_00000000+...)` CANNOT map the
BAR today. Extend the seL4 bcm2711 kernel device regions to cover the PCIe outbound
window above 4GB:
- Edit the kernel hardware spec / `deps/kernel/src/plat/bcm2711/overlay-rpi4.dts` (or
  the devices list that generates devices_gen.h) to add the window
  [0x600000000, 0x640000000) as user-available device memory, and ensure the kernel's
  max paddr / CONFIG_PADDR_USER_DEVICE_TOP covers it.
- Kernel rebuild (BOTH build-rpi4; deps/ is GITIGNORED -- re-apply on a deps reset;
  record the patch in the memory).
- Add the window to `prealloc_rpi4_devices` (`src/boot/boot_device_map.c`) in ascending
  paddr order (it is the highest, so last), stash a `dev_pcie_win_vaddr`, OR let
  xhci.c map it via sel4platsupport_alloc_frame_at now that the region exists.
- xhci.c maps `pcie_xhci_bar` (CPU 0x6_...) NON-CACHEABLE and drives the controller.

### D.2c -- run it
With the BAR mapped, `xhci_init()` resets the controller, sets up DCBAA/command/event
rings (single non-cacheable DMA pages, paddr via seL4_ARM_Page_GetAddress -- inbound
DMA is identity via RC_BAR2, so paddrs work as the controller's DMA addresses),
enumerates the keyboard, and the polling driver thread feeds keys via
`seL4_Call(serial_ep, SER_KEY_PUSH)`. A72 DMA-ring coherency is QEMU-INVISIBLE --
verify on HW (rings are non-cacheable; no code-via-data here so no Unify_Instruction
needed, unlike demand-text).

## Workflow (this is the fast loop now)
- D.2a is a pcie_brcmstb.c change (root task) -> flash-free: `ninja -C build-rpi4`,
  `python3 scripts/mkkernel8.py`, `cp disk/kernel8.img /Volumes/AIOSBOOT/kernel8.img`,
  eject, boot ([[feedback_flashfree_kernel]]). Source default PROBE_LEVEL=0 (safe);
  build with -DPCIE_PROBE_LEVEL=1 (or edit the #define) to probe -- disk/kernel8.img
  is already the PROBE_LEVEL=1 build.
- D.2b is a KERNEL change -> needs a full image (`ninja -C build-rpi4`, then mksdcard
  OR the kernel8.img is still just the FAT file -- a kernel change DOES land in
  kernel8.img, so FAT-copy still works; the ext2 partition is unchanged). Keep
  `disk/sdcard-rpi4.img` (and `disk/kernel8-recovery.img`, the PROBE_LEVEL=0 build) as
  recovery.
- SERIAL is essential: `python3 scripts/aios_console.py monitor /dev/cu.usbserial-0001
  --baud 115200`. QEMU cannot model brcmstb -- the PCIe/BAR path is HW-only; keep the
  shared xhci.c QEMU test (`xhci_key_qemu_test.py`) green.
- The local `../u-boot` tree enumerates the VL805 (rpi_4_defconfig, CONFIG_PCI_BRCMSTB);
  `disk/u-boot-rpi4.bin` boots it (copy as kernel8.img) -- a live reference for the
  BAR/xHCI state (`md`/`pci`/`usb` at its prompt). Restore AIOS: copy kernel8.img back.

## Gotchas / lessons from D.1 (do NOT re-chase)
- The D.1 wall was NEVER a power-gate / EEPROM / firmware issue. It was, in order:
  reset-ordering (read core regs before the RGR1 reset), inverted PERST polarity, CRS,
  the NOTIFY-is-a-reset-not-a-loader misread, the missing outbound window, missing SSC,
  and finally the **RC bridge bus-number forwarding** (sec=1/sub=1 at config 0x18 so
  the RC routes config to bus 1 -- a GENERIC-PCI-CORE step, not in the controller
  driver). Lesson: when porting a Linux/U-Boot controller driver, port the PCI-core
  bridge setup (bus numbers + command reg) too.
- NEVER chase: VC-mailbox power-on (GET_DOMAIN_STATE returns garbage), EEPROM VL805=1
  (Compute Module 4 ONLY), newer firmware (no change). The NOTIFY_XHCI_RESET call in
  pcie_brcmstb.c is a harmless no-op (the VL805 self-loads from its dedicated EEPROM on
  PERST) -- you may delete it + the VC-mailbox helper on cleanup.
- D.1 is detection only -- no input device yet. version.h bumps at the first keypress.

## Conventions
- No apostrophes in C comments. Commit only when asked. Commit msgs end with:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Build BOTH build-04 (QEMU smoke) + build-rpi4 after shared-code changes.

## Fallback
If D.2b (the seL4 kernel device-window change) proves hard, D.1 stands as a real,
HW-verified milestone (PCIe + VL805 detection). The keyboard is purely additive on
top of it. No pressure to land D.2 in one session.
