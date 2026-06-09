# NEXT 2026-06-09 -- USB HID follow-ups: lock LEDs, IRQ-driven xHCI, multi-device, mouse

Seed for a fresh session. Read with `HANDOVER.md`, the `project_usb_hid` memory, and
`feedback_hdmi_console_cacheable`. The core USB HID stack is DONE + HW-verified: a USB
keyboard types into the AIOS shell on a real Raspberry Pi 4, fully standalone (USB in,
HDMI out, no serial). This seed is four additive follow-ups on top of it.

## Where things are (committed, main, v0.4.185)
- **USB keyboard works on real HW.** `b4187ba` (v0.4.185), `5750ef7` (v0.4.184),
  `c5da05a` (D.1). The shared xHCI driver is `src/usb/xhci.c` (Layers 2-5: controller,
  enumeration, USB hub, HID keymap, a POLLING driver thread). PCIe Layer 1: generic ECAM
  `src/plat/qemu-virt/pcie_ecam.c` (QEMU), brcmstb `src/plat/rpi4/pcie_brcmstb.c` (RPi4,
  `PCIE_PROBE_LEVEL` default 2 = keyboard).
- **Topology on the Pi:** the keyboard is a LOW-SPEED device behind the VL805 INTERNAL
  USB 2.0 hub (`setup_hub`), via a Transaction Translator (TT). All Pi USB-A ports funnel
  through that hub.
- **DMA:** `xhci_dma_reserve()` reserves ONE low (<3GB) 2MB frame EARLY (from aios_root)
  and carves all rings/contexts from it -- RPi4 PCIe DMA MUST be in the low 3GB.
- **Console:** the interactive shell mirrors to HDMI via `tty_server` -> `DISP_CONSOLE`
  (label 116) -> `display_server` -> `fb_console`; the framebuffer is CACHEABLE with a
  per-page clean (`gpu_fb_flush`), so it is fast. See [[feedback_hdmi_console_cacheable]].
- **Driver thread:** `xhci_kbd_driver_fn` (xhci.c) busy-polls the event ring
  (`evt_poll_once` + `seL4_Yield`), spawned from `boot_services.c` when `xhci_kbd_ok`.
  Pinned to `ROOT_CORE` (core 0) like all root threads.

## Conventions / workflow (same as the whole USB arc)
- Develop on QEMU first: `-device qemu-xhci -device usb-kbd` / `-device usb-mouse` /
  `-device usb-hub`. Tests: `scripts/xhci_key_qemu_test.py`,
  `scripts/xhci_hub_key_qemu_test.py` (keyboard behind a hub). Inject input via the HMP
  monitor (`sendkey`, `mouse_move`, `mouse_button`).
- RPi4 deploy is FLASH-FREE for kernel/root changes: `ninja -C build-rpi4`,
  `python3 scripts/mkkernel8.py`, `cp disk/kernel8.img /Volumes/AIOSBOOT/kernel8.img`,
  `diskutil eject AIOSBOOT`, reinsert + boot ([[feedback_flashfree_kernel]]). The HDMI
  console is responsive now, so you can drive the Pi from the monitor + USB keyboard
  without serial (serial via `aios_console.py monitor /dev/cu.usbserial-0001` if needed).
- Build BOTH `build-04` (QEMU) + `build-rpi4` after shared changes. No apostrophes in C
  comments. Commit only when asked; bump `version.h` + commit when a task lands on HW.
  Commit msgs end with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

---

## Task 1 -- USB lock LEDs (Num/Caps/Scroll Lock light)

**State:** the lock STATE is tracked in software (v0.4.185): `num_lock`/`caps_lock` in
xhci.c, `hid_to_ascii` gates the numpad on `num_lock` and case on `caps_lock`. The
physical LED is NOT lit -- it was implemented then REVERTED in `b4187ba` because it
DESTABILISED a real keyboard (see below). The LED code is NOT in git; you re-add it.

**The LED mechanism (re-add this):** keyboard LEDs are host-driven. On a lock-key press
the host sends a HID Output report via SET_REPORT on EP0:
```c
/* led_buf = a 1-page DMA buffer (dma_page) allocated in setup_keyboard. */
led_buf[0] = (num_lock?1:0) | (caps_lock?2:0) | (scroll_lock?4:0);  /* bit0 Num, 1 Caps, 2 Scroll */
control_transfer(0x21, 0x09 /*SET_REPORT*/, 0x0200 /*Output report, id 0*/,
                 kbd_iface, 1, led_buf_pa, 0 /*OUT*/);
```
Lock keycodes: Num Lock 0x53, Caps Lock 0x39, Scroll Lock 0x47. In `process_report`,
on a new press of those, toggle the state + call `set_leds()`. Also call it once at the
end of `setup_keyboard` to apply the initial (Num Lock on) state.

