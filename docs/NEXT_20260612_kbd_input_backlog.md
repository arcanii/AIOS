# Keyboard input pipeline -- backlog (2026-06-12)

Captured after the v0.4.203 keystroke-rate fix. Context: the USB HID input
path drops keys when the driver thread is held longer than the interrupt
ring covers. v0.4.203 fixed the two per-keystroke costs (per-key printf to
the polled UART; tty echo running before the SER_KEY_PUSH reply). These are
the structural follow-ups, in value order.

## Current architecture (for orientation)

Three hops, three buffers:

1. Keyboard <- xHCI controller: HARDWARE polling, ~10ms interval
   (boot-protocol interrupt-IN endpoint, via the VL805 hub TT -- the
   keyboard is low-speed). Autonomous while transfers are armed.
   Buffer: the 32-deep multi-arm transfer ring (INT_RING_BUFS, v0.4.192/197)
   = ~320ms of capture cover while the driver thread is busy. If all 32
   drain, the keyboard is UNPOLLED and strokes are lost at the device
   (worst case the LS keyboard wedges -- the v0.4.19x death).
2. Controller -> driver thread: SOFTWARE polling (yield loop pumping the
   event ring, xhci_kbd_driver_fn). IRQ mode exists (/proc/xhci.irq.1, IRQ
   bound) but brcmstb MSI routing is NOT wired on the Pi -- polling is the
   default and the only HW-verified mode. Typematic (host-side key repeat)
   is generated in this loop and pauses while the driver is blocked.
3. Driver -> tty_server: blocking seL4_Call(SER_KEY_PUSH) per keystroke
   (reply now immediate as of v0.4.203). tty buffers: key_buf 512 raw,
   line_buf 256 cooked + line queue.

## 1. Shared-memory key FIFO driver->tty (removes the per-key Call)

Replace the per-keystroke seL4_Call with a single shared frame holding a
lock-free ring (one producer: xhci driver; one consumer: tty_server) plus a
notification to wake the consumer. The driver never blocks on input
delivery again; a busy tty/display chain can no longer drain the USB ring.
Follow the pipe-SHM pattern -- and map BOTH ends with the SAME memory type
(the [[pipe-shm-cache-coherency]] lesson: mismatched cacheability on the
A72 reads stale zeros; QEMU cannot catch it).

Sizing: 512-entry ring of (char, flags) is plenty (matches key_buf).
Also route the root UART RX push (aios_root.c SER_KEY_PUSH callers) through
the same ring so the tty has ONE input spine.

## 2. brcmstb MSI -> true IRQ mode for the xhci driver

Wire MSI routing through the brcmstb PCIe bridge so the driver blocks on
its (already-bound) IRQ notification instead of yield-polling the event
ring. Removes a permanent core-0 yield-spinner and gives the driver
instant wakeups. NOTE: typematic today is polling-mode only -- the repeat
emitter must move to a deadline (timeout on the IRQ wait, or a timer
notification) before IRQ mode can be the default. The .irq.1 opt-in path
exists for incremental testing; it has never been HW-verified.

## 3. Widen the USB cover (cheap insurance, after 1+2)

INT_RING_BUFS 32 -> 64 doubles the unattended-capture window to ~640ms
(64 x 64B stride = 4KB report memory per device -- still one page). Only
worth it if 1+2 leave any residual drain risk; measure first via the
key_events counter vs delivered count.

## 3b. Keyboard auto-recovery after a stall-induced wedge (NEW, high value)

A Source-B quantum (see NEXT_20260612_vl805_dma_stall.md) leaves the LS
keyboard unpolled for 32s+, which wedges it (LED off) until reboot. The
driver already detects the conditions (int-IN error storm -> park; or
int_proc stalling while armed). On detection: USB port reset on the VL805
root/hub port + re-enumerate + re-setup HID (the full setup_hid path already
exists). Turns "dead until reboot" into a ~2s hiccup. Test by inducing a
quantum (type during heavy spawn load on a PCIe-on kernel).

## 4. Stop-Endpoint LED fix (pre-existing backlog, restating)

Runtime SET_REPORT to the LS keyboard behind the TT STALLs with a full
multi-arm ring (HW-proven v0.4.192). A safe runtime lock-LED update needs
Stop Endpoint around the SET_REPORT. Until then lock keys are
software-state only; /proc/xhci.led.N stays as the explicit diagnostic.

## Related open items

- Process-spawn cost: fork=261ms, exec=314-970ms of pipe-server occupancy
  per command (measured by the v0.4.202 [pipe] SLOW probes). This is the
  residual "sticky" feel during command startup and the reason the input
  path needed slack at all. Real fix = spawn optimization (own session).
- The episodic 20-160s whole-system stalls: kernel-independent, per-boot,
  diag probes shipped in v0.4.202 -- see the project_stall_hunt memory.
