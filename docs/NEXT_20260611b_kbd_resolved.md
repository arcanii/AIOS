# 2026-06-11b — USB keyboard first-key death RESOLVED (v0.4.192, HW-verified)

Close-out for docs/NEXT_20260611_hw_kbd_wedge.md. The regression is FIXED and
v0.4.192 (build 2038) is HW-verified on the real RPi4: full login + commands +
key repeat on the USB keyboard, across power cycles.

## Root cause (the real one, after several wrong turns)

The xHCI keyboard driver armed ONE interrupt-IN transfer at a time and re-armed
it only AFTER the blocking key delivery (`seL4_Call(SER_KEY_PUSH)` -> tty ->
`DISP_CONSOLE` -> HDMI render). During any echo the keyboard endpoint had ZERO
pending transfers, so the controller stopped polling it. This low-speed keyboard
behind the VL805's transaction translator (10ms interval) tolerates only a brief
unpolled window before it resets (lock LED out, input dead). The margin was
always thin -- the v0.4.188-191 changes shifted boot/runtime timing enough to
make ~the first keypress fail, but NO single commit was "the bug":
- v0.4.188 flusher: explicitly ruled out on HW (disabled-flusher kernel, build
  2027, still died).
- v0.4.190 getty / v0.4.189 KDF / v0.4.191 thread_server: not on the keypress
  path (adversarially reviewed + the failure reproduced with them unchanged).

## The fix (v0.4.192, src/usb/xhci.c)

1. **Multi-arm**: `INT_RING_BUFS`(8) interrupt-IN transfers always armed, each
   pointing at its own slice of the report DMA page (`RPT_STRIDE` 64). xHC
   completes them FIFO; `kbd_try_deliver` snapshots buffer `int_proc % 8`,
   re-arms that buffer BEFORE the blocking decode, then processes. The keyboard
   is continuously polled no matter how long the driver blocks.
2. **Typematic** (host-side repeat): boot-protocol keyboards report only state
   changes. New press arms repeat (latest wins); driver loop re-emits after
   500ms then every 66ms; the release report disarms. Polling mode only.
3. **Lock-key LED = software-only at runtime.** HW-PROVEN constraint (serial:
   `cc=6` STALL then `cc=-1` timeout, keyboard wedged): with the ring full, a
   runtime SET_REPORT through the TT stalls this keyboard. v0.4.186's
   endpoint-aware dispatch is NOT sufficient with a full ring. Num/Caps still
   gate numpad/case correctly; the physical LED stays at enumeration state
   (Num on). PROPER FIX (backlog): xHCI Stop Endpoint on the interrupt-IN,
   SET_REPORT, restart + re-arm.

Tests: `scripts/xhci_typematic_qemu_test.py` (new; run-of-17, stops on
release) + xhci_key/hub_key/led/proc/multidev/mouse all pass; smp 7/7.

## Hard-won process lessons (cost most of the session)

- **VERIFY EVERY DEPLOY.** Several "failed" HW tests were STALE KERNELS: the Pi
  ran build 2009 while we believed we'd swapped in 2012-2024 (the serial banner
  proved it). Flash-free kernel8.img procedure now: copy -> `shasum` ON THE
  CARD vs source -> `strings | grep build` ON THE CARD -> eject. The banner
  build number on boot is the ground truth. Version was bumped to 0.4.192
  mid-arc precisely to make the banner unambiguous.
- **One serial reader at a time.** Two background Python readers on
  /dev/cu.usbserial-0001 SPLIT the byte stream (each gets fragments); captures
  looked empty/garbled. Kill by `lsof -t /dev/cu...` before starting a reader.
- **Successful SET_REPORTs are silent** -- `[xhci-led]` lines print only on
  failure, so "no log" != "no transfer".
- Mac pty exhaustion from repeated `expect` spawns broke SSH ("no more ptys");
  netconsole (TCP, no pty) is the fallback, ONE connection at a time.
- AIOS SSH remains one-session-per-boot; netconsole survives but forks per
  command (gentle use only).

## State / leftovers

- **Committed**: v0.4.192 (this fix). The Pi runs build 2038 = this commit.
- **Stash** `v0.4.192 wip: eMMC-yield + re-arm-first + heartbeat`: contains the
  eMMC poll-loop yield (blk_emmc.c -- emmc_wait_int/_cmd/_dat seL4_Yield after
  1ms so a missed-bit 2s spin cannot monopolize core 0; v0.4.176-class
  hardening, QEMU-clean, rode along on HW builds 2024/2027 without issues) plus
  a superseded xhci.c. Worth extracting blk_emmc.c as v0.4.193 next flash
  cycle; drop the xhci.c part.
- **Backlog**: Stop-Endpoint LED fix; netconsole accept/fork-storm DoS;
  echo-burst smoothing (eMMC-yield should help); HDMI scroll-perf (HW panning);
  sweep item 1d (dependency pinning) -- the next major task before sharing the
  repo.
