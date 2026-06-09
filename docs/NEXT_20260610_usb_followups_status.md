# NEXT 2026-06-10 -- USB HID follow-ups DONE (merged v0.4.186) + open HW items

Handover after the four USB HID follow-ups from `docs/NEXT_20260609_usb_followups.md`
(lock LEDs, multi-device, mouse, IRQ-driven xHCI), plus a Ctrl-key fix and an HDMI
scroll-freeze diagnostic. **Merged to `main` at v0.4.186.** Read with the `project_usb_hid`
memory and `feedback_hdmi_console_cacheable`.

All work is on the SHARED tree, so it ships to both QEMU + RPi4. The default driver
behaviour on hardware is UNCHANGED (polling, single keyboard as in v0.4.185) -- every
addition is additive, and the one risky piece (IRQ mode) is opt-in + defaults off.

## Status at a glance
| Item | QEMU | RPi4 HW |
|------|------|---------|
| Task 1 -- lock LEDs (Num/Caps/Scroll) | verified | **HW-VERIFIED** (Num+Caps lights work) |
| Ctrl modifier (Ctrl-C = 0x03, etc.) | verified | pending confirm (flashed) |
| Task 3 -- multi-device (kbd+mouse) | verified | single-kbd works; multi untested |
| Task 4 -- mouse + /proc/mouse | verified | untested |
| Task 2 -- IRQ-driven xHCI | verified (INTx, IRQ 37) | **polling only** (brcmstb MSI pending) |
| HDMI scroll | logic verified (ramfb) | **FREEZES -- open, diagnostic shipped** |

Full QEMU suite: 9/9 green (`scripts/xhci_*_qemu_test.py`, `scripts/fb_scroll_qemu_test.py`).

## Live tools added (both platforms, no reflash to use)
- `cat /proc/xhci` -- controller + per-device + LED + IRQ snapshot.
  `.led.N` (1 Num, 2 Caps, 4 Scroll), `.lock`, `.irq.0|1` pokes (driver-thread applied).
- `cat /proc/mouse` -- system mouse state (x, y, buttons, events).
- `cat /proc/fbcon` -- fb_console scroll/flush phase + per-page flush progress (HW-freeze).

---

## OPEN ITEM 1 (priority) -- HDMI console freezes on the first scroll

**Symptom (HW):** once the keyboard fills the screen and fb_console must scroll, the
display stops updating and the keyboard wedges, but AIOS keeps pinging.

**Cascade (confirmed by design):** `display_server` wedges in the scroll ->
`tty_server` blocks on its `DISP_CONSOLE` Call -> the USB driver blocks on `SER_KEY_PUSH`
-> keyboard dead + screen frozen. `net_driver`/`net_server` are independent -> still pings.

**Not a logic bug:** QEMU with `-device ramfb` runs the SAME `fb_console.c` scroll path and
survives 182 scrolls (`scripts/fb_scroll_qemu_test.py`). The bounds are correct at
1024x768 (768 pages, fully mapped), and the full-FB cache clean is the same one the boot
splash does successfully. So it is specific to the real CACHEABLE framebuffer, isolated to
the scroll's **3 MB overlapping `memmove` + per-page clean** on the GPU-reserved region --
the one operation boot never exercises (the splash only does direct writes).

**Diagnostic shipped:** `/proc/fbcon` (read it over netconsole AFTER a freeze -- the net
threads survive). It shows `scrolls=N phase=P(memmove|clear|mark|flush|done)` and
`flush: pages=done/total`:
- `phase=1(memmove)` => hung in the 3 MB scroll memmove.
- `phase=4(flush) pages=K/760` => hung cleaning page K (a specific physical page).
- `phase=5/idle` while still frozen => the hang is NOT in fb_console; it is the IPC layer /
  display_server stuck elsewhere -- check `/proc/serverstats`.

**Next steps:** flash, repro, `cat /proc/fbcon` + `/proc/serverstats` over netconsole; also
note whether SERIAL freezes too and whether any VM-fault/abort appears in `/proc/log`. Then
the targeted fix follows (likely candidates: replace the musl `memmove` with a manual
clean-as-you-go row copy if it is the memmove; or chunk/skip the per-scroll full clean if
it is the flush). Files: `src/boot/fb_console.c` (scroll_up/flush), `src/boot/boot_display_init.c`
(gpu_fb_flush), `src/plat/rpi4/display_vc.c` (cacheable FB map). NOTE: this is a
PRE-EXISTING display issue (the scroll path predates this arc) -- it is unrelated to the
USB code; the USB keyboard just made it reachable.

## OPEN ITEM 2 -- Task 2 brcmstb MSI (enable IRQ mode on the Pi)
`plat_pcie_xhci_irq()` in `src/plat/rpi4/pcie_brcmstb.c` returns -1, so the Pi stays on the
proven polling driver. To enable IRQ mode there (reclaim core-0 CPU), finish the brcmstb MSI
bring-up ON the Pi (QEMU cannot model it): program the RC MSI target/data, the VL805 MSI
capability, unmask `MSI_INTR2`, and return the GIC SPI from the pcie DTB `msi-parent`. The
shared driver loop (drain-then-recheck-before-`seL4_Wait`, IRQ 37 path) is already proven on
QEMU via INTx; only the routing is platform work. Then `cat /proc/xhci.irq.1` on the Pi.

## OPEN ITEM 3 -- HW confirm the additive features
Plug a keyboard AND a mouse into the Pi: `cat /proc/xhci` should list `dev[..] kbd` +
`dev[..] mouse` and both work; `cat /proc/mouse` should track motion. Confirm Ctrl-C
interrupts a command (e.g. `sleep 30` then Left-Ctrl+C). Single keyboard is the only
HW-proven config so far.

## Key files
- `src/usb/xhci.c` -- the whole driver (struct usb_dev array, evt_dispatch endpoint-aware
  events, set_leds, ctrl_char, process_mouse_report, IRQ mode, xhci_diag_cmd/xhci_mouse_state).
- `src/plat/qemu-virt/pcie_ecam.c` + `src/plat/rpi4/pcie_brcmstb.c` -- `plat_pcie_xhci_irq()`.
- `src/boot/fb_console.c` + `src/boot/boot_display_init.c` -- scroll + /proc/fbcon diag.
- `src/servers/display_server.c`, `src/apps/tty_server.c` -- the output/mirror IPC chain.
- `src/procfs.c` -- /proc/{xhci,mouse,fbcon} wiring.
- Tests: `scripts/xhci_{key,hub_key,ctrl,led,proc,multidev,mouse,irq}_qemu_test.py`,
  `scripts/fb_scroll_qemu_test.py`.

## Deploy (flash-free, root-task only)
`ninja -C build-rpi4` -> `python3 scripts/mkkernel8.py` -> `cp disk/kernel8.img
/Volumes/AIOSBOOT/kernel8.img` -> `diskutil eject AIOSBOOT` -> reinsert + boot. No disk
rebuild needed (no dash/sbase/getty/sshd change).
