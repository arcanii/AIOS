# NEXT (session result + deploy): TCP regression FIXED + console cleanup + LED scaffolding

Companion to `docs/NEXT_20260615f` (the brief). This is the RESULT + the HW deploy/verify
plan. Repo `~/Desktop/github_repos/AIOS`, branch `main`. Version bumped 253 -> **0.4.254**.
UNCOMMITTED (Bryan commits/pushes).

## TL;DR

The v0.4.253 TCP regression is ROOT-CAUSED, FIXED, and verified on QEMU *under injected
loss* (the test the lossless suite could never run). Console cleanup (boot banner + netconsole
banner + getty banner) done. Keyboard-LED Stop-Endpoint scaffolding added opt-in/inert (HW
iteration needed). Nothing is on the Pi yet -- JOB 0 (physical rollback) still gates everything.

## ROOT CAUSE (multi-lens diagnosis + code-verified + empirically reproduced)

Two coupled defects in commit `3e3e26a`, both invisible to lossless+instant QEMU SLIRP:

1. **Trigger** -- `net_server.c` froze a FIN_WAIT socket only on `(ACK) && !(FIN)`. A real
   peer routinely sends a COALESCED `FIN+ACK` on close; that branch SKIPPED it, so the socket
   never freed, lingered to the 10s `TCP_CLOSE_MAX_MS` deadline, and `net_tcp_rto_check` gave
   up and **sent a TCP_RST**. A RST makes the peer DISCARD already-received-but-unread data =
   the SSH 0/20 + netconsole "Connection reset by peer". (The `!(FIN)` guard predates 3e3e26a,
   but 3e3e26a's new give-up turned a benign slot-leak into an active RST.)
2. **Cascade** -- the give-up freed the slot (`in_use=0`) but left `rtx_due_ms/rtx_count/
   closing/fin_sent/close_deadline_ms` STALE. The SYN-on-LISTEN child-allocation path did NOT
   reset them (only the `NET_SOCKET` active-connect path did). A new connection reusing a
   poisoned slot hit `net_tcp_rto_check`'s give-up in `SYN_RCVD` before reaching ESTAB ->
   RST during the handshake = "worked once, then every connection RSTs".

## THE FIX (net_server.c) -- 3 surgical changes, deferred-close design kept

- **(A)** FIN_WAIT free now triggers on `fin_sent && snd_una>=snd_nxt` (drop the `!(flags &
  TCP_FIN)` guard) -> a coalesced FIN+ACK frees the socket promptly (no linger -> no give-up).
- **(B)** the SYN-on-LISTEN child allocation calls `net_sock_tx_init(conn)` -> a reused slot
  never carries stale rto/close state into SYN_RCVD (kills the cascade).
- **(C)** the give-up NO LONGER sends a RST -- it frees the slot silently and calls
  `net_sock_tx_init` to clear the rto/close fields. A RST is what discarded the client's data;
  a dead peer times out by itself, a slow peer's later segments hit the no-socket drop. This is
  the only `TCP_RST`-on-a-connected-socket send in the tree, and it is gone.

The data-retransmit core (snd_una, txbuf, NET_SENDTO buffering, the rto data/FIN retransmit)
was sound and is UNCHANGED.

## NEW: lossy-QEMU test infrastructure (the thing that was missing)

QEMU SLIRP is lossless+instant, so the loss/retransmit/give-up/RST paths never ran. Added
deterministic loss injection that runs entirely on QEMU (see `feedback_qemu_cannot_model_loss`):

- `/proc/netstat.txdrop.N`  -- drop the next N OUTBOUND DATA segments (exercises retransmit).
- `/proc/netstat.findrop.N` -- drop the next N OUTBOUND FINs (forces the give-up path while
  data still flows, so the guest does not hang).
- `/proc/netstat.ackdrop.N` -- drop every Nth inbound pure-ACK.
- All three SPARE the netconsole control channel (port 2323) so the harness stays reliable.
- New `/proc/netstat` counters: `tcp_rtx_segs`, `tcp_rst_sent`, `tcp_giveups`, `tcp_tx_drops`,
  `tcp_ack_drops` -- legible from netconsole on the real Pi too (the only window into the loss
  path QEMU cannot model).
- `nettest closelinger <ip> <port>` -- connect+echo+close then sleep ~14s so the server-side
  socket LINGERS (no op-98 process-exit reap), mirroring a long-lived server; the give-up then
  fires during the sleep.
- New harness `scripts/tcp_loss_qemu_test.py` (boots build-04, drives over netconsole).

## VERIFICATION (QEMU) -- all GREEN on the shipping v0.4.254

- **`tcp_loss_qemu_test.py` 6/6** AND the airtight before/after: with the RST temporarily
  re-introduced, R2b FAILed (`tcp_rst_sent=1`, give-up fired); with the fix, R2b PASSes
  (`tcp_rst_sent=0`, give-up STILL fires giveups 0->1). Same path exercised, only the RST
  differs -- the loss test distinguishes broken from fixed.
- **No-regression:** `net_socket_qemu_test` 8/8 on build-04 (flag-OFF) AND build-netd
  (flag-ON, Pi config); `ssh_qemu_test` 6/6; `netd_qemu_test` 10/10.
- All four trees build clean (build-04 / build-netd / build-rpi4 / build-rpi4-netd).

**Still HW-pending (the FINAL gate):** the lesson of this saga is QEMU-green != HW-proven. The
loss repro is faithful but synthetic. After deploy, run a real short-session loss-rate check
(`ssh root@<pi> 'echo hi'` x40, count empty output; expect ~0) and watch `/proc/netstat`
`tcp_rst_sent` stay 0 + `tcp_giveups` low under normal use.

## DEPLOY PLAN (in order)

1. **JOB 0 -- physical rollback FIRST (Bryan).** Restores the network via the known-good cube
   kernel (v0.4.252). `disk/kernel8.img` (sha `b3aa70d`) + `/tmp/card_kernel8_cube_backup.img`.
   Mount the SD on the Mac, `cp disk/kernel8.img /Volumes/AIOSBOOT/kernel8.img`, verify sha,
   eject, reinsert, power-cycle. Network back (cube has no TCP regression).
2. **Deploy the v0.4.254 fix OVER THE NETWORK** (now that the cube net works -- the deploy rides
   the cube's good TCP, exactly like the netd Stage-3 flash-over-network):
   ```
   python3 scripts/mkkernel8.py \
     --kernel build-rpi4-netd/images/aios_root-image-arm-bcm2711 \
     --output disk/kernel8_v254.img
   python3 scripts/pi_flash.py --host <pi-ip> --kernel disk/kernel8_v254.img   # reboots
   ```
   DO NOT overwrite `disk/kernel8.img` (keep it as the rollback). Verify `/proc/version` shows
   0.4.254 build 2441 + sha-on-card after.
3. **Push the disk apps** (getty + netconsole run from DISK -- the kernel flash does NOT carry
   their fix; getty is spawned via exec_thread from the filesystem):
   ```
   python3 scripts/pi_filexfer.py push build-04/sbase/netconsole /bin/netconsole <pi-ip>
   python3 scripts/pi_filexfer.py push build-rpi4-netd/projects/aios/getty /bin/getty <pi-ip>
   ```
   (confirm the on-disk getty path) then `reboot` over netconsole to pick them up. Drive
   netconsole GENTLY (~50s rest between connections).
4. **Verify HDMI is clean** (login prompt + shell only; boot/[dtb]/[fs]/[net]/[gpu] now on
   serial, getty/netconsole banners gone) and the short-session loss-rate check above.

## CONSOLE CLEANUP (Task 2 + 3) -- what shipped + a finding

- **Boot banner (Task 2 part 1):** `aios_root.c` boot status (`[boot]/[dtb]/[fs]/[net]/[gpu]`)
  now uses `printf` (root debug UART = serial) instead of `fb_console_printf` -> OFF the HDMI.
  Verified architecturally: `fb_console_putc` is called ONLY by display_server (the tty mirror)
  + fb_console.c; root printf cannot reach it. KERNEL change (ships with the v254 flash).
- **netconsole banner (Task 2 part 3):** removed the `"[netcon] v2 listening..."` startup printf
  that raced getty's login prompt on the shared HDMI mirror. DISK app (push). Mind
  `netconsole-redirect-fd-bug` -- fd1 routing untouched (just stopped printing).
- **getty banner (Task 3):** removed the stale `AIOS_VERSION_FULL` banner (getty.c) baked into
  the on-disk binary. DISK app (push).
- **FINDING re Task 2 part 2 ([INF] messages):** the brief expected `[INF]` on HDMI. Traced it:
  root `AIOS_LOG`/`printf` go to the kernel debug UART (SERIAL), NOT fb_console (root does not
  call `aios_init_caps`, and the boot-banner author used `fb_console_printf` explicitly because
  root printf does not reach HDMI). So root `[INF]` is ALREADY serial-only -- where Bryan wants
  it. The only process chatter on the HDMI mirror was netconsole (fixed) + a one-line sntp
  "time set" (left -- informational, not in the screenshot). If the real Pi still shows `[INF]`
  on HDMI after deploy, capture the actual photo -- there is a path not evident in the code.

## TASK 4 (keyboard lock LEDs) -- HW-TESTED, ring-resume WRONG, disabled (needs serial-capture iteration)

**HW RESULT 2026-06-15 (v0.4.254 on the real Pi, via `cat /proc/xhci.led.0`):** the LED HALF
WORKS -- the poke turned the lock LEDs OFF, so the Stop-Endpoint + SET_REPORT on EP0 is fine.
BUT the interrupt-ring RESUME is WRONG: immediately after, the keyboard emitted a stuck
repeated key ('r') and then went DEAD. So the re-prime re-delivers a STALE report and/or the
`int_enq/int_proc/int_cycle` re-sync does not match the HW dequeue after the Stop. A reboot
recovered (the risk stayed contained to the explicit poke -- normal typing was never affected).
`set_leds_runtime` is now DISABLED (`#if 0`, safe no-op) -- this needs a real-HW iteration loop
with serial capture (each wrong re-arm wedges the keyboard), not blind edits. Fix direction
(in the `#if 0` block): read the EP-context TR Dequeue Pointer the HW wrote AFTER the Stop and
resume from there (do NOT force ring start); zero the report buffers before re-priming so no
stale 'r' is re-delivered; drain the Stop-Endpoint Transfer/Command events first.

### Original scaffolding notes (superseded by the HW result above)

The lock keys still toggle SOFTWARE state only (default unchanged). A runtime `SET_REPORT`
STALLs (cc=6) on the LS keyboard behind the VL805 TT while the interrupt-IN ring is armed
(HW-PROVEN v0.4.192), wedging the device -- the documented "needs Stop-Endpoint, backlogged".

Added (xhci.c): `#define TRB_STOP_EP 15` + `set_leds_runtime()` = Stop Endpoint -> SET_REPORT on
EP0 -> re-establish the interrupt ring (reset `int_enq/int_proc/int_cycle`, Set TR Dequeue to
ring start, re-prime `INT_RING_BUFS`) -> restart. Wired ONLY to the explicit `/proc/xhci.led`
poke (NEVER a keypress), so testing it can wedge at worst the explicit diagnostic (reboot
recovers) and NEVER normal typing. Ships INERT.

**HW test protocol (real keyboard + serial capture, AFTER the network is back -- do NOT bundle
with the critical TCP flash):**
1. `cat /proc/xhci.led.7` (all LEDs on) / `.led.0` (off) and WATCH the physical LED.
2. Read `/proc/xhci` -> `g_led_last_cc` (expect 1=SUCCESS, not 6=STALL) + USBSTS (HSE/HCE 0).
3. Confirm the keyboard STILL TYPES after the poke (the ring re-sync worked).
4. **If the LED sets + the keyboard keeps typing:** wire `set_leds_runtime` into the lock-key
   handler in `process_kbd_report` (the three `kc==0x53/0x39/0x47` cases) -- it runs on the
   driver thread, so it can call it directly. Then Caps/Num light the LED.
5. **If the keyboard goes quiet after the poke:** the int_enq/int_proc/cycle re-sync vs the HW
   dequeue is wrong -- read the EP context TR Dequeue Pointer the HW wrote after Stop, rather
   than forcing ring start; and drain the Stop-Endpoint Transfer/Command events. See the
   tuning notes in the `set_leds_runtime` comment.

## USB MASS STORAGE (external HDD) -- Stages 1-2 DONE + QEMU-VERIFIED (v0.4.255 WIP)

A USB Mass Storage (Bulk-Only Transport + SCSI) driver over xHCI, in src/usb/xhci.c.
UNCOMMITTED. QEMU-tested via `scripts/usb_msc_qemu_test.py` (5/5): boots build-04 with
`-device qemu-xhci -device usb-storage,drive=<raw img>`.

DONE (QEMU-verified, all four trees compile):
- **Stage 1** -- enumerate class-8/Bulk-Only (0x50): `setup_msc()` finds the two BULK
  endpoints, configures both EP contexts in one Configure-Endpoint (EP type 2=Bulk OUT,
  6=Bulk IN), `bot_bulk()` (NORMAL TRB + doorbell + event-ring wait, Link-wrap at 255),
  `bot_scsi()` (CBW "USBC" -> data -> CSW "USBS", tag+status validated), INQUIRY +
  READ_CAPACITY. setup_device dispatches MSC after HID fails (device class is 0; the
  class is at the interface). HW result QEMU: `131072 sectors x 512 = 64 MB`, INQUIRY
  `QEMU HARDDISK`.
- **Stage 2** -- `scsi_read_10()` / `scsi_write_10()`. READ-ONLY LBA0 self-test at
  enumeration (safe on any drive); WRITE(10) self-test GATED to the QEMU disk
  (`msc_qemu`, destructive on LBA1). Verified: READ returns the planted LBA0 pattern,
  WRITE persists (offline image byte-match).

- **Stage 4 DONE + QEMU-VERIFIED -- the drive MOUNTS at /mnt/usb and reads+writes files.**
  `scripts/usb_msc_mount_qemu_test.py` (attaches an ext2 image as usb-storage): the drive
  enumerates, boot mounts it (`[vfs] Mounted /mnt/usb`), and over netconsole `ls /mnt/usb`
  + `cat /mnt/usb/etc/passwd` read correctly AND `echo > /mnt/usb/x` round-trips + persists
  to the on-disk image. Pieces: (a) blk_cache extended to drive 2 (arrays [2]->[3],
  blk_cache_read2/write2, write-THROUGH), (b) the RUNTIME-CONCURRENCY request queue --
  the xHCI event ring is single-consumer, so usb_blk_read/write (FS thread) post {write,lba,
  status,done} to g_msc_req + spin-wait (seL4_Yield); the driver loop (g_msc_driver_running)
  runs msc_service_request via the same scsi_read_10/write_10, keyboard reports interleave
  in the bulk wait. During the MOUNT (boot thread, pre-driver-thread) usb_blk_* transfer
  DIRECTLY (sole consumer). (c) usb_msc_mount() in boot_fs_init.c, called from aios_root.c
  AFTER xhci_init + BEFORE boot_start_services spawns the driver thread: ext2_init(&ext2_usb,
  blk_cache_read2, 2) + vfs_mount("/mnt/usb", ...). One 512-byte sector per request; assumes
  512-byte blocks.
- **REMAINING: Stage 5** stall/reset recovery (RESET_EP + SET_TR_DEQ on CC_STALL, cap
  retries) for a flaky real drive; **Stage 6** multi-sector read_multi (perf). Then **HW**
  with a real USB drive (brcmstb PCIe path vetted by the keyboard; BOT/SCSI is HW-independent;
  the runtime request queue + real-drive timing + 512-vs-4K block size need HW confirmation).

## Also queued (unchanged from f): keyboard LED corrected resume (design captured above,
needs a serial-capture HW session), xHCI VL805-downstream-hub hotswap (design done, see
the workflow output + NEXT_20260615e), V3D textured console (NEXT_20260615d).
