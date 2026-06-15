# NEXT (seed): USB-MSC + >2TB HW-PROVEN; mount is firmware-blocked -> USB HOTPLUG is the next epic

Self-contained seed for a fresh session. Repo `~/Desktop/github_repos/AIOS`, branch `main`.
Read `HANDOVER.md` (top) + `MEMORY.md` first; this is the companion to
`docs/NEXT_20260615g_tcp_fix_deploy.md` (the TCP-fix / console / USB-MSC-stages-1-4 arc).

## TL;DR

The USB Mass Storage (external-HDD) driver and its new **>2TB (READ_CAPACITY(16)/READ(16))**
support are **HW-PROVEN on a real 4TB Buffalo drive**. The only un-closed USB piece, the
mount+read/write HW test, is **blocked by RPi4 firmware** (an ext2 USB drive present at boot makes
the bootloader try to boot FROM it and hang) -- which makes **USB hotplug the clear, and now
necessary, next epic**. Everything is UNCOMMITTED (Bryan commits via GitHub Desktop).

## JOB 0 -- commit (Bryan runs GitHub Desktop; do NOT commit yourself)

Two commits, verified split. The full file map + draft messages are in
`docs/NEXT_20260615g_tcp_fix_deploy.md` and the HANDOVER. Delta from that map (this session grew
ARC-2):

* **ARC-1 = v0.4.254 (TCP fix + console + LED scaffolding).** HW-deployed. Unchanged from the
  NEXT_20260615g map: `version.h` (252->254), `net.h`, `net_stack.c`, `net_tcp.c`, `net_server.c`,
  `nettest.c`, `getty.c`, `netconsole.c`, `sntp.c`, `procfs.c`, `scripts/tcp_loss_qemu_test.py`,
  + the LED-only hunks of `xhci.c` (TRB_STOP_EP / set_leds_runtime / LED handler) + the
  boot-banner hunk of `aios_root.c`. Both session docs go here.
* **ARC-2 = v0.4.255 WIP (USB Mass Storage + >2TB).** QEMU-verified; driver+>2TB HW-proven.
  `blk_cache.h`, `blk_cache.c`, `boot_fs_init.c`, the USB-MSC hunks of `xhci.c` (now incl the
  >2TB `scsi_read_capacity_16` / `scsi_rw16` / `scsi_blk_rw` / 64-bit `g_msc_req.lba` + the
  WRITE-self-test buffer fix + the last-LBA(16) self-test), the `usb_msc_mount()` call hunk of
  `aios_root.c`, and the scripts `usb_msc_qemu_test.py`, `usb_msc_mount_qemu_test.py`,
  **`usb_msc_big_qemu_test.py`** (new this session).
* `xhci.c` + `aios_root.c` are MIXED -> stage by hunk; every hunk is wholly one arc. `version.h`
  stays 252->254 in ARC-1 (the >2TB work did NOT touch it; bump to 255 on the next flash if a
  v0.4.255 label is wanted).

## Current Pi state + deploy model

* Pi runs **v0.4.254 build 2464** (= the full uncommitted tree, incl >2TB) at 192.168.0.8. DHCP
  bounces `.8`<->`.250`<->`.197`; if dark, `arp -an | grep dc:a6:32:1c:2e:e1`. Rollback on the
  local disk: `disk/kernel8.img` = the cube v0.4.252 build 2385 (network-healthy; physical-restore
  safety net). The >2TB HW kernel: `disk/kernel8_v255_usbmsc.img` (build 2464).
* Deploy: develop+verify on QEMU; kernel -> `mkkernel8 --kernel build-rpi4-netd ... --output
  disk/kernel8_v255_usbmsc.img` then `pi_flash.py --host <ip> --kernel <that>` (do NOT overwrite
  the cube rollback). Disk apps -> `pi_filexfer push` + reboot.
* **netconsole/netd fragility (recurring):** a post-boot ~32s TLBI stall can wedge netd (root
  alive, net dead); netconsole wedges under back-to-back connections (~50s between, ONE held
  conn). Drive gently. If the net is dead but serial shows the heartbeat, the system is alive --
  power-cycle to recover. [[project_stall_hunt]], [[project_netconsole]].

## PRIORITY 1 -- USB hub-port HOTPLUG (the next epic; now necessary, not optional)

