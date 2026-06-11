# Changelog

Milestone-level history of AIOS (Open Aries). Versions are commit-granular
(`v0.4.NNN: ...` in `git log`); this file tracks the arcs that matter. Newest
first. "HW-verified" means confirmed on a real Raspberry Pi 4, not just QEMU.

## v0.4.197 (2026-06-11)
- Typematic runaway guards. HW failure mode (builds 2045/2046): the keyboard
  dies mid-press (the recurring TT-death), its RELEASE report never arrives,
  and host-side repeat then types the key FOREVER -- the echo/scroll/auth storm
  saturates core 0 and takes the net down ("rrrr..." / the v0.4.195 "#" flood;
  this also re-frames the v0.4.193 hang as likely runaway-driven, though the
  revert stands -- the yield added risk for no benefit). Guards: (1) a
  PORT_STATUS_CHANGE event disarms all typematic (a hot-removed/reset keyboard
  posts one -- this is the precise fix); (2) a dead-man cap (KBD_REPEAT_MAX_RUN
  = 300 repeats, ~20s) on consecutive repeats with no intervening report from
  the device. New `scripts/xhci_runaway_qemu_test.py`: hold a key, device_del
  the keyboard mid-hold -- repeats must stop (PASS; normal repeat unaffected).
- Interrupt-IN ring deepened 8 -> 32 buffers (the report page holds 64): fast
  typing + slow login-time echoes could drain 8 while the driver blocked in one
  echo Call; an empty ring stops the controller polling the
  LS-keyboard-behind-the-TT, which is the device-death trigger (HW-seen: died
  mid-"root" at the login prompt). 32 puts the cliff beyond any human burst.

## v0.4.196 (2026-06-11)
- REVERT v0.4.193 (eMMC poll-loop yield). On real HW it regressed disk-heavy
  interactive use: `ls -l /bin` (a stat storm of inode reads) froze, recovered,
  then the system fully hung (keyboard LED out, net dead). Mechanism: yielding
  inside the eMMC DATA-phase wait stretches every sector read (each yield is a
  round-robin lap through the core-0 busy-pollers), and the silent
  timeout-then-proceed path can churn retries in the sub-ms fast path,
  monopolizing core 0 -- the exact failure it intended to prevent. The
  motivating problem (eMMC waits starving the keyboard) was already properly
  fixed by v0.4.192 multi-arm, so the yield was speculative hardening with a
  real cost and no remaining benefit. LESSON: do not insert scheduler yields
  into a device DATA phase; if eMMC waits need hardening, bound + yield only
  the line-free waits (cmd/dat inhibit), never between buffer-ready and the
  FIFO drain.

## v0.4.195 (2026-06-11) (merged from fix/reboot-flush-fs-thread)
- Reboot/shutdown now flush the write-back block cache via `FS_SYNC` on the
  fs_thread instead of calling `blk_cache_flush()` directly from the root
  task. The cache and the backend DMA ring are single-owner (fs_thread);
  the direct call raced any in-flight fs IO at shutdown. QEMU
  `reboot_flush_qemu_test.py` 4/4 (dirty data survives a watchdog reboot);
  harness uses private disk copies + a throttled writer so concurrent test
  sessions cannot corrupt each other. HW-pending -- rides the next card swap
  together with v0.4.193 (eMMC-yield) and v0.4.194 (V3D Phase 0 re-verify).

## v0.4.194 (2026-06-11) -- HW-verified (merged from design/rpi4-v3d-driver)
- V3D 4.2 GPU Phase 0 (power/IDENT/IRQ): `src/gpu/v3d.c` claims the pre-mapped
  MMIO, binds IRQ 106 masked, and pins the VC mailbox tag buffer -- with ZERO
  V3D register access in the boot path (a power-gated block SErrors; the power
  sequence runs only on an explicit `/proc/v3d.power` poke). HW-verified on the
  real Pi: DEAD->PASS power-on, idempotent, reboot-stable; V3D 4.2, 8 QPUs,
  MMU present. Design docs (DESIGN_V3D_IMPLEMENTATION.md) merged alongside;
  key correction vs the research doc: hub IDENT0 = "VHUB", core = "V3D\004".
  QEMU is a no-op (`has_v3d=0`); `scripts/v3d_qemu_test.py` 5/5 validates the
  plumbing (graceful refusal + display path not regressed).

