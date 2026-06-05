# AIOS HANDOVER -- HISTORY (older arcs)

Prior-session records (v0.4.110 -> v0.4.168) moved out of HANDOVER.md to keep it
focused on the current state. Full per-session detail is in `docs/NEXT_*.md` and
the memory index (`MEMORY.md`). Newest first.

---

## Where we left off (v0.4.167 -> v0.4.168) -- RPi4 HDMI DISPLAY WORKING

The RPi4 HDMI display now lights up under AIOS via the VC mailbox (Phase B), HW-verified:
the monitor shows the AIOS gradient + `AIOS 0.4.168` banner + `[boot] ...` fb_console text,
replacing the firmware rainbow. UNCOMMITTED at handoff. See `rpi4-hdmi-phaseb-mailbox` memory.

* **Root cause (HW-verified)** -- the v0.4.166 "VC mailbox call failed" was NOT (only) the bus
  alias the prior memory suspected. v0.4.167 added `dma_pa | 0xC0000000` + diagnostics; the boot
  serial then showed `[gpu] tag buf: arm_pa=0xfbe3c000` + `bad response 0x0`: the DMA tag buffer
  (plain `vka_alloc_frame`) landed at ~3.9GB, so `| 0xC0000000` was a no-op and the VideoCore
  could not read it. The legacy VC mailbox property buffer MUST be in the low 1GB (the bus alias
  only spans ARM-phys 0-0x3FFFFFFF). genet's mailbox works only because it allocates early/low.
* **Fix (v0.4.168)** -- allocate the tag buffer at a fixed low address,
  `sel4platsupport_alloc_frame_at(0x3A000000)` (base of the GPU reserved region, a RAM-backed
  device untyped < 1GB). Then `0x3A000000 | 0xC0000000 = 0xFA000000` is a valid VC bus alias ->
  the mailbox returns `FB up`, `map_fb_pages` succeeds, fb_console renders. `src/plat/rpi4/display_vc.c`.
* **Diagnostics kept** -- the mailbox OUTCOMES now go through `AIOS_LOG_*` so `cat /proc/log |
  grep gpu` shows PASS/FAIL over netconsole (no lossy serial). v0.4.167 also (re)confirmed
  netconsole needs the TCP connection HELD OPEN (`printf | nc -w` returns empty; use a python
  socket that recv-loops until idle). Display is OUTPUT-only; interactive HDMI needs USB HID.

**Next:** committed (`38d1f6c`). See the newer `v0.4.169 -> v0.4.171` entry above for the
colour/3D/deploy/speed arc that followed.

## Where we left off (v0.4.166) -- FAST PULL, WALL-CLOCK/SNTP, HDMI re-enable

Committed `b0c5c04`. time/SNTP/__get HW-verified on the real RPi4; HDMI re-enabled boot-safely
but not yet lit. Record + next: **`docs/NEXT_20260604c.md`**.

