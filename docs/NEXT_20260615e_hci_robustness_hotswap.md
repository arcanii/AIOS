# NEXT: HCI (xHCI/USB) robustness + keyboard HOTSWAP (queued for next week)

Bryan (2026-06-15): next week, look at HCI robustness and device (keyboard) HOTSWAP --
plug/unplug a USB keyboard at runtime and have it (re)enumerate WITHOUT a reboot. Builds
on [[project_usb_hid]] (PCIe + xHCI + HID stack, HW-verified). Driver: src/usb/xhci.c;
design: docs/DESIGN_USB_HID.md.

## Current state (code-traced)

- **Enumeration is BOOT-ONLY.** `xhci_init()` (xhci.c:1330-1333) scans the root ports
  (PORTSC) once at boot; `setup_device()` (1188) + `setup_hub()` (1084, the VL805 internal
  hub, polled ~2.5s via HUB_GET_STATUS) are `static`, called only from that boot loop.
- **Port Status Change events are DISCARDED.** `TRB_PORT_STS_EVT` (type 34) arrives on the
  event ring but `evt_dispatch()` (xhci.c:177-200) drops it at line ~199 (only disarms
  typematic) -- no port decode, no handler.
- **No device teardown.** On unplug nothing frees the slot: no DISABLE_SLOT, DCBAA[slot]
  not cleared, and the DMA pool (`dma_page()`, 283-291) only INCREMENTS `dma_pool_used`
  (512-page / 2MB cap) -- 6 pages/device leak forever. `g_devs[MAX_USB_DEV=8]` fills up.
- Event ring is single-consumer (the `xhci_kbd_driver_fn()` loop, 950-999) -- any hotswap
  handler MUST run INLINE there, not in a separate task (race on evt_dispatch + g_devs).

## HOTSWAP -- gaps + first steps (from the scope)

Missing constants (add in xhci.c): `PORTSC_CSC (1u<<17)` connect-status-change,
`PORTSC_PLC (1u<<22)` port-link-change, `TRB_DISABLE_SLOT 10`, `EVT_PORT(trb3)
(((trb3)>>24)&0xFF)`.

1. **Decode PORT_STS_EVT** in `evt_dispatch()` (xhci.c:199): extract the port via
   `EVT_PORT(e[3])`, read PORTSC, dispatch `handle_port_status_change(port, PORTSC&CCS)`.
2. **`device_teardown(struct usb_dev*)`** (~50 LOC): DISABLE_SLOT cmd -> DCBAA[slot]=0 ->
   in_use=0 -> (add DMA-pool reclaim for the 6 pages: dev_ctx/in_ctx/ep0_ring/int_ring/
   rpt/led_buf -- pool tracking does not exist yet, add a freelist or per-device page list).
3. **`device_enumerate(root_port)`** (~20 LOC): factor the boot-loop body (port_reset ->
   speed via PORTSC -> dev_alloc -> address_and_describe -> cls==9 setup_hub / cls==3
   setup_hid) into a PUBLIC wrapper reused by both `xhci_init()` and the hotswap handler.
4. **`handle_port_status_change(port, ccs)`** (~100 LOC): `!ccs` -> find g_devs[] by port
   -> device_teardown; `ccs` -> port_reset + device_enumerate.
5. **VL805 downstream hotplug (RPi4):** the keyboard sits behind the VL805 HS hub, so root
   PORT_STS_EVT alone is not enough -- ARM the hub's interrupt-IN status-change endpoint
   (EP 0x81), parse the port-change bitmap, and re-drive HUB_GET_STATUS on the changed
   downstream port. (Interim failsafe: periodic PORTSC CSC poll in the driver loop, W1C to
   clear.) Watch PORTSC_PLC for LS/FS devices behind the TT.
6. **Slot reuse:** each replug burns a new xHCI slot (ENABLE_SLOT) until MAX_USB_DEV(8)
   exhausts -- teardown must actually release the slot so replug reuses it.

Key refs: xhci.c boot loop 1330, evt_dispatch 177-200, setup_device 1188, setup_hub 1084,
address_and_describe 1012-1077, setup_hid 772-880, ep0_recover 561-592 (RESET_EP type14 +
SET_TR_DEQ type16), dma_page 283-291, g_devs/usb_dev 366-417, cmd_submit 210-230.

## ROBUSTNESS -- known items (needs the deep-dive pass; the scoping agent failed to return)

From [[project_usb_hid]] + the refs above:
- first-key-death RESOLVED v0.4.192, hardened v0.4.197 (32-deep event ring + typematic
  runaway guards, HW-verified incl reboot) -- check for residual fragility under stress.
- Endpoint halt/stall recovery exists (`ep0_recover`: RESET_EP + SET_TR_DEQ) -- audit
  whether the HID interrupt-IN endpoint (not just EP0) gets the same recovery on STALL.
- the Stop-Endpoint LED fix is BACKLOGGED; lock-LED is software-only.
- DMA-pool leak (above) is itself a robustness bug (long-run / many-replug exhaustion).
- command timeouts / controller reset+recovery (xHC fatal error -> HCRST) -- verify
  there is a watchdog, not an unbounded wait, on cmd_submit / event waits.
- TODO NEXT WEEK: re-run a read-only scope of the xHCI error/recovery paths (the workflow
  lens that failed) -- event-ring/transfer-ring error TRB handling, command-ring timeouts,
  endpoint-halt recovery for the HID EP, and controller-level reset.

## Order

Hotswap is the bigger, well-scoped piece (steps 1-6) and is HW-only to verify (real Pi,
real keyboard plug/unplug; QEMU's xHCI can model connect/disconnect too -- worth a QEMU
smoke first via `device_add`/`device_del usb-kbd`). Robustness is incremental hardening on
top. Suggest: hotswap root-port first (QEMU-testable), then the VL805 downstream hub
(HW-only), then the robustness sweep.