## v0.4.193 (2026-06-11)
- RPi4 eMMC: the polled completion waits (`emmc_wait_int` / `emmc_wait_cmd` /
  `emmc_wait_dat`, src/plat/rpi4/blk_emmc.c) now `seL4_Yield()` once a wait
  exceeds 1ms (`EMMC_YIELD_MS`). Previously a missed SDHCI status bit busy-spun
  the FULL timeout (up to `EMMC_DATA_MS` = 2s) on the fs_thread with no yield --
  monopolizing core 0, which every root server (display, tty echo, USB driver)
  shares, so one bad eMMC wait stalled the whole interactive system
  (v0.4.176-class hazard, same lesson: HW poll loops must not own the core).
  The sub-ms fast path stays a tight spin -- zero throughput cost; during a
  real multi-ms wait the card is the bottleneck, so yielding is free.
  QEMU-clean (virtio path unaffected): sync 5/5, smp 7/7. HW-pending -- rides
  the next card swap (kernel-only).

## v0.4.192 (2026-06-11) -- HW-verified
- USB keyboard: fixed the "dies on the first key press" hardware regression
  (LS keyboard behind the VL805 TT resets, lock LED out, input dead). ROOT
  CAUSE: the driver armed ONE interrupt-IN transfer at a time and re-armed only
  AFTER the blocking SER_KEY_PUSH echo (tty -> HDMI render), so during any echo
  the keyboard went unpolled past its TT timeout and reset. The margin was
  always thin; the v0.4.188-191 timing shifts made it fail on ~the first key.
  FIX: multi-arm -- keep `INT_RING_BUFS`(8) interrupt-IN transfers armed in a
  ring of report buffers (FIFO completion order); a consumed buffer is
  re-armed BEFORE the blocking decode, so the controller never stops polling
  the keyboard. HW-verified: 158 clean decodes, full login + commands, then a
  second session with key repeat.
- Typematic (host-side key repeat): the HID boot keyboard only reports state
  changes, so repeat must be host-driven. Holding a key now repeats after
  500ms at ~15cps, latest press wins, release disarms. QEMU test:
  `scripts/xhci_typematic_qemu_test.py` (run of 17, stops on release).
- Lock-key LEDs are now SOFTWARE-ONLY at runtime (Num/Caps still gate the
  numpad/case; the physical LED stays at its enumeration state). HW-proven
  (serial cc=6 STALL then cc=-1 timeout): with the multi-arm ring full, a
  runtime SET_REPORT through the TT stalls this LS keyboard and wedges it --
  the v0.4.185 kill resurfaced; v0.4.186's endpoint-aware dispatch is not
  sufficient with a full ring. Proper fix (Stop Endpoint around SET_REPORT)
  is backlogged. The `/proc/xhci.led` poke remains as an explicit diagnostic.
- Process lessons from the debugging arc (see docs/NEXT_20260611b_kbd_resolved.md):
  verify every flash-free kernel8.img swap by sha-on-card AND the serial
  banner build number (several "failed" HW tests were stale kernels); one
  serial reader at a time (two readers split the byte stream); the v0.4.188
  flusher and the v0.4.190/191 changes were individually ruled out on HW.

## v0.4.191 (2026-06-11)
- thread_server: `pthread_join` no longer head-of-line-blocks the server. It was
  a blocking `seL4_Recv` on a per-thread fault endpoint, so one join froze all
  thread create/join across every process. Threads now fault onto the shared
  thread_server endpoint via a per-thread badged mint, and the server is an
  event loop with deferred reply caps -- joins park and other clients keep
  being served. Handles both orderings (join-before-exit, exit-before-join as a
  zombie), return-value propagation, and slot reuse.