* **netconsole `__get` (fast pull)** -- netconsole reads the file itself and streams it
  length-framed, bypassing `dash -c cat` (fork + pipe relay + cat's small-chunk writes). HW:
  ~370 KB/s vs the old ~128 B/s; `pi_filexfer.py pull` uses it. File transfer (push+pull) is
  now fast both ways.
* **Wall-clock time + SNTP** -- the RPi4 has no RTC, so time was uptime (~1970). Now: a
  wall-clock epoch offset in the root task (PIPE_GET/SET_TIME), libc adds it for
  gettimeofday/CLOCK_REALTIME, `settimeofday` sets it. `src/apps/sntp.c` queries a public NTP
  server over UDP (net stack routes off-subnet via the gateway -- no DNS); getty auto-runs it
  at boot. HW: `date` shows the real 2026 date. See `aios-wall-clock-time` memory.
* **HDMI re-enable** (RPi4) -- removed the v0.4.150 gate, go straight to the VC mailbox
  (Phase B). **Boot-safe (HW-confirmed)**: graceful -1 on failure, no assert. BUT the mailbox
  alloc fails on the Pi (Phase B was never HW-tested; old HDMI used Phase A diag-stub).
  Prime fix lead: the VideoCore BUS address for the DMA tag buffer (try `| 0xC0000000`). See
  `rpi4-hdmi-phaseb-mailbox` memory. Debug next session.
* **Bluetooth/HCI plan** -- `docs/DESIGN_BLUETOOTH_HCI.md` (PL011/BT UART is free; needs a
  proprietary firmware blob + a stack; low priority).

**Next:** debug the HDMI VC mailbox (bus-address lead); optionally write ext2 file mtimes via
`aios_wall_now()`; flash-over-network (FAT write); scp/sftp (lost mbedTLS). See NEXT_20260604c.

## Where we left off (v0.4.165) -- FILE TRANSFER + REDIRECT FIX

Built on the v0.4.164 network-control base; all HW-verified on the real RPi4. Committed
`cfead22`. Record + next: **`docs/NEXT_20260604c.md`**. Three things:

* **File push + pull over netconsole** (`scripts/pi_filexfer.py push|pull`, no crypto/reflash).
  Length-framed + sha256-verified. PULL = `wc -c` + `sha256sum` + `cat` read-exactly-N-bytes;
  PUSH = netconsole intercepts `__put <path> <len>` then reads N raw bytes into the file. HW:
  5120-byte binary pushed integrity-OK, /proc + configs pulled. PULL of larger DISK files is
  slow (sbase `cat` small-chunk fs reads) -- a netconsole `__get` would fix that.
* **`2>&1` redirect fix** -- shell fd-dup redirects hung over netconsole because `close(fd 2)`
  reset the SHARED `stdout_pipe_id` (AIOS routes fd 1+2 via one global), killing fd 1 routing
  so output went to serial, not the pipe. close(2) now leaves it alone. See
  `netconsole-redirect-fd-bug` memory. (libaios_posix change -> dash rebuilt.)
* **Faster pipe SHM** -- the shared xfer page is now cacheable on BOTH ends (v0.4.164 made it
  non-cacheable: correct but slow). Coherent on the single-core PIPT A72, HW-verified. The
  v0.4.164 bug was the cacheability MISMATCH, not cacheable-ness.

**Next:** a netconsole `__get` (fast pull); flash-over-network (needs FAT-boot-partition write);
scp/sftp proper (needs the lost mbedTLS rebuilt). See `docs/NEXT_20260604c.md`.

## Where we left off (v0.4.163 -> v0.4.164) -- NETWORK CONTROL ACHIEVED

**Drive the Pi over the LAN, reboot it over the network, auto-recover -- no serial after
flashing.** All HW-verified on the real RPi4. Full record + next steps:
**`docs/NEXT_20260604c.md`**. Committed `616aa10`. Headlines:

* **netconsole v2** (`src/apps/netconsole.c`) -- command-per-connection plaintext TCP shell
  (port 2323, LAN/dev only, no auth). Reads a socket line, runs `dash -c "<line>"` (runs +
  EXITS -> clean stdout EOF), streams output back, loops. Replaces the v1 `dash -i` relay that
  never flowed I/O on HW (no tty/line discipline). O_NONBLOCK poll + per-command timeout/SIGKILL.
* **net_server accept backlog** -- `NET_ACCEPT` drains an already-ESTABLISHED child before
  blocking; fixes the fast-reconnect race that capped netconsole AND sshd at one session.
* **reboot** (BCM2711 watchdog) -- PM block 0xFE100000 mapped ascending; `aios_system_reboot()`
  arms PM_WDOG/PM_RSTC; `reboot` + `shutdown -r` via the PIPE_SHUTDOWN reboot flag.
* **auto-start** -- getty forks/execs netconsole once at startup (a running, post-settle
  process). Spawning it from boot_services aborted the root server (boot-critical-path allocator
  reclamation race) -- do NOT retry from the boot path.
* **pipe SHM cache fix (the HW-only blocker)** -- the PIPE_MAP_SHM xfer frame was mapped
  cacheable in pipe_server but non-cacheable in the client; on the Cortex-A72 the server's write
  stayed in D-cache while the client read stale zeros -> netconsole output came back all-NUL.
  Mapped both ends non-cacheable. **QEMU does not model cache attributes -- a 9/9 QEMU pass did
  NOT catch it; only real hardware did.** See `pipe-shm-cache-coherency` memory.

**Next: SCP/SFTP** (file transfer over the network) -- the last roadmap item. SSH is blocked on
rebuilding the lost mbedTLS; a base64-over-netconsole or a plaintext file-transfer port unblocks
it without that yak-shave. See `docs/NEXT_20260604c.md`.

## Where we left off (v0.4.153 -> v0.4.162) -- GENET NETWORKING COMPLETE

**Full bidirectional networking on real RPi4 hardware.** GENET went from "DMA inert
both ways" to a cold-boot DHCP lease (192.168.0.8) + bidirectional ping (0% loss,
~4.5ms), IRQ-driven RX, real board MAC (dc:a6:32:1c:2e:e1). All HW-verified. Full
record + the network-control roadmap: **`docs/NEXT_20260604b.md`**. Headlines:

