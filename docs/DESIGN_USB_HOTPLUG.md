# DESIGN: USB runtime HOTPLUG (xHCI) -- external drive insert-after-boot

Status: Path A IMPLEMENTED + QEMU-verified (2026-06-16); Path B = DESIGN. Driver:
`src/usb/xhci.c`. Builds on the root-port hotswap (commit `07fa756`, v0.4.253), the
USB-MSC driver + >2TB (v0.4.255), and the boot-only `usb_msc_mount()`
(`src/boot/boot_fs_init.c`). Synthesized from a 5-lens read-only design workflow +
direct code grounding. Companion: `docs/NEXT_20260615h_usb_hotplug.md`,
`docs/NEXT_20260615e_*`.

## Path A -- AS BUILT (QEMU-verified, HW-pending)

`scripts/usb_msc_hotplug_qemu_test.py` 12/12 (insert-after-boot enumerate -> runtime
mount -> ls/cat/write/persist -> unplug teardown -> replug re-enumerate+re-mount); no
regression (boot-mount 6/6, basic 5/5, >2TB 4/4, keyboard hotswap PASS, net_socket
8/8); all four trees build. Key deltas from the plan below:

* **CRITICAL fix not in the original plan -- the driver thread must always run.** The
  xHCI driver thread (sole event-ring poller) spawned only `if (xhci_kbd_ok)`, so an
  empty-at-boot system never polled for hotplug (the device arrived -- PORTSC `ccs=1`,
  USBSTS PCD set -- but `evt_deq=0`, nothing draining the ring). Fixed: new
  `xhci_running` flag set at the end of `xhci_init`; `boot_services.c` spawns the thread
  whenever the controller is up. This is the linchpin of insert-after-boot.
* **Single-drive guard:** a 2nd simultaneous MSC drive is enumerated but NOT made the
  `/mnt/usb` backend (a `g_msc_dev` swap under the mounted `ext2_usb` would corrupt it).
* **Documented LIMITATION -- different-drive swap:** `blk_cache` drive 2 is not
  invalidated on unplug. Re-inserting the SAME drive is correct; swapping a DIFFERENT
  drive into the slot within one boot may serve stale cached sectors -> reboot between
  different drives. A safe invalidate must coordinate with the FS thread mid-fill (it
  parks inside `get_line` via `usb_blk_read`'s spin-wait, so a naive drop is a
  use-after-free) -- backlogged.
* **Adversarial review (4 lenses) outcome:** the "multi-core race" findings were FALSE
  POSITIVES -- all root threads are pinned to core 0 (`ROOT_CORE`) AND the FS thread
  spin-waits on the `g_msc_req` queue (never touches the event ring at runtime), so
  there is no concurrent ring access to race. Real fixes applied: `xhci_msc_ok = 0`
  init, `g_msc_mount_inline` clear-then-barrier, diag `g_msc_dev` snapshot.
* `/proc/xhci` now shows MSC state (`msc:` line) + `.auto.0|1` runtime automount toggle.
* **HW-verify items (next flash, serial capture):** SuperSpeed runtime hotplug on the
  real VL805 (the runtime path resets an already-trained port -- `port_reset` 500ms is
  proven for the SuperSpeed drive at boot); and the now-always-running poll thread's
  interaction with the TLBI stall when NO device is present.

---

## 1. Why hotplug is necessary (the firmware constraint)

The RPi4 bootloader tries to BOOT FROM any USB drive present at power-on; an ext2
USB drive makes it hang before `kernel8.img` loads (cold AND warm reboot, confirmed
HW this session). So a USB drive cannot be plugged at boot -> the only workable
model is insert-AFTER-boot, i.e. runtime hotplug. (Alternative unblock for a pure
mount test, no hotplug code: set the EEPROM `BOOT_ORDER` to SD-only via
`rpi-eeprom-config` on a Linux box.)

## 2. The key reframing -- this epic BIFURCATES by device speed

The 4TB Buffalo enumerated at boot as **slot 3, SuperSpeed** -- it came in on a
SuperSpeed ROOT port via `setup_device`->`setup_msc` (`xhci.c:1733`), NOT behind the
USB-2 hub. The LOW-SPEED keyboard is what sits behind the VL805 USB-2 hub
(`setup_hub`, `xhci.c:1242`). So "USB drive hotplug" is two different paths:

