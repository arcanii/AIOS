# NEXT (seed): USB-MSC bulk-STALL recovery (Stage 5) + USB-churn fragility -- v0.4.257

Self-contained handoff. Repo `~/Desktop/github_repos/AIOS`, branch `main`. Companion to
`docs/NEXT_20260616_usb_hotplug_hw.md` (the USB-hotplug epic that shipped just before this).
Read `HANDOVER.md` (top) + `MEMORY.md` first.

## TL;DR

Items 1 (USB-MSC Stage 5 stall recovery) and 4 (stall/HDMI fragility under USB churn) from
the prior seed are DONE, **QEMU-verified, AWAITING HW**. A bulk STALL (cc=6) no longer wedges
the pipe: the driver clears the halt on BOTH the xHC (Reset Endpoint + Set TR Dequeue) and the
device (ClearFeature ENDPOINT_HALT), then reads the CSW. Version bumped 256 -> **0.4.257**.
Nothing flashed to the Pi yet.

## What changed (`src/usb/xhci.c`, all in the BOT/SCSI mass-storage section)

1. **`bot_ep_recover(d, dir_in)`** (new) -- clears a Halted bulk endpoint, mirroring the
   HW-proven `ep0_recover`: Reset Endpoint (Halted->Stopped) -> ClearFeature(ENDPOINT_HALT) on
   EP0 (device-side clear) -> Set TR Dequeue to the live ring cursor with the current cycle bit.
   Bounded: if the Reset Endpoint command times out the controller is wedged -> skip the rest
   (don't compound ~3s of busy-wait). ClearFeature + Set TR Dequeue results are logged loudly.
2. **`bot_scsi`** now recovers on all three BOT phases: a CBW STALL clears bulk-OUT + fails (the
   caller's retry re-issues on a clean pipe); a **data-phase STALL clears the halt and STILL
   reads the CSW** (BOT 5.3.x -- the old code returned early, leaving the EP Halted, so the CSW
   + next CBW read cc=-1: the exact failed-replug wedge this fixes); a CSW STALL clears bulk-IN
   + retries the CSW once.
3. **`setup_msc`** retries `READ_CAPACITY` (3x) so a STALLed first access (the SuperSpeed
   first-replug repro) enumerates cleanly on the now-cleared pipe instead of failing.
4. **`/proc/xhci.stalltest.N`** -- fault-injection poke (QEMU never STALLs a bulk EP). Arms N
   faked data-phase STALLs raised AFTER the real transfer (ring stays consistent), so the
   recovery path is reachable + regression-tested on QEMU. Default 0 = inert; zero overhead and
   byte-identical behavior on the happy path.

## Item 4 (USB-churn fragility) -- resolved as investigation + synergy, NOT a poll-loop change

The seed hypothesized the always-polling USB driver thread aggravates the ~32s TLBI stall.
Investigated against `project_stall_hunt` + an adversarial review; conclusion:

* **DO NOT touch the poll loop.** Its busy `seL4_Yield()` spin is the SCU-clock *cure* (the
  nodes=4 mechanism): a nap/sleep would let the A72 SCU quiesce and REINTRODUCE the 32s stall,
  and risks starving the LS keyboard behind the VL805 TT. USB issues no TLBIs, so it can't
  directly cause the teardown-TLBI stall; it's at most an indirect frequency amplifier
  (enumeration burst rate, already clamped to 32ms).
* **The real, fixable churn jank was item 1's territory:** a Halted endpoint made `bot_bulk`
  busy-spin the full 2000ms per transfer on cc=-1, several times per failed replug. The
  recovery removes those episodes; `bot_ep_recover` is self-bounded.
* The residual heavy-spawn-storm stall is the SMP remote-TLBI shootdown to idle cores 1-3
  (deferred, correctness-sensitive kernel change -- see `project_stall_hunt`). The HDMI wedge
  is the separate display_server cacheable-FB scroll-freeze, out of scope here.

## HW verification Bryan should run (real RPi4, drive from SERIAL not netconsole)

1. Build + flash-over-network the production kernel (build-rpi4-netd):
   `python3 scripts/pi_flash.py --host <ip>` (3-way sha; preflight `fatswap /nonexistent` -> -4).
2. **The real repro:** hot-replug a SuperSpeed drive on a USB-3 (blue) port. Pre-fix the FIRST
   replug logged `MSC data cc=6` (STALL) + `CSW/CBW cc=-1` -> `READ_CAPACITY failed`, then a 2nd
   slot succeeded. Now the first replug should re-enumerate cleanly. Watch for the new serial
   lines `MSC ... STALL -- clearing halt ...` (recovery fired) and a clean `USB MSC ready:`.
3. **Fault-injection sanity (any drive):** `cat /proc/xhci.stalltest.1`, then read a file off
   `/mnt/usb` -> serial shows `MSC data STALL INJECTED (test)` + the recovery line + the read
   still returns correct data. (Inert by default; the poke is the only way to arm it.)
4. Confirm the keyboard-on-the-hub keeps working through MSC churn (no new wedge).

## Follow-ups (deferred -- do NOT implement blind; needs an HW-iteration session)

* **Full BOT reset recovery (spec 5.3.2 / 6.7.2 / 6.8):** Bulk-Only Mass Storage Reset (class
  request bmRequestType=0x21, bRequest=0xFF, wIndex=interface) on a 2nd consecutive CSW STALL or
  a Phase Error; clear BOTH endpoints on a CBW STALL. Today the TUR / READ_CAPACITY retry loops
  mitigate this; full handling is a bigger, device-state-sensitive change.
* The nested deadline busy-waits in `cmd_submit`/`control_transfer`/`bot_bulk` do not yield --
  by design: adding a yield there hung the board once (eMMC data-phase yield, reverted). Leave.

## Gates (QEMU, all green; build 2513)

New `usb_msc_stall_recovery_qemu_test.py` **9/9** (exercises the recovery path via the poke) ·
`usb_msc` 5/5 · `usb_msc_mount` 6/6 · `usb_msc_hotplug` 12/12 · `usb_msc_swap` 11/11 ·
`usb_msc_big` 4/4 · `usb_hub_hotplug` 11/11 · `xhci_hotswap` PASS (sendkey decode is the known
pre-existing QEMU env flake). All four build trees compile (build-04, build-rpi4-netd, build-rpi4).
Adversarial review (4 lenses + per-finding verify): core correctness REFUTED-as-bugs (ring/cycle
state, command ordering, ClearFeature params, DCI->addr, fault-injection consistency all
confirmed correct); 3 minor fixes applied (RMW snapshot on the inject counter; loud logging on
ClearFeature + Set TR Dequeue).