- Two bugs the adversarial review caught and that are fixed here: (1) a process
  could `seL4_Send` on its own thread's fault cap to forge an exit of a *live*
  thread (cross-process use-after-free) -- the server now `seL4_TCB_Suspend`s
  before freeing, so the worst case is self-DoS; (2) allocating the reply slot
  inside the join handler risked a nested `seL4_Call` clobbering the saved reply
  cap (hang) -- the slot is now pre-allocated at thread create.
- Tests: `scripts/thread_qemu_test.py` 5/5 (`test_join` deferred/zombie/retval/
  reuse/concurrent; `test_threads` mutex counter == 2000). Both trees build.
- Pre-existing limitations surfaced (not fixed here): libaios stdio is not
  thread-safe (a concurrent `printf` from a worker + main corrupts stdout);
  `pthread_detach` is a stub (a detached thread zombies a slot until proc exit).

## v0.4.190 (2026-06-10)
- Security: closed a local privilege-escalation. Previously any non-root process
  could send `PIPE_SET_IDENTITY(0)` to set its own `active_procs[].uid = 0` and
  gain filesystem root-write. The handler is now gated on the caller already
  being root. getty and sshd were restructured to drop privilege in the forked
  shell **child** (root at fork) rather than mutating their own long-lived slot,
  so they stay root and every login/session gets the correct identity.
- Defense-in-depth: `exec_server` now derives a child's authz uid from the
  caller's real uid (badge), only honouring the caller-supplied `CWD=uid:gid`
  for a root caller -- removing the latent forge-CWD vector (not user-reachable
  today, since only the root task holds `exec_ep`).
- Tests: `scripts/identity_qemu_test.py` (6/6: root/user/multi-login all work;
  the exploit is denied `rc=-1` and ineffective via `idtest`) and
  `scripts/ssh_nonroot_qemu_test.py` (2/2: non-root SSH login gets `user`
  identity). Root SSH regression 6/6. Both trees build; adversarially reviewed
  (the sshd cross-session regression it found is fixed here).

## v0.4.189 (2026-06-10)
- Salted password hashing: the auth server (`src/apps/auth_server.c`) now stores
  `$a1$<salt>$<hash>` where the hash is SHA3-512 iterated 12000x over
  `(h || salt || password)`. Per-user salt defeats rainbow tables and makes
  identical passwords hash differently; the iteration count adds a brute-force
  work factor. Replaces the old bare, unsalted single SHA3-512. (Honest scope:
  a research-OS work factor, not Argon2/bcrypt-grade.)
- `scripts/gen_etc_passwd.py` writes `disk/rootfs/etc/passwd` with matching
  hashes, reading the KDF constants from `aios_auth.h` so host and device never
  drift. Clean break: old unsalted hashes no longer authenticate (reset with
  `passwd`). Constant-time hash comparison; plaintext zeroed on every path.
- Removed dead `src/aios_auth.c` (a stale, unbuilt duplicate auth server still
  on the old unsalted format).
- QEMU-verified: `scripts/auth_kdf_qemu_test.py` (login with a host-generated
  hash proves the KDF matches byte-for-byte; wrong password denied) + SSH login
  regression 6/6. Both trees build; adversarially reviewed (no memory-safety or
  bypass bugs).

## v0.4.188 (2026-06-10)
- Durability: implemented POSIX `sync(2)`/`fsync(2)`/`fdatasync(2)`, wired to a
  new `FS_SYNC` IPC that flushes the write-back block cache on the fs_thread
  (the only thread that may touch the unlocked cache + DMA ring).