* **Path A -- SuperSpeed drive on a root port** (the real external-HDD case; USB-3
  drives land here). The EXISTING root-port hotswap already ENUMERATES it at
  runtime: a Port Status Change Event -> `g_port_change` (set in `evt_dispatch`,
  `xhci.c:215`) -> `handle_port_changes` (`xhci.c:1016`) -> `setup_device(p)` ->
  `setup_msc`. Missing pieces are small: **(A1)** call the mount at runtime, **(A2)**
  fix the driver-thread mount DEADLOCK, **(A3)** reclaim MSC DMA pages on teardown.
  **Fully QEMU-verifiable** (`qemu-xhci` SS root port + hot `device_add usb-storage`).

* **Path B -- USB-2 drive behind the VL805 hub** (USB-2 thumb drives; the prepped
  ext2 stick). A downstream change does NOT raise a root Port Status Change Event --
  the hub absorbs it and signals on its own interrupt-IN status pipe, which
  `setup_hub` never arms. Needs the hub-int pipe + downstream reconcile + the same
  A1/A2/A3. The interrupt path is **HW-only-verifiable** (QEMU modeling uncertain --
  see sec 8).

**Both share** the runtime-mount + deadlock + teardown-leak work, so Path A is the
correct first step: it is small, QEMU-verifiable, low-risk, and covers the actual
external-HDD use case. Path B is the larger HW-only follow-on.

### Already-in-place mechanics (no new infra needed)

* The driver thread (`xhci_kbd_driver_fn`, `xhci.c:1091`) **polls the event ring
  continuously** (`while (evt_dispatch(...) != DISP_NONE)` then `Yield`). Runtime
  port-change events AND (once armed) hub-int Transfer Events land on that ring and
  are picked up by the existing poll loop -- no MSI/IRQ work required. Default
  `xhci_irq_mode=0` (polling) stays.
* The single-consumer / reentrancy rule: `evt_dispatch` runs NESTED inside
  enumeration command waits (`cmd_submit`/`control_transfer`). Any enumerate/teardown
  MUST run at TOP LEVEL in the driver loop (the `g_port_change` pattern), never inside
  `evt_dispatch`. New handlers obey this.
* The MSC request queue (`g_msc_req` + `msc_service_request`, `xhci.c:1663`) already
  bridges FS-thread block I/O to the driver thread at runtime.

---

## 3. Path A -- runtime mount of a root-port SuperSpeed drive (QEMU-verifiable)

### A2 (do first) -- the mount DEADLOCK fix

At runtime the mount runs ON THE DRIVER THREAD. `usb_msc_mount` -> `ext2_init` ->
`blk_cache_read2` -> `usb_blk_read` (`xhci.c:1688`): with `g_msc_driver_running==1`
it posts to `g_msc_req` and spin-waits for the driver thread to service it -- but the
driver thread IS the caller -> DEADLOCK. (At boot it works: the boot thread runs the
mount before `g_msc_driver_running` is set, taking the DIRECT branch.)

Fix: a `volatile int g_msc_mount_inline` (near `g_msc_driver_running`). In
`usb_blk_read`/`usb_blk_write` change the gate to
`if (!g_msc_driver_running || g_msc_mount_inline)` -> direct transfer. Set the flag
(with `arch_dsb()`) around the runtime mount only. Safe because: the driver thread is
the sole consumer and is busy IN the mount (not in its service loop), and no FS-thread
I/O can arrive until `/mnt/usb` exists. During the direct transfer `bot_bulk` still
calls `evt_dispatch`, so keyboard reports interleave -- identical to the boot mount.

### A1 -- invoke the mount at runtime, inline, top-level

`setup_msc` (on success) sets a new `g_msc_mount_pending=1` ONLY if not already
mounted. The driver loop, at top level (after `handle_port_changes`), checks the flag
and calls a `msc_runtime_mount()` wrapper: set `g_msc_mount_inline=1; dsb;`
`usb_msc_mount(); dsb; g_msc_mount_inline=0`. Runs inline in the driver thread, so it
completes before any FS-thread activity; idempotent (a non-ext2 drive logs and stays a
block device only, as at boot). `g_msc_dev` is single-global = first MSC drive only
(documented; `setup_msc` keeps the existing "first device" behavior).

### A3 -- reclaim MSC DMA pages on teardown (latent leak, fix regardless)