* **DMA datapath (v0.4.154)** -- rewrote the register map vs U-Boot bcmgenet:
  descriptors in the register block (RX 0x2000 / TX 0x4000), correct ring/ctrl bases
  (0x3000/0x5000, 0x3040/0x5040), word-unit ring addrs, SCB_BURST, RING_CFG, DESC_OWN,
  TX QTAG+CRC, RX prod/cons de-swap. The old map put descriptors past the 64KB window
  and control regs onto descriptor 0, so DMA never ran. TX up.
* **RGMII (v0.4.155)** -- EXT_RGMII_OOB_CTRL config. RX up (RXp climbs to 78 on HW).
* **/proc/genet live probe (v0.4.156)** -- dump/peek/poke/mdio/tx/reinit/ip/irq/mac.
  The tool that let us bisect the rest with live register pokes, not a flash per attempt.
* **DHCP (v0.4.157/158)** -- xid reuse across retransmits (RFC 2131; a real-LAN OFFER was
  xid-rejected, QEMU SLIRP masked it) + real board MAC via VC firmware mailbox + .ip
  one-line counters. Real-LAN lease 192.168.0.8.
* **IRQ-driven RX (v0.4.159/160/162)** -- behind a live toggle, ring-full deadlock fixed
  (NAPI re-check), then defaulted on. Cold-boot verified.
* **UMAC_MAC1 packing (v0.4.161)** -- 16-bit field, bytes 4,5 in the LOW half; the old
  high-half write left the unicast RX filter wrong -> ARP/DHCP (broadcast) worked but
  ping (unicast) was dropped. Fixed -> ping both ways.

**Next: network control** (run longer, flash less -- see `docs/NEXT_20260604b.md`). SSH
server EXISTS (src/ssh/, port 2222) -- confirm it over GENET + auto-start it = Claude
controls the Pi over the network, no serial. Reboot and scp/sftp are NOT implemented
(add a BCM2711 watchdog reset; add an exec-channel / sftp subsystem).

## Where we left off (v0.4.150 -> v0.4.153)

Networking session: **RPi4 GENET MAC layer brought up on real hardware**, a **DHCP
client** shipped (QEMU-proven), the serial tool fixed -- and the GENET DMA datapath
pinned down as the remaining blocker. All code COMMITTED. Full detail + next-session
plan: **`docs/NEXT_20260604a.md`**. Headlines:

* **GENET full MAC bring-up, HW-VERIFIED (v0.4.151, `143e1ce`).** The v0.4.150 "UMAC
  fault" was the SYS_RBUF_FLUSH_CTRL.SWINIT reset latch, not clock-gating. Clear it
  before any UMAC access -> UMAC reachable, PHY BCM54213 link up 100Mbps FD, boots to
  login. See `genet-umac-swinit` memory.
* **DHCP client (v0.4.152, `e292e88`), QEMU-proven.** `src/net/net_dhcp.c` --
  DISCOVER/OFFER/REQUEST/ACK at net_server startup, applies the lease in place, falls
  back to static on a ~4s timeout. Leases 10.0.2.15 from QEMU SLIRP (`aios_console.py
  qemu --net user`), then ARP+ping the gateway. See `dhcp-client` memory.
