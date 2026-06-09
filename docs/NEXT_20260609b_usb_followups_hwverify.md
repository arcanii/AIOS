# NEXT 2026-06-09b -- USB HID follow-ups: QEMU-COMPLETE, HW-verify pending

Follow-up to `docs/NEXT_20260609_usb_followups.md`. All FOUR tasks (lock LEDs,
multi-device, mouse, IRQ-driven xHCI) are IMPLEMENTED and QEMU-VERIFIED. Both `build-04`
and `build-rpi4` compile. UNCOMMITTED and NOT yet flashed to the Pi -- this seed is the
HW-verification checklist. Bump `version.h` + commit each task as it proves out on HW
(the convention from the prior seed).

The default driver behaviour is UNCHANGED on hardware (polling, single-keyboard works as
in v0.4.185); every new capability is additive and, for the risky one (IRQ mode),
opt-in. So flashing this is low-risk: the worst case is "same as before".

## What changed (all in the shared tree, so both platforms get it)
- `src/usb/xhci.c` -- the bulk. Endpoint-aware event dispatch, lock LEDs, multi-device
  `struct usb_dev` array, mouse, IRQ mode, `/proc/xhci` + `/proc/mouse`.
- `src/plat/qemu-virt/pcie_ecam.c` -- `plat_pcie_xhci_irq()` (QEMU INTx -> GIC SPI).
- `src/plat/rpi4/pcie_brcmstb.c` -- `plat_pcie_xhci_irq()` (brcmstb MSI, returns -1 =
  poll, HW bring-up pending -- see Task 2).
- `src/procfs.c`, `include/aios/xhci.h`, `include/aios/pcie.h` -- wiring.
- New QEMU tests: `scripts/xhci_{led,proc,multidev,mouse,irq}_qemu_test.py`.

## New live tools (both platforms, no reflash to use)
- `cat /proc/xhci` -- controller + per-device + LED + IRQ snapshot.
- `cat /proc/xhci.led.N` -- poke lock LEDs (hex: bit0 Num, 1 Caps, 2 Scroll). The DRIVER
  thread issues the SET_REPORT (the proc thread only pokes a request -- the event ring
  has a single safe consumer), so this is the reflash-free way to diagnose the LED.
- `cat /proc/xhci.lock` -- set LEDs from the current Num/Caps/Scroll software state.
- `cat /proc/xhci.irq.1` / `.irq.0` -- switch the driver to IRQ mode / back to polling.
- `cat /proc/mouse` -- system mouse state (x, y, buttons, event count).

---

## Task 1 -- lock LEDs (the original HW blocker)
QEMU: `xhci_led_qemu_test.py` (typing survives runtime SET_REPORTs -- the exact
regression), `xhci_proc_qemu_test.py` 6/6 (poke -> SET_REPORT cc=1, no HSE/HCE).

Root-cause fixes layered in (any of which may have been the HW cause):
- **Endpoint-aware `control_transfer`** -- a control transfer now waits for ITS own
  EP0 completion and delivers any interrupt-IN report that arrives meanwhile, instead
  of consuming the first transfer event it sees.
- **EP0 ring Link TRB** -- the EP0 ring never wrapped before (latent: it would wedge
  after 255 TRBs once runtime SET_REPORTs cycled it). Now it has a Link + cycle toggle.
- **EP0 STALL recovery** -- Reset Endpoint + Set TR Dequeue if a SET_REPORT STALLs.
- **USBSTS HSE/HCE logging** after every SET_REPORT.

**HW verify on the Pi (standalone, USB kbd + HDMI):**
1. Flash-free deploy: `ninja -C build-rpi4`, `python3 scripts/mkkernel8.py`,
   `cp disk/kernel8.img /Volumes/AIOSBOOT/`, eject, reinsert, boot.
2. Press Num Lock on the USB keyboard. EXPECT: the NumLock light toggles AND typing
   keeps working (the v0.4.185 regression was that input stopped).
3. If input still dies: do NOT guess -- `cat /proc/xhci` over netconsole/serial and read
   the `last SET_REPORT cc=` + `USBSTS` + the `dev[..] state=` line. cc=6 STALL,
   HSE/HCE=1 controller fault, state dropping to 1/2 = re-enumeration. Then
   `cat /proc/xhci.led.1` / `.led.0` to retrigger live and watch which it is -- all
   without reflashing. The candidate fixes are in `set_leds`/`ep0_recover`.

## Task 3 -- multi-device
QEMU: `xhci_multidev_qemu_test.py` (hub + keyboard + mouse all enumerate; keyboard types,
mouse reports). The single-device globals are now a `struct usb_dev g_devs[8]` array;
`setup_hub` enumerates ALL downstream ports; `xhci_init` enumerates ALL root ports.

**HW verify:** plug a keyboard AND a mouse into the Pi (both behind the VL805 hub).
`cat /proc/xhci` should list `dev[..] kbd` and `dev[..] mouse`. The keyboard must still
type. (Single keyboard is the only HW-proven config so far; multi is QEMU-only.)

## Task 4 -- mouse
QEMU: `xhci_mouse_qemu_test.py` 3/3 (`/proc/mouse` shows the cursor accumulate + clicks).
Boot mouse decode (buttons/dx/dy/wheel) + a `/proc/mouse` consumer. A framebuffer cursor
via `display_server` is a sensible NEXT (it would read the same `g_mouse_*` state).

**HW verify:** move a USB mouse, `cat /proc/mouse` -- x/y/buttons/events should change.

## Task 2 -- IRQ-driven xHCI (opt-in; default stays polling)
QEMU: `xhci_irq_qemu_test.py` 5/5 -- the xHCI INTx line routes through gpex to GIC IRQ 37,
the driver BLOCKS on it (`seL4_Wait`), and a marker typed on the keyboard echoes back
while blocked (count=22 wakeups). This proves the whole IRQ datapath on QEMU.

**RPi4 is the open HW item.** `plat_pcie_xhci_irq()` in `pcie_brcmstb.c` returns -1, so
the Pi STAYS ON POLLING (safe, unchanged). To enable IRQ mode on the Pi, finish the
brcmstb MSI bring-up (the meaty bit the original seed flagged), all on the Pi since QEMU
cannot model it:
1. Program the RC MSI target addr (`PCIE_MISC_MSI_BAR_CONFIG_LO/HI`) + data
   (`PCIE_MISC_MSI_DATA_CONFIG`).
2. Program the VL805's MSI capability (in its PCI config) to that target/data.
3. Unmask the MSI in `MSI_INTR2_MASK_CLR` (it is currently MASKED in `pcie_bringup`).
4. Find the GIC SPI the RC raises for MSI from the pcie DTB node (`msi-parent` /
   `interrupts`) and return `32 + SPI` from `plat_pcie_xhci_irq()`.
5. Then `cat /proc/xhci.irq.1` on the Pi and confirm the keyboard still types and
   `/proc/xhci` `count` climbs. `.irq.0` reverts to polling if anything is off.
The driver loop (drain-then-recheck-before-Wait, latched notification) is already correct
and shared -- only the routing in step 1-4 is platform work.

## Suggested HW session order
Task 1 (the real win -- the LED light) -> Task 3/4 (plug in kbd+mouse, read /proc) ->
Task 2 brcmstb MSI (separate, only if you want to reclaim core-0 CPU). Bank + commit each
as it proves out.