- Periodic write-back flusher: a dedicated root thread (`flush_server.c`,
  modelled on `serverstats`) issues `FS_SYNC` every 30s, bounding the data-loss
  window of drive 0's write-back policy to the last half-minute. Stats at
  `/proc/flush`; `/proc/cachestats` now also reports `flushes` and live
  `dirty`. Test: `scripts/sync_qemu_test.py` (5/5: flusher live, sync returns,
  data survives, periodic count advances, dirty settles to 0).

## v0.4.187 (2026-06-10)
- SSH failed-login backoff: escalating delay before each counted failed
  password attempt (2s/5s/8s across the three attempts of a first connection;
  +1s per prior failure this boot; capped 10s); reconnecting no longer resets
  the cost. Successful first-try logins are unaffected.
- SSH pre-auth hardening: an unauthenticated client can no longer hold the
  single-session server forever -- 60s pre-auth grace deadline (ARM-counter
  time-based, polled via O_NONBLOCK reads), 64-line cap on version-exchange
  junk, 16-round caps on the service-request and userauth loops.
  Test: `scripts/ssh_backoff_qemu_test.py` (backoff, flood, idle-drop,
  recovery).
- LGPL compliance: the committed TinyCC source tree now carries its COPYING
  (`disk/rootfs/usr/src/tcc/COPYING`).
- Developer experience: `compile_commands.json` generated for both build trees
  (clangd/IDE support), third-party license attribution
  (`THIRD_PARTY_LICENSES.md`), `CONTRIBUTING.md`, `.clang-format`,
  `.editorconfig`, this changelog; README achievements list moved here.

## v0.4.184 - v0.4.186 (2026-06-08 - 2026-06-10) -- standalone Pi: USB keyboard + HDMI
- **USB HID keyboard on real RPi4** (HW-verified): full stack brcmstb PCIe
  (link trains 5.0 GT/s) -> VL805 xHCI -> USB hub enumeration -> HID boot
  keyboard. The shell mirrors to the HDMI framebuffer console, so the Pi runs
  fully standalone -- USB keyboard in, HDMI monitor out, no serial cable or
  laptop. QEMU harnesses: `scripts/xhci_key_qemu_test.py` + the hub variant.
- Five QEMU-invisible hardware bugs fixed along the way (scratchpad Hi/Lo swap,
  DMA rings above the 3GB PCIe inbound window, the internal VL805 hub,
  connect-debounce, LS-via-TT routing).
- Follow-ups: lock LEDs (HW-verified), Ctrl modifier decode (Ctrl-C),
  multi-device + USB mouse with `/proc/mouse` (QEMU-verified), opt-in
  IRQ-driven xHCI (`/proc/xhci.irq.1`), numpad + Caps Lock, cacheable (fast)
  HDMI console, `/proc/fbcon` scroll diagnostics.
- Known open item: the HDMI console freezes on first scroll on real hardware
  (pre-existing cacheable-FB scroll issue; diagnostic shipped).

## v0.4.179 - v0.4.183 (2026-06-07) -- 4-core SMP + process scaling
- **RPi4 4-core SMP** (HW-verified: `/proc/hw` cores=4, 0% ping loss): all
  four Cortex-A72 cores boot via spin-table; the long-blamed "SMP hang" was an
  invisible-elfloader ghost.
- Concurrent-process ceiling raised ~5x: process table 16 -> 64, demand-paged
  ELF `.text` (resident = executed code only), read-only `.text` shared across
  same-binary processes.

## v0.4.172 - v0.4.178 (2026-06-05 - 2026-06-07) -- storage speed + SSH recovered
- Write-back block cache + CMD25 multi-block eMMC writes: 4.5x faster file
  writes on real hardware (HW-verified).
- Time-based (not iteration-count) completion waits across eMMC, GENET, and
  the VC mailbox -- removed a 32.6s hardware stall QEMU could never show.
- **SSH over LAN restored and always-on**: rebuilt libmbedcrypto, getty
  auto-starts sshd, sequential-reconnect fixes (fd-slot + auth-session leaks),
  relay self-heal, **sftp + scp**, `nslookup` (DNS resolver), `pidof` /
  `pkill` / `killall`.