* **Tooling (`8da5683`).** `aios_console.py` configure_serial() now falls back to
  `stty` (the termios path EINVAL'd on this macOS host and killed the driver); added
  `qemu --net user` (the fast QEMU net harness).
* **GENET DMA = THE NEXT BLOCKER (v0.4.153, `42b2e31`, WIP).** MAC+PHY work but the
  descriptor DMA engines are INERT both ways: HW diag shows TXp climbing (frames
  queued) while TXc stays 0 (TX DMA never sends) and RXp stays 0. v0.4.153 fixed the
  RX prod-init (16 -> 0) and switched to polling RX + datapath diagnostics. Next: port
  the bcmgenet DMA-enable (DMA_SCB_BURST_SIZE, DMA_RING_CFG, word-unit ring addrs).
  HARDWARE-GATED -- a flash per attempt. Plan in `docs/NEXT_20260604a.md`.

## Where we left off (v0.4.144 -> v0.4.150)

A very large session -- file-backed mmap, a block-read race fix, COW Step 3, and a
**HW-verified RPi4 device-MMIO fix**. Full detail + next steps: **`docs/NEXT_20260603c.md`**.
Several pieces are verified but **UNCOMMITTED** (push from GitHub Desktop). Headlines:

* **File-backed mmap (v0.4.144-146, COMMITTED `a60a12e`).** Explored A/C/B; kept
  demand-paged B. `mmap(MAP_SHARED/MAP_PRIVATE, fd, off)` + `msync` write-back;
  both fault handlers fill pages via `vfs_pread`. `test_mmap` 9/9 on QEMU.
* **Block-read hardening (v0.4.147, COMMITTED `c784577`).** virtio-blk completion
  poll captured `used->idx` AFTER `QUEUE_NOTIFY` -> raced fast cached completions
  under host load -> spurious `-1` on uncached reads -> spurious exec EPERM. Fixed
  (capture before notify). **This commit BROKE build-rpi4** (procfs referenced a
  qemu-only symbol); the fix is uncommitted in `blk_cache.c` -- commit it soon.
* **COW Step 3 RESOLVED (v0.4.148, uncommitted).** The "post-promotion EPERM" was
  the block-read race, NOT a COW bug -- `do_fork` always succeeded; it reproduced
  with the gate OFF. `COW_STRIP_PARENT=1` now works (0 EPERM, parent_promotions 2,
  strip_errs 0). The NEXT_20260503a hypothesis was wrong.
* **RPi4 device-MMIO blocker FIXED + HW-VERIFIED (v0.4.149/150, uncommitted).**
  GENET + display were disabled in v0.4.98 for one shared root cause: seL4 device
  untypeds are forward-only, so peripheral MMIO must be claimed ASCENDING by paddr
  (GENET 0xFD58 + VC mailbox 0xFE00B sit below the first-mapped GPIO/UART/eMMC).
  Fix: `prealloc_rpi4_devices()` (new `boot_device_map.c`) maps all peripheral MMIO
  low->high. On real RPi4: `[devmap]` maps all 5 ascending, GENET responds **rev
  v6.0**, eMMC + ext2 work. GENET full bring-up (UMAC clock-gating) still faults ->
  made probe-only in v0.4.150 to keep the Pi bootable; display deferred.
  GENET/display bring-up = next, HW-gated.

**Uncommitted, ready to commit:** `cow.c` (COW Step 3); `blk_cache.c` + `blk_virtio.c`
(RPi4 build fix); `boot_device_map.c` + `device_map.h` + `aios_root.c` + `net_genet.c`
+ `blk_emmc.c` + `display_vc.c` + `CMakeLists.txt` + `version.h` (RPi4 device-map).
**Immediate: verify the v0.4.150 checkpoint boots on the Pi (`disk/sdcard-rpi4.img`
ready @ 23:36), then commit.**

## Where we left off (v0.4.140 -> v0.4.143)

The headline: the long-standing **`cat /etc/passwd | wc -l` = 0** bug (and
`ls | wc -l`) is FIXED, verified on QEMU and on the **real RPi4 hardware**. Full
record + seed: `docs/NEXT_20260603b.md`.

* **v0.4.140** (prior session): `aios_bss_res[]` fixed a real use-after-free of
  `bss_reservation`. Kept -- but it never affected the cat|wc symptom.
* **v0.4.141 -> v0.4.143** (this session): the cat|wc bug was NOT demand-BSS (the
  earlier docs mis-attributed it). It was a pipe writer/reader **EOF-ordering
  race**: the creating shell (and the sibling reader, and the writer's own
  redundant write-fd close) latched the pipe `write_closed` BEFORE the slow
  fs-reading writer registered -> the reader got a premature EOF. **v0.4.143**
  latches `write_closed` in exactly one place -- the registered writer's exit
  (`handle_child_fault`, helper `pipe_live_writer_exists`); no close latches.
  Builtins register via dup2/PIPE_SET_PIPES so they latch on exit too.
  Timing-independent. Verified deterministic on QEMU --smp 4 and on the RPi4
  single-core (cat|wc=2, ls|wc=104, seq 2000|wc=2000, multi-stage, early-exit).
  v0.4.142 silenced the benign "Range ... not reserved" COW-in-BSS-range noise.
* **Tooling** (committed, no version bump): `scripts/aios_console.py` now has a live
  `watch` subcommand -- colored on-screen transcript of a session while the driver
  runs headless (mirrors to `.aios_console.live`, gitignored). Run `watch` in your
  own terminal.
* **Item 1 "harden pipes under load" -- investigated, deferred.** It is a resource
  -ceiling problem (MAX_ACTIVE_PROCS=16, VKA 8000, morecore 6MB -> ~16 procs) whose
  failures (EPERM-on-exec, "Cannot fork", dropped pipe writes, boot storms) are
  largely artifacts of artificial multi-QEMU host contention; single-instance and
  real hardware work reliably. A v0.4.144 client-side pipe-write data-loss fix was
  tried and REVERTED (busy-yield added pressure / deadlocked late readers). See
  `docs/NEXT_20260603b.md` and BACKLOG.md "Next up". Recommended next: item 2
  (file-backed mmap).

---

## Where we left off (v0.4.126 -> v0.4.139)

Two arcs landed in this batch: POSIX VM/FS syscall fill-in (replacing
silent stubs with real IPC) and RPi4 hardware-test prep (FAT32, flash
script, SMP build enable, test plan). User has an RPi4 in hand; first
hardware boot is the next milestone.

* **v0.4.126**: real `mprotect` via new `PIPE_MPROTECT` (label 85).
  Server walks caller's vspace, calls `seL4_ARM_Page_Map` per page with
  new rights. Same kernel mechanism the v0.4.123 COW investigation
  proved safe. `src/apps/test_mprotect.c` covers the round trip.
* **v0.4.127**: mprotect extended -- `PROT_NONE` via per-page Unmap,
  `PROT_EXEC` clears the XN bit. test_mprotect now exercises both.
* **v0.4.128**: real `munmap` via `PIPE_MUNMAP_ANON`. Previously a
  no-op; now actually frees frames. Verified by re-mmap after munmap.
* **v0.4.129**: `/proc/cmdline` summarises the boot environment.
* **v0.4.130**: real `ftruncate` via new `FS_TRUNCATE` IPC.
* **v0.4.131**: `/proc/cmdline` platform-aware (rpi4 vs qemu-virt) +
  `BACKLOG.md` for deferred items so HANDOVER's pending table stays
  tight.
* **v0.4.132**: `scripts/mksdcard.py` swapped mtools `mformat` for
  macOS `newfs_msdos` (run against the image as a hdiutil `-nomount`
  vnode), then patches BPB `hidden_sectors=2048` by hand. RPi4 firmware
  now reads our FAT32 partition cleanly -- the documented Method 2
  manual workaround is no longer the only path. Linux dev hosts fall
  back to mformat.
* **v0.4.133**: `scripts/flash-rpi4.sh` -- one-shot SD card flasher.
  Wraps `mksdcard.py` + `dd` with safety checks (refuses
  /dev/disk0..2, refuses Device Location: Internal, refuses partition
  slices, requires literal `YES`). Tested all five refusal paths;
  hardware happy path waits on a real card.
* **v0.4.134**: `KernelMaxNumNodes` 1 -> 4 in `settings-rpi4.cmake`.
  Build clean, image grew ~50 KiB. Elfloader spin-table driver was
  already wired upstream. qemu raspi4b is silent for our SD image
  (firmware/UART quirks unrelated to SMP); validation happens on real
  hardware.
* **v0.4.135**: RPi4 SMP fallback. The first real-hardware boot found
  that v0.4.134 (SMP=4) HANGS at the firmware-to-kernel handoff -- the
  elfloader spin-table secondary-core bring-up never returns. Reverted
  `KernelMaxNumNodes` 4 -> 1 in `settings-rpi4.cmake`; single-core boots
  cleanly to login on hardware. Full record: `docs/NEXT_20260602a.md`.

* **v0.4.136**: `flash-rpi4.sh` accepts the macOS built-in SDXC reader
  (auto-allows removable Secure Digital, or `--allow-internal`) and
  hash-gates the flash to catch silent non-flashes.
* **v0.4.137**: `scripts/aios_console.py` -- stdlib (no-pty) driver to
  drive AIOS over a qemu unix socket or the RPi4 serial. Logs in, runs
  commands, captures output. The session's diagnostic workhorse.
* **v0.4.138**: `pipe_maybe_free` releases the VKA audit count on pipe
  free -- `/proc/vka` `live` is now accurate. This was the real cure for
  the RPi4 pipe wedge (removed a false-memory-pressure spiral).
* **v0.4.139**: pipe_server reclaims orphaned pipes at slot exhaustion
  (a `had_child`-guarded safety net). Wedge fix VERIFIED on hardware:
  580 pipes / 3 boots / 0 wedge / `live` flat. See
  `docs/NEXT_20260602a.md` sections 3.1-3.2.

Also landed two docs (no version bump):
* **`hw/rpi4/HARDWARE_TEST.md`**: phase-by-phase first-boot checklist.
  Physical setup, expected serial output at each stage, functional
  checklist, diagnosis playbook, version fallback ladder.
* **`docs/SEL4_DEVInvestigation.md`**: seed for a session that wants
  to look at the seL4 side -- current snapshot, the `seL4_Debug*`
  ABI we underuse, five concrete diagnostic gaps from this session's
  work, ground rules for kernel patches behind a single
  `CONFIG_AIOS_KDEBUG` gate, ordered investigation list (A-E).

### State of the RPi4 hardware test (BOOT + PIPE FIX DONE, 2026-06-02)

First real-hardware boot done AND the pipe bug fixed this session. Full
record: `docs/NEXT_20260602a.md`. Headlines:

* **Boots + works on HW** (v0.4.139, single-core): login, eMMC/ext2, all
  servers, `/proc/hw`, `/proc/cmdline`, file redirect, `test_mprotect`,
  and now **shell pipes** (580 pipes / 3 boots / 0 wedge, `live` flat).
* **Pipe wedge FIXED** (v0.4.138/139). The cure was v0.4.138's accurate
  `/proc/vka` `live` count (it removed a false-memory-pressure spiral:
  refused BSS maps -> failed forks -> leaked pipes -> wedge). v0.4.139
  adds a slot-reclaim safety net (dormant -- nothing leaked in 580).
* **SMP still off.** v0.4.134 (SMP=4) hangs at the firmware-to-kernel
  handoff (elfloader spin-table). v0.4.135 fell back to single-core.
  Re-enabling SMP is the main open RPi4 item.
* **Benign leftover**: the demand-BSS reservation race (`Range ... not
  reserved`) still fires (~4/pipe) but is now harmless log noise.
* **Tooling**: drive AIOS over serial/qemu with `scripts/aios_console.py`
  (no pty). Flash with **balenaEtcher** + hash-gate `kernel8.img` (the
  built-in SDXC reader trips `flash-rpi4.sh`'s internal guard; v0.4.136
  relaxed it).

## Where we left off (v0.4.121 -> v0.4.125)

This batch finished COW Phase 2 Steps 1-3 from `docs/NEXT_20260502b.md`
plus a new server health probe. Steps 1 and 2 are live. Step 3
(parent-side stripping) has all the plumbing landed but is gated off
because of an unresolved post-promotion regression -- see "COW Phase 2
Step 3 status" below and `docs/NEXT_20260502c.md`.

* **v0.4.121**: COW Phase 2 Step 1 (WnR fault detection) + server
  health probes. cow_handle_write_fault now reads FSR bit 6 and
  returns 0 on read faults so they fall through to the real
  exit path (a read fault inside a COW range is an instruction-fetch
  bug, not a COW promotion). New in-process probe thread pings
  pipe/fs/thread/net/disp/crypto every 5s with `SVC_PING` (label 5);
  exposed via `/proc/serverstats`. Probe runs at priority 200 (same
  as the servers it pings) -- 180 was permanently starved.
* **v0.4.122**: COW Phase 2 Step 2 (per-frame refcount). 64-entry
  table in cow.c keyed by paddr; cow_frame_acquire wired into
  cow_setup_segment, cow_frame_release into cow_release_proc and the
  promotion path. Pure observation -- no behaviour change.
  `/proc/cow` exposes the table. Smoke confirms BSS-fault count
  unchanged from v0.4.121 baseline (kept the table to 64 entries
  specifically to avoid the 1024-entry BSS-shift trap from the
  abandoned attempt).
* **v0.4.123**: kernel investigation + Step 3 probe. Read seL4 ARM
  `performPageInvocationMap` to confirm Page_Map remap-in-place is
  safe and only touches the cap slot, the PTE, and the TLB. Probe
  (gated behind COW_PROBE_PARENT_STRIP) verified the kernel
  mechanism in production: parent strip succeeds, parent dies on
  next write to a stripped page, getty respawns. Probe code stays
  in cow.c gated off; full investigation in
  `docs/NEXT_20260502c.md`.
* **v0.4.124**: Step 3 plumbing landed gated OFF. cow_setup_segment
  takes parent_idx; cow_handle_write_fault has the parent-promotion
  branch (kernel Page_Map of fresh frame, stash in
  cow_promoted_global, set cow_disabled to flip future forks of
  this proc to eager copy). cow_release_proc skips parent ranges
  in the vspace_unmap walk. Per-proc Step 3 state lives in cow.c
  globals (NOT active_proc_t) -- first attempt put it inline and the
  resulting BSS shift broke dash startup, repeating the v0.4.122
  lesson.
* **v0.4.125**: cow_current_cap helper + fork.c plumbing. Threads
  parent_idx through fork_copy_into_existing / region / stack so
  the eager-copy fallback after a parent's promotion sources from
  cow_promoted_global instead of the orphaned parent_cap. Step 3
  mechanism now proven end-to-end: smoke shows
  `parent_promotions: 2, strip_errs: 0`, no kernel errors. But
  with COW_STRIP_PARENT=1 dash post-promotion sees EPERM on
  subsequent fork+exec (`wc`, `shutdown` fail). With the gate at 0
  the system is v0.4.122-equivalent.

### COW Phase 2 Step 3 status

Mechanism: working. Production-on: not yet. The wc/shutdown bug
manifests only when COW_STRIP_PARENT=1, after a parent has
promoted at least one page. It's behind a default-off flag and
loses no existing capability (eager-copy fork path is unchanged
when strip is 0). Next session can flip the flag, repro the EPERM
on second fork+exec, and trace through `do_fork`'s 12 -1 paths
to find which one fires post-promotion. Likely candidates: cap
allocation interacting with the orphaned parent_cap, or a child
cspace cap that ends up wrong. See `docs/NEXT_20260503a.md`.

## Where we left off (v0.4.110 -> v0.4.119)

Three intertwined arcs landed in this batch: copy-on-write fork,
block-layer caching with reclaim, and a stack-overflow bug that had
been masquerading as a BSS-init mystery for several sessions.

* **v0.4.110**: COW fork infrastructure (Phase 1 WIP) -- cow.c +
  fault handler hooks, gated off pending the reservation-tracking
  interaction.
* **v0.4.111**: COW enabled for the file-backed (data) portion of
  writable LOAD segments. Reservations created fresh after
  sel4utils_elf_load frees its own. cow_release_proc explicitly
  unmaps before destroy.
* **v0.4.112**: Block cache + `/proc/cachestats`. ext2 sector I/O
  routes through 4 KB cache lines (LRU, hash table, vka-backed).
  Two backends (drive 0 = system disk, drive 1 = log).
* **v0.4.113**: Cache reclaim under memory pressure --
  vka_audit_check_headroom calls blk_cache_evict before failing.
  Process pages always beat cache pages.
* **v0.4.114**: Boot warmup prefetch + `/proc/filehits` profiler.
  17 files (~123 KB) preloaded before login; per-path access counter
  hooked into vfs_read.
* **v0.4.115**: File-redirect inheritance across exec.
  `aios_sys_execve` packs stdout/stderr file paths into the
  PIPE_EXEC IPC; `__wrap_main` re-opens and seeds stdout_redir_*.
  Fixes `ls > /tmp/o` producing an empty file.
* **v0.4.116**: Warmup file list tuned from real `/proc/filehits`
  data after a representative interactive session.
* **v0.4.117**: TCC self-host with libc -- pre-linked libaios_tcc.o
  (~613 KB) wraps the AIOS runtime + libc subset, packaged as a
  one-member `/usr/lib/libc.a`. tcc on AIOS now compiles + links
  programs that #include <stdio.h>, call printf/malloc/exit.
* **v0.4.118**: Stack-overflow fix that we had been working around
  with FILEHITS_MAX/PATH_MAX tuning since v0.4.114. Symptom looked
  like a BSS-init bug; root cause was a stack overflow corrupting
  probe_info. Real fix landed; the FILEHITS comment is now just
  the simple "80 chars is plenty" line.
* **v0.4.119**: cow_release_proc page-by-page unmap (was bulk).
  Reduces but does NOT eliminate the residual "BSS fault: map
  failed / Range for vaddr X not reserved" log noise after
  fork+exec -- the v0.4.119 commit message overstated this.
  Pure baseline still emits ~10 such errors per smoke session.
* **v0.4.120**: Log rotation now keeps a backup generation. Was
  unlink+recreate (data loss); now reads current `aios.log` into a
  lazily-malloc'd 1 MB heap buffer, recreates `aios.log.1` from it,
  then drops `aios.log` and recreates fresh. Buffer is heap (not
  static BSS) to avoid shifting the root task's BSS layout.

### COW Phase 2 attempt (between v0.4.119 and v0.4.120, abandoned)

Spent a session attempting Phase 2 (parent-side stripping +
refcount table + stack COW per `docs/DESIGN_COW_FORK.md`). All
of the work was reverted. Three concrete blockers found and
documented in `docs/NEXT_20260502b.md`:

1. **Parent stripping** via `seL4_ARM_Page_Map(parent_cap,
   parent_pd, va, R/O)` triggered "Invocation of invalid cap" /
   cap fault in the forked child. Mechanism unclear -- possibly
   needs unmap-then-remap, or interacts with the shared frame's
   CDT.
2. **Stack COW** reservation collided with child's IPC buffer
   when probe extended into low memory.
3. **Refcount table size** (1024 entries) shifted BSS layout
   enough to amplify the v0.4.119 baseline noise.

The wholesale rewrite was too coupled to debug. The NEXT doc
proposes a 5-step incremental restart. Step 1 (WnR fault
detection) is ~10 LOC and risk-free; do that first if resuming.

Cumulative this batch: COW Phase 1 fully functional, disk-cache
hit rates ~84% in normal sessions, file redirection works
end-to-end, on-AIOS tcc can build hello-world style programs
against libc, log rotation keeps one historical generation.

---

## What is pending

Design docs:

* `docs/DESIGN_DEMAND_BSS.md` -- implemented in v0.4.106-107.
* `docs/DESIGN_COW_FORK.md` -- Phase 1 + Phase 2 Steps 1-2 live;
  Step 3 plumbed but gated off (see `docs/NEXT_20260503a.md`).
  Steps 4-5 (stack COW, parent-dies safety) not started.

### Active tactical items

| Item | LOC | Risk | What ships |
|---|---|---|---|
| **RPi4 pipe bug** | done | n/a | FIXED v0.4.138/139, verified on HW (580 pipes / 3 boots / 0 wedge). Cure was v0.4.138 accurate `live`; v0.4.139 reclaim is a dormant safety net. Underlying demand-BSS reservation race (bug 1) still fires but is now benign log noise -- see `docs/NEXT_20260602a.md` for a future cleanup. |
| **RPi4 SMP bring-up** | ? | high | v0.4.135 fell back to single-core because the elfloader spin-table secondary-core bring-up hangs on real HW. To re-enable: make the elfloader print on the RPi4 UART, then per-core boot trace (item E, `docs/SEL4_DEVInvestigation.md`). |

Everything else (COW Step 3 fix, block cache write-back, file-backed
mmap, COW Steps 4+5, server-probe auto-restart, swap, smoke-driver
polish, fault-observation harness) is in [BACKLOG.md](BACKLOG.md).

---

## Architecture: virtual memory (the part we just rebuilt)

### Per-process layout

Each spawned process has its own seL4 VSpace via
`sel4utils_configure_process_custom`. ELF segments are loaded by
`sel4utils_elf_load`. The main writable region is a 6 MB
`morecore_area` BSS that musl uses for malloc.

### Demand-paged BSS (v0.4.106-107)

After `sel4utils_elf_load`, exec_server / pipe_server:

1. Parse program headers, find largest LOAD segment with
   `memsz > filesz`. That is the BSS.
2. `vspace_unmap_pages(VSPACE_FREE)` releases the eagerly-allocated
   BSS frames.
3. `vspace_reserve_range_at()` keeps the address range reserved for
   later fault-time mapping.
4. Store `[bss_lazy_start, bss_lazy_end]` and `bss_reservation` in
   `active_proc_t`.

The fault handler runs in two places:

* **exec_thread** -- handles foreground processes (EXEC_RUN/NICE)
  spawned with their own dedicated fault EP. Loop in
  `src/servers/exec_server.c` after the spawn, classify VMFault vs
  exit.
* **pipe_server dispatcher** -- handles background / forked-then-execed
  processes whose fault EP is minted into the shared `pipe_ep` with a
  per-process badge. `src/servers/pipe_server.c`, before
  `handle_child_fault`.

Both fault paths:

* If `seL4_Fault_VMFault` and `seL4_GetMR(seL4_VMFault_Addr)` is in
  `[bss_lazy_start, bss_lazy_end)`:
  * `vka_audit_check_headroom(1)` (refuse on pressure)
  * `vspace_new_pages_at_vaddr(bss_reservation)` for one page
  * `vka_audit_frame(VKA_SUB_OTHER, 1)`
  * `ap->audit_pages_allocated++`
  * `seL4_Reply` to resume the thread; loop for more
* Else (real fault or exit): break out, take the existing exit path

EXEC_RUN_BG (getty) uses minted pipe_ep so pipe_server handles its
faults too. Without this, getty would deadlock on first BSS access.

### IPC anonymous mmap (v0.4.104)

`PIPE_MMAP_ANON` (label 83) in `pipe_server.c`. Caller sends number
of pages in MR0; server calls `vspace_new_pages` on caller's
proc.vspace and replies with the vaddr. Cap at 1024 pages (4 MB)
per call. Used by `aios_sys_mmap` in `src/lib/posix_misc.c` when
`MAP_ANONYMOUS` is requested.

### VKA accounting (v0.4.103, 0.4.109)

* `vka_audit_frame(sub, n)` increments per-subsystem and
  `vka_live_frames`
* `vka_audit_frame_release(n)` decrements `vka_live_frames`
* `vka_audit_check_headroom(n)` returns -1 if pool pressured;
  exec_server uses this to refuse spawn when free pages < 2000
* `vka_audit_release_proc_pages(&ap->audit_pages_allocated)` is
  called before each `sel4utils_destroy_process` to release the
  process's tracked pages
* `/proc/vka` and `/proc/meminfo` show real numbers

---