**Why necessary:** AIOS enumerates+mounts USB MSC only AT BOOT. But an ext2 USB drive present at
boot makes the RPi4 bootloader try to boot FROM it and HANG before kernel8.img loads (cold AND
warm reboot -- confirmed this session; recover by removing the drive). So you cannot leave a USB
drive plugged at boot -> the only workable model is **insert-after-boot**, i.e. hotplug. (The 4TB
data drive (NTFS) booted past via a warm reboot; an ext2 drive does not.)

**The work:** the drive sits behind the **VL805 hub**, so this is HUB-port hotplug, NOT root-port.
* The root-port hotswap (keyboard, commit `07fa756`) does NOT cover hub-downstream devices.
* Design (captured in `docs/NEXT_20260615e` + the `usb-next-phase-design` workflow output): arm
  the **hub's status-change interrupt-IN endpoint**; on a port-status-change event, reset+address
  the new device, run `setup_device` (which already dispatches HID/MSC), and for an MSC device run
  the `usb_msc_mount()` path at RUNTIME (the request-queue concurrency is already built:
  `g_msc_req` + the driver thread service the FS-thread, [[project_usb_msc]]).
* **HW-only verifiable** (QEMU's hub model will not exercise the brcmstb/VL805 path). Develop the
  enumeration/dispatch on QEMU where possible, but the real gate is the Pi + serial capture.
* Runtime mount caveat: `usb_msc_mount` currently runs on the BOOT thread (sole event-ring
  consumer); a runtime mount must go through the driver-thread request queue (already exists) or
  carefully serialize against it.

**Alternative quick unblock (if you just want the mount HW test):** set the RPi4 EEPROM
`BOOT_ORDER` to SD-only via `rpi-eeprom-config` (on a Linux/RPi-OS box) so the firmware ignores
USB at boot -> then a drive can be present at boot and AIOS enumerates+mounts it. A built +
QEMU-verified AIOS-builder ext2 image is ready at `/tmp/usbstick.img` (`usb_stick_qemu_check.py`
5/5 -- mounts, reads `aios_hwtest.txt`, write round-trips). `dd` it to the WHOLE device (raw, no
partition table -- AIOS reads ext2 at LBA0). Note AIOS ext2 wants 1KB blocks + minimal features:
`mke2fs -b 1024 -t ext2 -O ^resize_inode,^dir_index,^ext_attr,^filetype /dev/sdX`.

## Other queued priorities (unchanged)

2. **Keyboard lock LEDs** -- needs a SERIAL-CAPTURE HW session. `set_leds_runtime()` in `xhci.c` is
   `#if 0`d; the corrected resume (doorbell-resume from the HW dequeue + drain the Stop event, NO
   ring reset) is in the `#if 0` block. Each wrong ring-resume wedges the keyboard -> power-cycle.
   [[project_usb_hid]], `docs/NEXT_20260615g` Task 4.
3. **USB-MSC Stage 5/6** -- stall/reset recovery (RESET_EP + SET_TR_DEQ on CC_STALL) + multi-sector
   read_multi (perf). Software, QEMU-dev. Lower priority than hotplug.
4. **V3D textured console** -- `docs/NEXT_20260615d` (the cube is done; this is the next GPU epic).
5. **usb_blk_read/write still hardcodes a 512-byte copy** -- fine for 512e drives (the 4TB Buffalo
   is 512); a native-4K drive would need it generalized. Backlog.

## Key artifacts this session

* `src/usb/xhci.c` -- the >2TB code (scsi_read_capacity_16, scsi_rw16, scsi_blk_rw, 64-bit lba,
  last-LBA(16) self-test, WRITE-self-test buffer fix).
* `scripts/usb_msc_big_qemu_test.py` -- the >2TB QEMU gate (sparse 2.36TB image), 4/4.
* `disk/kernel8_v255_usbmsc.img` -- build 2464 (>2TB) HW kernel; `disk/kernel8.img` = cube rollback.
* `/tmp/usbstick.img` (+ `/tmp/usb_stick_qemu_check.py`) -- a ready, QEMU-verified ext2 stick image.

## Lessons (this session)

* QEMU-green != HW-proven, but the HW gate is worth it: the 4TB drive surfaced the
  READ_CAPACITY(10) 2TB saturation that drove the whole >2TB feature -- only a real drive showed it.
* Serial (`aios_console.py monitor --mirror`) is the right HW-debug tool (the mirror file is
  readable from the host); netconsole is too fragile for iteration. /proc/xhci does NOT show MSC.
* The RPi4 firmware will try to boot from a USB drive -- a real constraint on any
  drive-at-boot design. [[project_usb_msc]] has the full finding.
