# NEXT 2026-06-08 -- USB HID Phase D: bring up RPi4 PCIe -> VL805 xHCI on real HW

Seed for a fresh session. Read with `HANDOVER.md`, `docs/DESIGN_USB_HID.md` (the
full design + the "Phase D findings" / "HW RESULT" sections), and the
`project_usb_hid` memory. The USB HID stack is QEMU-COMPLETE (a USB keyboard types
into the AIOS shell on QEMU); this is the last layer -- the RPi4 hardware bring-up.

## TL;DR -- the one thing to build
Get the RPi4 PCIe controller + VL805 xHCI alive so the existing shared xHCI driver
(`src/usb/xhci.c`) runs on real hardware -> a USB keyboard types into the AIOS
shell on the Pi. Two gates, in order: (1) POWER ON the controller (the firmware
power-gates it at OS handoff) via a VC-mailbox request, then (2) expose the PCIe
MMIO window to seL4 (it is above the 4GB device-untyped top) so the xHCI BAR maps.

## Where things are (committed, v0.4.183, branch main)
- **Phase 2 shared .text: HW-VERIFIED.** Done.
- **USB HID A/B/C: QEMU-complete + committed.** PCIe ECAM
  (`src/plat/qemu-virt/pcie_ecam.c`), xHCI init + enumeration + HID + keymap +
  polling driver thread (`src/usb/xhci.c`), fed to tty via SER_KEY_PUSH. Test:
  `python3 scripts/xhci_key_qemu_test.py` (PASS). Layers 2-5 are
  platform-independent; only Layer 1 (PCIe) differs by platform.
- **Phase D (RPi4): BLOCKED, code present but safe.** `src/plat/rpi4/pcie_brcmstb.c`
  has the brcmstb bring-up + VL805 config-detect (D.1). `boot_dtb.c parse_pcie`
  currently leaves `has_pcie=false` on RPi4 (the fixed-address fallback is
  reverted) so `plat_pcie_init` is SKIPPED and the boot is safe. `dev_pcie_vaddr`
  (controller regs, 10 pages @0xFD500000) is wired into `prealloc_rpi4_devices`
  (only claimed when has_pcie). The xHCI driver only runs when `pcie_xhci_present`.

## The two gates (confirmed by HW testing -- see DESIGN_USB_HID.md HW RESULT)
**Gate 1 -- the firmware power-gates the PCIe controller.** Reading 0xFD500000
from AIOS SError'd -> kernel halt (the controller is powered down at handoff; the
pcie node is "present" in the DTB but its ranges does not parse). vl805.bin is NOT
needed (it is an EEPROM-update blob; the VL805 fw is in the bootloader EEPROM).
The VL805/PCIe is "loaded on request from the kernel" -- so AIOS must REQUEST it.
Unblock with ONE of:
  (a) **Quickest test, no AIOS code:** set `VL805=1` in the bootloader EEPROM
      config (`rpi-eeprom-config` from Raspberry Pi OS), so the firmware brings
      PCIe up at boot; AIOS then sees the controller already powered.
  (b) **Self-contained:** issue `RPI_FIRMWARE_NOTIFY_XHCI_RESET` (mailbox tag
      0x00030058 -- the call Linux makes on RPi4 to load the VL805 fw + bring up
      PCIe), maybe + `SET_DOMAIN_STATE(RPI_POWER_DOMAIN_USB=6, on)` (tag 0x38030),
      via the existing `mbox_call` in `src/plat/rpi4/display_vc.c` (channel 8,
      buffer @0x3A000000), BEFORE `plat_pcie_init`.
**Gate 2 -- the PCIe MMIO window is outside seL4's address space.** The window is
CPU 0x6_00000000 (DTB ranges), but seL4 bcm2711
(`build-rpi4/kernel/gen_headers/plat/machine/devices_gen.h`) exposes only RAM <4GB
+ a few device frames. So `sel4platsupport_alloc_frame_at` cannot map the xHCI BAR.
Fix: extend the seL4 bcm2711 device regions to include the PCIe window above 4GB
(`deps/kernel/src/plat/bcm2711/overlay-rpi4.dts` / the kernel hardware spec ->
devices_gen.h; ensure max paddr covers it), kernel rebuild. (Gitignored deps/ --
re-apply on a deps reset.) NOTE: D.1 link bring-up + VL805 config-detect needs
ONLY gate 1 (config access uses the controller regs, not the window); the BAR
needs gate 2.

