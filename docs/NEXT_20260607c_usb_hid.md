# NEXT 2026-06-07c -- HCI: USB HID keyboard (standalone Pi)

Seed for a fresh session. Read with `HANDOVER.md`, the full design in
`docs/DESIGN_USB_HID.md`, and the memory index (`MEMORY.md`). The previous arc
(SMP + process capacity + shared .text, through v0.4.183) is committed; v0.4.183
(shared .text) is QEMU-verified and pending a HW flash-verify -- close that out
first if not already done.

## TL;DR -- the goal and the first step
**Goal:** USB keyboard input on the RPi4 so it is a standalone computer -- the
HDMI framebuffer is already lit (v0.4.168), this adds the input half. **First
step:** Phase A -- generic PCIe ECAM enumeration on QEMU virt, discover the
qemu-xhci controller, program its BAR. That is the foothold the whole stack
builds on, and it is 100% QEMU work.

## Why this, why now
- HDMI out works; keyboard in is the missing half of a usable standalone Pi.
- The full design already exists in `docs/DESIGN_USB_HID.md` (5 layers: PCIe ->
  xHCI -> enumeration -> HID -> keymap), now reframed QEMU-first.

## The key strategy -- QEMU-first (do NOT develop blind on HW)
QEMU `-machine virt` models the entire USB stack:

    -device qemu-xhci,id=xhci -device usb-kbd

Type in the QEMU display window (or `sendkey` on the monitor). Layers 2-5 (xHCI,
enumeration, HID, keymap -- ~3,500 of ~5,000 lines, and the parts you iterate
most) are IDENTICAL on QEMU and the Pi. Only Layer 1 (PCIe root complex) differs:
generic ECAM on QEMU vs brcmstb on the Pi. So build A-C on QEMU until you can
type into the AIOS shell, then write ONLY the brcmstb backend (Phase D) and reuse
Layers 2-5. Full split + file layout: `docs/DESIGN_USB_HID.md` "Development
Strategy".

## File layout (from the design)
- Shared, platform-independent: `src/usb/xhci.c`, `usb_enum.c`, `hid_keyboard.c`,
  `usb_keymap.c`.
- Per-platform Layer 1 behind `src/plat/input_hal.h`:
  `src/plat/qemu-virt/pcie_ecam.c` (build first), `src/plat/rpi4/pcie_brcmstb.c`
  (Phase D, HW).
- Integration point (confirmed in current code): feed each decoded char to
  tty_server via `SER_KEY_PUSH` (label 4) -- the SAME path UART input uses today
  (see `src/apps/tty_server.c`). No new consumer needed.

## Phase A -- the immediate task (QEMU)
1. Add `parse_pcie()` to the DTB parser for `pci-host-ecam-generic` (ECAM base
   from `reg`, bus-range, MMIO/IO `ranges`). Stash in `hw_info` (add
   `pcie_paddr`, `has_pcie`).
2. `src/plat/qemu-virt/pcie_ecam.c`: map the ECAM region (sel4 device untyped /
   the existing MMIO-claim path), config-space read/write helpers, enumerate
   bus 0.
3. Find qemu-xhci (class 0x0C0330), read BAR0 size (write-all-ones probe),
   assign it a window from the host bridge `ranges`, write BAR0, enable
   MMIO + bus-master in the command register.
4. **Done when:** AIOS_LOG prints `xHCI found at <bus:dev.fn>, BAR0=<addr>` on the
   QEMU boot. Then move to Phase B (xHCI init) per the design doc.

Phases B (xHCI init), C (enum + HID + keymap -> type in QEMU), D (brcmstb PCIe on
HW) are spelled out in `docs/DESIGN_USB_HID.md` "Phased Implementation".

## Gotchas
- **seL4 device memory:** PCIe ECAM + the xHCI BAR are device-untyped MMIO. Claim
  them ascending-paddr like the other RPi4 peripherals on HW
  ([[feedback_sel4_device_untyped_order]]); on QEMU the virt MMIO window is
  flexible. Map xHCI registers + DMA rings with the right memory attributes.
- **DMA coherency:** xHCI rings/contexts are read by the controller via DMA. On
  the A72 these must be coherent -- map them non-cacheable OR clean/invalidate
  around each access (QEMU will NOT catch a missing barrier; same class of trap as
  the pipe-SHM and demand-text I-cache bugs -- [[feedback_pipe_shm_cache]]).
  Decide the policy in Phase B and keep it consistent.
- **64-bit addresses:** xHCI contexts/rings use 64-bit physical pointers; get the
  paddr of each allocated frame (the vka cookie / vspace paddr helper), do not
  hand it a vaddr.
- **IRQ:** start with event-ring POLLING (a driver thread) to get keystrokes
  working; wire the real IRQ (legacy INTx via GIC on QEMU; MSI later) after the
  datapath is proven.
- **Phase D only:** the VL805 may need a firmware load, or the RPi firmware may
  have already powered + trained it -- check before assuming a cold bring-up.

## How to test
- **QEMU (A-C):** add `-device qemu-xhci,id=xhci -device usb-kbd` to the boot
  command (see README "Boot"); type in the QEMU window. Each phase has a concrete
  done-signal (see above + design doc). Keep `scripts/smp_qemu_test.py` green.
- **HW (D):** flash build-rpi4, plug a USB keyboard into a USB-A port, type, see
  it echoed on HDMI / over netconsole.

## Where things are (current state, v0.4.183)
- RPi4: 4-core SMP, GENET networking, HDMI lit, eMMC, SSH + netconsole over LAN.
- No PCIe/USB code yet -- greenfield. `src/plat/` has the HAL pattern
  (`blk_hal.h`/`net_hal.h`/`display_hal.h` + `qemu-virt/` + `rpi4/` backends);
  add `input_hal.h` the same way.
- Input today: UART IRQ -> `SER_KEY_PUSH` -> tty_server -> shell.

## Conventions
- QEMU-first; flash only for Phase D (the brcmstb backend). No apostrophes in C
  comments. Commit only when asked. Bump `version.h` per milestone. Commit msgs
  end with:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

## Scale note
This is the biggest subsystem the project has taken on (~5,000 lines; xHCI is the
long pole at ~2,500). It is multi-session -- land it phase by phase, each with a
green QEMU test, rather than in one push.