**THE BLOCKER (why it was reverted):** calling that SET_REPORT at ENUMERATION (setup,
on the main thread) was fine, but calling it at RUNTIME from the POLLING DRIVER THREAD
(inside `process_report`, on a lock-key press) KILLED the keyboard -- all input stopped.

**Root cause is UNCONFIRMED -- DIAGNOSE before fixing.** The obvious "the control
transfer steals an interrupt-IN event from the shared event ring" theory is WEAK: when
`process_report` runs, the interrupt EP has just completed (its event was consumed) and
is NOT re-armed until `arm_int` AFTER `process_report`, so no interrupt events should be
in flight during `set_leds`. So something else breaks. Instrument it: log the SET_REPORT
completion code + `USBSTS` (HSE/HCE) right after; after the call, read the hub downstream
PORT STATUS and the device slot state -- does the keyboard STALL EP0, re-enumerate
(connection bounce), or does the controller flag an error? Candidate fixes once you know:
  (a) **Endpoint-aware control_transfer** -- the transfer-event TRB carries the Endpoint
      ID (`e[3]` bits [20:16]) + Slot ID (bits [31:24], `EVT_SLOT`). Make control_transfer
      only consume events for ITS EP0/slot and route others to the keyboard handler.
      (Useful for multi-device too -- Task 3.)
  (b) **EP0 halt recovery** -- if the SET_REPORT STALLs (cc=6), EP0 is halted: issue
      Reset Endpoint + Set TR Dequeue Pointer before the next transfer.
  (c) **LS-via-TT control-OUT timing** -- a low-speed control-OUT through the TT may need
      a longer timeout or a different setup; compare with how the IN control transfers
      (which work) differ.
  (d) **Re-enumeration** -- if the keyboard bounces, handle the connection change.
**Verify on QEMU:** `usb-kbd` handles SET_REPORT (no visible LED), and the QEMU tests
must still type AFTER a simulated lock-key press (the exact regression). Then HW: the
NumLock light should toggle, and input must survive.

---

## Task 2 -- IRQ-driven xHCI (replace the busy-poll; free core 0)

**State:** `xhci_kbd_driver_fn` busy-polls + `seL4_Yield`s, pinned to core 0 with the
root servers. It works but wastes CPU and adds latency.

**Goal:** the xHCI interrupter (IR0) raises an interrupt when it enqueues an event; route
it to the CPU, bind a seL4 IRQ handler, and have the driver thread BLOCK (`seL4_Wait` on
the IRQ notification) instead of spinning. Then it consumes 0 CPU when idle.

**What it takes:**
- Enable interrupts: `USBCMD.INTE` (bit 2) + interrupter `IR0.IMAN.IE` (bit 1); set
  `IR0.IMOD` for moderation. On each IRQ, drain the event ring, then re-arm by writing
  `ERDP` with the EHB (Event Handler Busy, bit 3) bit set to clear it.
- **Route the PCIe interrupt to the GIC -- the hard, platform-specific part:**
  - QEMU: the `qemu-xhci` uses MSI-X (table in a BAR) by default, or legacy INTx via the
    PCI host bridge `interrupt-map` (DTB) to a GIC SPI. INTx is simpler if wired -- check
    the virt PCIe node's `interrupt-map` in the DTB.
  - RPi4 brcmstb: the VL805 raises MSI -> the brcmstb RC has an internal MSI controller
    (the `msi-controller` / `msi-parent` in the pcie DTB node; the MSI_INTR2 registers
    at 0x4500-0x4510 already touched in pcie_brcmstb.c -- they were MASKED there). You
    must set up the MSI target + unmask, then bind the resulting GIC SPI. This is the
    meaty bit; develop QEMU first.
- **Bind the IRQ in seL4:** follow the GENET pattern (`net_genet.c` + `boot_net_init` --
  a dedicated seL4 IRQ handler + notification, [[feedback_net_irq]]) and the UART IRQ in
  `aios_root.c`. Get an `IRQHandler` cap for the SPI, set a notification, `seL4_Wait` on
  it in the driver thread, `Ack` after draining.
**Suggested order:** QEMU INTx (or MSI) first -> the driver thread blocks on the notif ->
then the brcmstb MSI path on HW. Keep the polling path as a fallback (`#ifdef`/runtime)
until IRQ is HW-proven, since the brcmstb MSI is unproven.

---

## Task 3 -- Multi-device support (keyboard + mouse, multiple devices)

**State:** xhci.c is SINGLE-device: the globals `dev_slot`, `dev_ctx`, `in_ctx`,
`ep0_ring`/`ep0_enq`/`ep0_cycle`, `int_ring`, `rpt`, `kbd_*` hold ONE device. `setup_hub`
finds the FIRST connected downstream port and stops.