`device_teardown` (`xhci.c:1000`) frees only the 6 HID pages; an MSC device leaks
`bo_ring`, `bi_ring`, `msc_buf`, `msc_io`. Add `dma_free()` for those (NULL-guarded;
`dma_free` no-ops NULL). On unplug the slot + all DMA pages reclaim so replug reuses
the slot. Also clear `g_msc_dev`/`xhci_msc_ok`/`g_msc_mount_pending` and
`vfs_umount("/mnt/usb")` (if a umount path exists; else leave the mount stale and
re-register on replug -- TBD, see open questions).

### A -- gating (default OFF, inert)

Even Path A re-arms nothing risky, but the runtime mount touches vfs from a new
context. Gate the runtime-mount call behind a runtime `/proc/xhci.automount` toggle
(default ON for Path A since it is QEMU-proven; revisit). The DEADLOCK fix and the
teardown-leak fix are unconditional (pure correctness).

### A -- QEMU test (`scripts/usb_msc_hotplug_qemu_test.py`, new)

Boot build-04 with `-device qemu-xhci` (no drive). After login, HMP
`device_add usb-storage,drive=...,bus=xhci.0` -> expect serial `USB MSC ready` +
`USB drive mounted at /mnt/usb`; over netconsole `ls /mnt/usb` + `cat` a known file.
HMP `device_del` -> `device unplugged ... torn down` (+ verify DMA pages reclaimed via
`/proc/xhci`/`/proc/vka`). `device_add` again -> re-enumerate + re-mount (slot reuse).

---

## 4. Path B -- USB-2 drive behind the VL805 hub (the hub epic, HW-only int path)

### B1 -- arm the hub interrupt-IN status pipe (`setup_hub`)

Mirror `setup_hid` (`xhci.c:864`) + `arm_int_buf` (`xhci.c:655`):
* Parse the hub config descriptor (fetch the FULL descriptor, not just the 9-byte
  header it reads now at `xhci.c:1249`) for the single interrupt-IN endpoint
  (`type==5`, `attr&0x3==3`, `addr&0x80`). Do NOT hardcode EP 0x81 -- compute
  `dci = (addr&0xF)*2+1` from the discovered address.
* Allocate `status_ring` + `status_buf` via `dma_page()` (reuse the HID `int_ring`/
  `rpt` fields -- a hub has no HID EP, so the fields are free; document the aliasing).
* Configure-Endpoint adds the int-IN EP context (interval exponent via the proven
  `xhci.c:929-944` logic; type 7, CErr=3, MPS, TR dequeue). The hub Hub-bit/nports
  Evaluate-Context (`xhci.c:1267`) already runs; fold the int EP into the same or a
  follow-on Configure-Endpoint.
* Arm ONE transfer (status-change bitmap is `ceil((nports+1)/8)` bytes, clamp nports
  to 31 as `xhci.c:1263` does; bit N = port N changed, bit 0 = hub-local).

### B2 -- recognize the hub-int Transfer Event (reentrancy-safe)

In `evt_dispatch` (`xhci.c:190`), the Transfer Event branch routes to
`kbd_try_deliver`, which EXCLUDES `USB_HUB` (`xhci.c:825`) -> a hub int event is
currently dropped. Add: before/after `kbd_try_deliver`, if `(slot,ep)` matches a
`USB_HUB` device's int DCI, SNAPSHOT the bitmap, set `g_hub_change=1`, return
`DISP_OTHER`. Never enumerate here (reentrancy).

### B3 -- `handle_hub_changes()` at top level + `hub_enumerate_port()`

Driver loop, after `handle_port_changes`: `if (g_hub_change) handle_hub_changes()`.
It clears the flag, finds the hub, reads the snapshotted bitmap, and for each changed
downstream port: `HUB_GET_STATUS` (0xA3), ACK via `CLEAR_FEATURE` (`C_PORT_CONNECTION`
=16, `C_PORT_RESET`=20), then reconcile:
* connected && `!dev_on_hub_port(hub,port)` -> `hub_enumerate_port(hub,port)`
* `!connected` && dev -> `device_teardown(dev)`
Then RE-ARM the hub int transfer (top level only). `hub_enumerate_port` factors the
per-port body of `setup_hub` (`xhci.c:1289-1319`: reset -> speed -> `dev_alloc` ->
`address_and_describe(d, port, hub_root_port, kspeed, hub->slot, port, ...)`) and --
crucially -- dispatches **HID AND MSC** (today `setup_hub` calls only `setup_hid`,
`xhci.c:1317`; add `setup_msc` so a USB-2 drive behind the hub enumerates AT ALL, even
at boot). For MSC, set `g_msc_mount_pending` (Path A handles the mount).