- `ls -l` shows file mtimes (stat read path plumbed, v0.4.174).

## v0.4.162 - v0.4.171 (2026-06-04 - 2026-06-05) -- Pi on the LAN
- **GENET Ethernet end-to-end on real RPi4** (HW-verified): full DMA datapath,
  RGMII, mailbox MAC, DHCP lease, bidirectional ping, IRQ-driven RX.
- **Network control channel (netconsole)**: run commands, push/pull files
  (`pi_filexfer.py`), and reboot the Pi over the LAN -- no serial needed after
  flashing.
- Wall-clock time via SNTP at boot; ext2 mtimes written on create/mkdir.
- **RPi4 HDMI lit** (v0.4.168): VideoCore mailbox framebuffer at 1024x768 with
  fb_console; colour fix (BGR) + software 3D cube demo.
- Robust large-file transfer: 32KB RX ring, TCP receive fix, ext2
  double-indirect block writes.

## v0.4.135 - v0.4.161 (2026-06-02 - 2026-06-04) -- back on hardware
- Return to the real Raspberry Pi 4: first boot of the modern system since the
  v0.4.92/93 bring-up (v0.4.135, SMP fallback to single core), flash tooling
  hardened (built-in SDXC reader + post-flash hash gate, v0.4.136).
- RPi4 device-MMIO mapping discipline (ascending-paddr watermark), GENET
  bring-up saga, DHCP client (QEMU SLIRP + real LAN).
- Parent-side COW page stripping enabled (v0.4.148), file-backed mmap
  via demand paging, virtio-blk completion-race hardening, pipe EOF
  correctness under SMP contention.

## v0.4.95 - v0.4.134 (2026-04 - 2026-05) -- self-hosting + memory architecture
- **TCC self-hosting proven**: all 12 TinyCC source files compile natively on
  AIOS; tcc2 binary runs.
- Demand-paged BSS (10-50x fewer per-process pages), COW fork for the data
  segment (Phase 1.5), block cache with reclaim under pressure, boot warmup
  prefetch + `/proc/filehits` profiler.
- Real `mprotect` / `munmap` / `ftruncate`; `/proc/cmdline`, log rotation,
  VKA observability.
- RPi4 prep: SD-card image builder (the mtools-FAT32 firmware trap closed
  with `newfs_msdos`), one-shot flasher script, build target + serial-boot
  relocator stub.
- ZSH Phase 2: interactive ZLE line editor, termios infrastructure,
  libtermcap.

## v0.4.83 - v0.4.94 (2026-04) -- SSH + crypto + first hardware boots
- SSH server phases 1-5: ECDH key exchange, AES-256-CTR + HMAC-SHA-256
  transport, password auth via auth_server, session channel with interactive
  shell relay.
- TCP `connect()`, dynamic windows, socket lifecycle cleanup.
- crypto_server: ChaCha20 CSPRNG behind `/dev/urandom` + `getrandom()`.
- **First boots on real hardware**: seL4 + the root task on a real RPi4
  (v0.4.92), then a full interactive AIOS session -- HDMI display, SD card
  driver, serial login (v0.4.93).

## v0.4.68 - v0.4.82 (2026-04) -- a real Unix userland
- dash becomes the login shell (replacing mini_shell); TTY cooked/raw rework.
- Framebuffer display (ramfb), font rendering, splash; TCC compiles C
  natively; TCP sockets with IRQ-driven virtio-net.

## Earlier (0.4.0 - 0.4.67)
- The 0.4.x line: single root task on bare seL4 (no Microkit), ext2 + VFS +
  procfs, POSIX shim over musl (86+ syscalls), fork+exec+waitpid from disk,
  signals, pipelines, sbase userland, auth server (SHA-3-512), pthreads.
- Pre-0.4 branches (0.2.x, 0.3.x) explored Microkit-based designs; 0.1.x-era
  experiments included FAT16 and an on-device LLM ("ai build") study.
