# NEXT (seed): USB hotplug epic (Path A + Path B) HW-VERIFIED on the real Pi

Self-contained handoff. Repo `~/Desktop/github_repos/AIOS`, branch `main`. Read `HANDOVER.md`
(top) + `MEMORY.md` first. Companion design doc: `docs/DESIGN_USB_HOTPLUG.md`.

## TL;DR

The USB runtime-hotplug epic is **DONE and HW-VERIFIED on real silicon** (RPi4, build 2503):
Path A (root-port SuperSpeed drives) AND Path B (devices behind the VL805 hub) both work,
INCLUDING the keyboard-on-the-same-hub coexistence the review flagged HIGH. Five commits are
committed AND pushed (origin/main = `0f0f468`).

## Commits (all pushed)

```
0f0f468 usb: hub-downstream hotplug -- arm the VL805 hub status pipe (Path B, v0.4.256, ships inert)
cc597d7 blk_cache+ext2: invalidate USB drive caches on hot-swap (v0.4.256)
a6fcdd4 usb: runtime hotplug mount of a USB drive inserted after boot (Path A, v0.4.255)
fb69bff usb: Bulk-Only-Transport/SCSI mass-storage driver + >2TB support (v0.4.255 WIP)
a249dda net: fix TCP RST-on-close regression + console cleanup (v0.4.254)
```

`version.h` is still **254** (not bumped). The Pi runs **build 2503** (= the production
build-rpi4-netd kernel with all five commits), flashed over the network via
`pi_flash.py --host 192.168.0.8 --kernel disk/kernel8_v256_usbhotplug.img` (3-way sha, verified).
`disk/kernel8.img` is untouched (= the cube v0.4.252 rollback).

## HW-VERIFIED this session (real RPi4, serial capture)

* **Path A (root-port SS drive hotplug):** unplug -> `device unplugged: slot=N ... torn down`;
  replug -> re-enumerate, SLOT REUSED, `INQUIRY`, `USB MSC ready: 7814037168 sectors` (4TB),
  `last-LBA(16) @7814037167: OK 45 46 49 20` ("EFI ", >2TB READ(16)), runtime-mount path ran +
  correctly DECLINED the GPT drive. Verified with TWO drives (4TB Buffalo + Sony 15GB).
* **The single-drive guard (a review fix) on HW:** `second USB MSC device ignored (single-drive;
  first stays mounted)` -- the 4TB stayed the block backend when the Sony was added.
* **Path B (hub-downstream hotplug) -- FULLY VERIFIED via /proc/xhci.hub.1:**
  `hub status pipe armed: ep=0x81 dci=3 mps=1 bitmap=1 bytes speed=3` (real VL805 is HS -- the
  speed-aware interval review-fix matters here) -> `HUB INT event ... bitmap0=0x08` (the REAL
  VL805 delivers the interrupt-IN status transfer, as QEMU predicted) -> `hub port 3 change:
  removed` -> `device unplugged: slot=2 port=4294967295 -- torn down` (keyboard on the hub torn
  down via dev_on_hub_port) -> replug on port 4 -> `hub port 4 change: connected` -> `device:
  slot=2 speed=2 ... HID keyboard ... ready` (re-enumerated, slot reused).
* **KEYBOARD COEXISTENCE -- the review's HIGH risk -- RETIRED.** The keyboard rides the same
  hub; it kept typing while the hub pipe was armed + the reconcile ran CLEAR_FEATURE on its
  port, AND it was hot-replugged behind the hub and came back working. The VL805-TT-wedge did
  NOT materialize on real HW.
* The inert default behaves correctly: with Path B OFF (default), a device behind the hub is
  invisible at runtime (boot scan still enumerates it). `cat /proc/xhci.hub.1` arms it live;
  `.hub.0` disables the reconcile.

## Known rough edges the HW surfaced (follow-ups, in priority order)

1. **USB-MSC Stage 5 stall recovery** (the clear next CODE task). A SuperSpeed drive's FIRST
   replug attempt stalled on the bulk EP: `MSC data cc=6` (STALL) + `CSW/CBW cc=-1` ->
   `READ_CAPACITY failed`; a second attempt (new slot) succeeded. Needs RESET_EP + SET_TR_DEQ on
   `cc=6` so the first replug is clean. Concrete HW repro now. (xhci.c bot_scsi/bot_bulk.)
2. **Path B default-ON decision.** Its key HW risk (keyboard coexistence) is now proven safe,
   so `g_hub_hotplug` could flip 0->1 (default ON) -- a small commit + re-run the QEMU suite
   (usb_hub_hotplug 11/11, no-regress) + a reflash. Or keep the explicit /proc toggle one more
   cycle. Judgment call.
3. **version.h bump to 0.4.256** for the milestone label (the kernel currently reports 0.4.254
   build 2503; the work is labeled v255/v256 in comments). 1-liner + rebuild; Bryan commits.
4. **Stall + HDMI fragility under USB churn.** USB hotplug activity aggravates the ~32s TLBI
   stall (saw 65s once, 263-472ms other times -- variable, always recovered). The HDMI console
   also wedged once mid-session (the known cacheable-FB scroll-freeze / display_server wedge,
   NOT a Path A/B/cache regression -- a reboot recovered it cleanly). The always-polling USB
   driver thread is a suspected stall aggravator; revisit. [[project_stall_hunt]]

## Test harnesses (QEMU; all green)

`usb_msc_hotplug_qemu_test.py` 12/12, `usb_msc_swap_qemu_test.py` 11/11 (proven discriminating),
`usb_hub_hotplug_qemu_test.py` 11/11 (enable via /proc/xhci.hub.1), `usb_msc_mount` 6/6,
`usb_msc` 5/5, `usb_msc_big` 4/4. No-regress: `xhci_hotswap` PASS, `net_socket` 8/8, `smp` 7/7
(drive-0 cache stress); `xhci_hub_key` decode is a PRE-EXISTING QEMU sendkey env flake (fails on
clean tree too; the keyboard ENUMERATES). All four trees build.

## Lessons

* QEMU's `usb-hub` DOES model the interrupt-IN status pipe incl runtime changes -- a probe
  proved it, making Path B QEMU-developable instead of HW-blind. The real VL805 then matched.
* Drive USB-hotplug HW tests from SERIAL, not netconsole: the kernel `[xhci]` enumeration logs
  go to the UART, and netconsole degrades under the USB-activity-triggered TLBI stalls.
* On the RPi4, USB-3 (blue) ports = SuperSpeed root ports (Path A); USB-2 (black) ports route
  through the VL805 hub (Path B). The keyboard is always behind the hub.
* With Path B OFF you cannot hot-replug ANYTHING behind the hub, including the keyboard (it goes
  dead until reboot) -- that is the inert default working, and why Path B exists.