## Recommended order
1. Try gate-1 path (a): flip `VL805=1` on the EEPROM, re-enable the fixed-address
   fallback in `boot_dtb.c parse_pcie` (it is reverted; re-add the BCM2711 block),
   reflash, watch serial. If the controller now reads (no SError) and the brcmstb
   bring-up logs `[pcie] ... link=UP` + `bus1 dev0: VID=1106 ... VL805 DETECTED`,
   gate 1 is solved -> D.1 confirmed.
2. If (a) is not viable, implement gate-1 path (b) (mailbox NOTIFY_XHCI_RESET).
   Make it CRASH-SAFE: confirm via a status read / GET_DOMAIN_STATE before any
   controller MMIO, so a wrong call skips rather than halts.
3. Then gate 2 (seL4 window) + re-enable the xHCI BAR map in pcie_brcmstb.c (D.2),
   reuse `src/usb/xhci.c` Layers 2-5 unchanged -> keyboard on HW. Bump version.h +
   README when it lands.

## Critical gotchas (these WILL bite)
- **NEVER read a power-gated BCM2711 peripheral.** It SErrors -> "halting... Kernel
  entry via Unknown" -> kernel halt -> Pi unreachable -> reflash to recover. Do
  gate 1 (power-on) and confirm BEFORE any controller MMIO read.
- **SERIAL IS ESSENTIAL.** The `[pcie]` detection lines are printf -> mini-UART
  serial only (NOT /proc/log, which is a ring that rotates past boot). Capture with
  `python3 scripts/aios_console.py monitor /dev/cu.usbserial-0001 --baud 115200`
  (USB-serial on GPIO14/15). Without it you are blind.
- **QEMU cannot model brcmstb** -- the RPi4 PCIe path is HW-only. Keep
  `scripts/smp_qemu_test.py` at 7/7 (QEMU is unaffected by RPi4-only changes).
- **Drive the Pi gently over netconsole** (it can wedge); this Pi's netconsole is
  the "one command per line" variant. DHCP gives .8 (real MAC) or .127 (fallback).
- A safe, bootable recovery image is at `disk/sdcard-rpi4.img` (current main:
  plat_pcie_init skipped). Regenerate with `python3 scripts/mksdcard.py` after a
  build. Reflash via BalenaEtcher (root-task change -> no push-over-net).

## Key files
- `src/boot/boot_dtb.c` (parse_pcie -- re-add the BCM2711 fixed-address block)
- `src/plat/rpi4/pcie_brcmstb.c` (bring-up + VL805 detect; add the mailbox power-on)
- `src/plat/rpi4/display_vc.c` (mbox_call -- reuse for the mailbox request)
- `src/usb/xhci.c` (shared Layers 2-5; D.2 sets pcie_xhci_* + pcie_xhci_present)
- `src/boot/boot_device_map.c` / `include/aios/device_map.h` (dev_pcie_vaddr)
- `include/aios/pcie.h`, `include/aios/xhci.h`

## References
- rpi-eeprom firmware-2711 release-notes; RPi booteeprom docs (VL805=1 / vl805.bin).
- U-Boot/Linux pcie-brcmstb (bring-up sequence + offsets -- in DESIGN_USB_HID.md).
- RPI_FIRMWARE_* mailbox tags (Linux include/soc/bcm2835/raspberrypi-firmware.h).

## Conventions
- Develop/verify on QEMU first where possible; the brcmstb path is HW-only (flash +
  serial). No apostrophes in C comments. Commit only when asked. version.h ->
  0.4.184 when USB works on HW. Commit msgs end with:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

## Fallback
Phase D is purely additive -- if it stalls, the USB stack stays QEMU-complete and
the system is HW-verified at v0.4.183. No pressure to land it in one session; it
is a multi-flash HW effort. Bank each gate as it is proven.