**Goal:** support N devices (a keyboard AND a mouse, or multiple). The DCBAA already
indexes by slot, so the controller supports it; the DRIVER state must become per-device.

**What it takes:**
- A `struct usb_dev { uint32_t slot; volatile uint8_t *dev_ctx,*in_ctx,*ep0_ring;
  uint32_t ep0_enq,ep0_cycle; volatile uint8_t *int_ring,*rpt; uint64_t int_ring_pa,...;
  uint32_t int_enq,int_cycle,dci,mps,iface; int kind; ... }` array, replacing the globals.
  `control_transfer`/`cmd_submit`/`arm_int`/`process_report` take a `usb_dev*`.
- Enumerate ALL connected ports: in `setup_hub`, after finding one device, KEEP scanning
  the other downstream ports; and the root ports in `xhci_init`. Each -> its own slot +
  contexts + interrupt EP, registered in the device array.
- The driver loop dispatches each transfer event to the right device by SLOT + ENDPOINT
  (needs the endpoint-aware event handling from Task 1 -- they pair well). On a transfer
  event, look up the device by `EVT_SLOT(e[3])` + endpoint id, decode its report, re-arm.
- Watch the DMA pool: more devices = more rings/contexts. The 2MB low pool
  (`DMA_POOL_PAGES 512`) holds ~512 pages; a few devices fit, but size-check.
**Verify on QEMU:** `-device usb-kbd -device usb-mouse` (both behind `-device usb-hub`
to mirror the Pi). Both should enumerate + report.

---

## Task 4 -- USB mouse

**State:** none. The HID layer only sets up a boot KEYBOARD.

**Goal:** enumerate a boot MOUSE (HID interface protocol 2, vs 1 for keyboard), decode
its reports, and feed them to a consumer.

**What it takes:**
- In the config-descriptor parse (`setup_keyboard`, generalise to `setup_hid`), the
  interface descriptor `bInterfaceProtocol` is 1=keyboard, 2=mouse. Arm the interrupt-IN
  EP either way; for a mouse use `SET_PROTOCOL(boot)` then decode the 3-4 byte boot
  report: byte0 = buttons (bit0 L, 1 R, 2 M), byte1 = dx (signed), byte2 = dy (signed),
  byte3 = wheel (if present).
- **Where do mouse events GO?** No consumer exists -- decide one:
  (a) Simplest first cut: decode + log (`[xhci-mouse] dx=.. dy=.. btn=..`) to PROVE the
      mouse enumerates + reports on HW.
  (b) Then a destination: a `/dev/input`/`/dev/mouse` device + an event queue, OR draw a
      CURSOR on the framebuffer via `display_server` (a new DISP op + the cacheable-FB
      flush), OR a simple shared mouse-state in /proc.
- The mouse, like the keyboard, will be behind the VL805 hub and LS/FS -- needs Task 3
  (multi-device) if you want keyboard + mouse together, and benefits from Task 1's
  endpoint-aware events.
**Verify on QEMU:** `-device usb-mouse`; inject motion via HMP `mouse_move dx dy` /
`mouse_button`.

---

## Suggested order
1. **Task 1 diagnosis + endpoint-aware control_transfer** -- it is the smallest, unblocks
   the LED, AND the endpoint-aware event routing is the foundation for Tasks 3 + 4.
2. **Task 3 (multi-device)** -- builds directly on endpoint-aware events.
3. **Task 4 (mouse)** -- a second HID device on top of multi-device.
4. **Task 2 (IRQ-driven)** -- independent + the most platform-risky (brcmstb MSI); do it
   when you want to reclaim core-0 CPU. Keep polling as a fallback until HW-proven.

## Key files
- `src/usb/xhci.c` -- the driver (control_transfer, cmd_submit, process_report,
  setup_keyboard, setup_hub, address_and_describe, xhci_kbd_driver_fn, the single-device
  globals, the DMA pool).
- `src/plat/rpi4/pcie_brcmstb.c` + `src/plat/qemu-virt/pcie_ecam.c` -- PCIe (MSI/INTx for
  Task 2; brcmstb MSI_INTR2 regs are at 0x4508/0x4510, currently masked).
- `src/boot/boot_services.c` -- spawns the driver thread (`xhci_kbd_driver_fn`).
- `src/plat/rpi4/net_genet.c` + `src/aios_root.c` (UART IRQ) -- IRQ-binding references.
- `include/aios/xhci.h`, `include/aios/pcie.h`, `include/aios/root_shared.h` (IPC labels).
- Tests: `scripts/xhci_key_qemu_test.py`, `scripts/xhci_hub_key_qemu_test.py`.

## Fallback
All four are purely additive. The keyboard + standalone HDMI console are HW-verified and
committed -- nothing here risks that. Bank each task as it proves out on HW.