### B4 -- downstream teardown keying (the sentinel hazard)

Hub-downstream devices keep `root_port=0xFFFFFFFF` (the `dev_alloc` sentinel,
`xhci.c:463`), so `dev_on_root_port` never finds them. Add `parent_slot` +
`parent_port` to `struct usb_dev` (zeroed in `dev_alloc`, filled in
`hub_enumerate_port` from the `address_and_describe` args). New
`dev_on_hub_port(hub,port)` keys on `parent_slot==hub->slot && parent_port==port`.
On HUB unplug (root-port teardown of the hub itself), also tear down its children
(loop `g_devs` for `parent_slot==hub->slot`).

### B -- gating (default OFF, INERT -- mirror `set_leds_runtime`)

A wrong hub-int re-arm wedges USB exactly like the LED Stop-Endpoint resume did
(`xhci.c:698`). Gate ALL of Path B behind a compile guard `XHCI_HUB_HOTPLUG` AND a
runtime `/proc/xhci.hubplug.1` (default OFF). Boot hub enumeration stays always-on.
If a re-arm wedges on HW: the hub itself is not torn down and the root-port keyboard
path is independent, so recover by `echo 0 > /proc/xhci.hubplug` (or reboot); only
hub-port hotplug dies, the system stays alive.

---

## 5. New / changed symbols (summary)

`xhci.c`: `g_msc_mount_inline`, `g_msc_mount_pending`, `msc_runtime_mount()`;
`usb_blk_read/write` gate change; `device_teardown` MSC `dma_free`s. **[Path A]**
`g_hub_change`, `setup_hub_int()`, `arm_hub_int_buf()`, `handle_hub_changes()`,
`hub_enumerate_port()`, `dev_on_hub_port()`, `evt_dispatch` hub branch,
`struct usb_dev { parent_slot, parent_port }`, `setup_hub` int-arm + MSC dispatch.
**[Path B]** `boot_fs_init.c`: `usb_msc_mount` unchanged (reused by `msc_runtime_mount`).

## 6. Implementation order

1. **Path A** (one commit, QEMU-verified): A2 deadlock fix -> A3 teardown reclaim ->
   A1 runtime mount + `g_msc_mount_pending` -> `usb_msc_hotplug_qemu_test.py`. No flash
   needed to gain confidence; HW-verify on the next flash with a real USB-3 drive.
2. **Path B** (separate commit/arc): B1 hub-int arm -> B2 evt branch -> B3 reconcile +
   `hub_enumerate_port` (MSC dispatch) -> B4 teardown keying -> gating ->
   `usb_hub_hotplug_qemu_test.py`. HW-only int verification (serial capture).
3. **Pre-flash ADVERSARIAL gate (mandatory):** before ANY flash, run a multi-agent
   code review of the diff (reentrancy, deadlock, ring re-arm, cycle-bit, DCI, leak).
   HW iteration is expensive; QEMU-green != HW-proven (`feedback_qemu_cannot_model_loss`).

## 7. HW verification (serial capture, `aios_console.py monitor <dev> --mirror <f>`)

Success signatures: hub-int event -> `hub port N: device connected` -> `device:
slot=M` -> `USB MSC ready: ... sectors` -> `USB drive mounted at /mnt/usb` -> shell
`ls /mnt/usb` works. Failure: no event after insert = int not armed/re-armed;
no `USB MSC ready` = enum failed; keyboard stops typing after a hub event = int ring
wedged (recover via the runtime toggle). /proc/xhci does NOT show MSC -- serial only.

## 8. Open questions (verify empirically; do not trust either side blind)

* **Does QEMU `usb-hub` deliver the interrupt-IN status-change transfer?** Lenses
  disagree (L1/L2: probably HW-only; L5: yes, full path testable). RESOLVE by arming
  B1 and running `device_add usb-storage,bus=hub.0` on QEMU early -- if QEMU fires the
  hub Transfer Event, Path B is QEMU-developable; if not, Path B is HW-only. Path A
  does NOT depend on this.
* `vfs_umount` on unplug -- does a umount path exist? If not, decide: leave the mount
  stale + re-`ext2_init` the shared `ext2_usb` on replug, or add umount.
* LS/FS-behind-TT may need `PORTSC_PLC` watching in addition to the hub int (per
  `NEXT_20260615e:42`) -- separate HW-only sub-item if a USB-2 device misbehaves.
* Multi-drive (`g_msc_dev` single global) is out of scope -- first drive only.
