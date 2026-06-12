# Changelog

Milestone-level history of AIOS (Open Aries). Versions are commit-granular
(`v0.4.NNN: ...` in `git log`); this file tracks the arcs that matter. Newest
first. "HW-verified" means confirmed on a real Raspberry Pi 4, not just QEMU.

## v0.4.225 (2026-06-12)
Found and fixed the REAL push-corruption root cause -- an ext2 image-builder
off-by-one, not a network or eMMC bug. (The v0.4.224 eMMC hardening and this
are separate, both real; v0.4.224 fixed a stall-quanta-only fault, this fixes
a deterministic one.)
- **mkdisk Ext2Builder layout off-by-one** (scripts/ext2/builder.py): block
  group g>0 was laid out starting at `g*blocks_per_group`, but the kernel
  allocator (ext2.c) uses the standard `first_data_block + g*bpg` (= g*bpg+1).
  The 1-block disagreement made the kernel's "last block of group g" alias
  group g+1's BLOCK BITMAP. The allocator handed that bitmap block out as
  file data; subsequent allocations in g+1 rewrote the bitmap, clobbering the
  file's first ~124 bytes with 0xFF (a filling bitmap). Only bit -- groups
  0-9 happened to have their cross-group metadata marked used, so it bit only
  files large enough to reach group 10 (~960KB+), which is why 1.5MB kernel
  pushes hit it ~30-50% of the time and small files never did. Fix: standard
  `1 + g*bpg` for all groups + mark last-group padding bits used. All 16
  groups' metadata now correctly placed and reserved.
- This is the deterministic QEMU-reproducible corruption (3s/push, no stall
  quanta) behind the v0.4.223/224 deploy scare. Diagnosis chain: inbound TCP
  checksum-verified clean (new /proc/netstat counters, all zero) -> Pi-side
  sha == pulled sha != source (on-disk, write path) -> ext2 write probe
  showed correct data written then clobbered -> the clobbered block WAS a
  group's block-bitmap. netrx_repro.py: was 2-3/8 corrupt, now 0/16.
- Also added this session: inbound TCP checksum verification (drops + counts
  mangled segments instead of accepting them), SPSC rx-ring acquire barrier,
  /proc/netstat counters + a drop-every-Nth fault-injection knob
  (scripts/netrx_qemu_test.py), all of which proved the network path INNOCENT
  -- kept as permanent hardening + regression tooling.

## v0.4.224 (2026-06-12)
Block-layer integrity hardening -- the root-cause fix for the push corruption
caught (and worked around) during the v0.4.223 fatswap deploy. The corrupt
124 bytes were 0xFF: an empty-FIFO PIO drain.
- **eMMC data-phase timeouts are now failures** (blk_emmc.c): the v0.4.176
  time-based waits kept the old "caller proceeds" fall-through on timeout;
  under a 32.4s stall quantum the 2s deadline expires while the CPU is
  frozen, INT_BUF_RD never latches, and draining the data port returns
  0xFFFFFFFF per word -- which the ext2 partial-block RMW then merged into a
  file. Reads no longer drain an un-ready FIFO; writes no longer report
  success without XFER_DONE (a lost write let the write-back cache clear
  dirty on unprogrammed data). Each path retries the whole idempotent
  command once, then fails loudly. Counters in /proc/cachestats
  (emmc_timeout_retries/fails).
- **blk_cache evict_one** no longer discards a dirty line whose flush
  failed (silent write loss); it skips that victim, bounded.
- **ext2_pwrite_file** propagates write_block failures (RMW + zero-fill
  paths previously ignored them).
- The XFER_DONE-after-drained-FIFO read case keeps the old accept behavior
  (data is already in hand; the miss is bookkeeping).
- HW-verified on the Pi (deployed via pi_flash, banner v0.4.224 build 2096).
  The new counters immediately earned their keep: a 4-push forensic run still
  corrupted 2/4 pushes (short 0xFF-biased runs at block-aligned offsets, one
  recurring at +0xf0800) with emmc_timeout_* = 0 and a LOCAL 1.5MB cp through
  the same fs path byte-perfect -- so a SECOND, separate corruption source
  lives in the INBOUND TCP receive path (likely retransmit resequencing under
  stall-quanta loss). Characterized + contained (pi_flash pull-back verify);
  hunt doc: docs/NEXT_20260612_net_rx_corruption.md.

## v0.4.222 (2026-06-12)
FAT32 flash-over-network ("fatswap"): kernel deploys are now a network push +
reboot instead of a physical card swap (the v0.4.221 stall hunt burned ~18
card pulls; this ends that).
- **src/fat32.c + FS_FATSWAP**: root-task module that rewrites KERNEL8.IMG on
  the FAT32 boot partition IN PLACE -- never reformats, never moves dirents
  (the Pi firmware is intolerant of FAT relayout). Crash-ordered so a power
  cut at any instant leaves a bootable kernel: allocate new chain -> write
  data -> link both FATs -> COMMIT via one single-sector dirent write -> free
  old chain + FSInfo. Runs on the fs_thread (single-owner block I/O), reads
  the source from ext2, writes the FAT partition through new absolute-LBA HAL
  entry points (plat_blk_*_abs -- cache-bypassing; the FAT LBA range is
  physically disjoint from every cached ext2 sector). Reports sha256 of the
  bytes written AND of a full readback through the new dirent chain.
- **/bin/fatswap** (AiosPosixApp) + **scripts/pi_flash.py**: one host command
  = push kernel8.img over netconsole __put -> fatswap -> three-way sha verify
  (local == pushed == on-card readback) -> reboot -> ping-wait -> /proc/version
  banner check against the string baked in the kernel binary.
- **virtio MBR parse**: QEMU now boots composite (MBR-partitioned) images with
  the real SD layout (FAT32 boot at 2048 + ext2), with bare-ext2 fallback, so
  the whole path is QEMU-testable. scripts/fatswap_qemu_test.py: 46/46 --
  swaps (grow/shrink), exact free-cluster accounting, FAT mirror + dirent +
  FSInfo invariants via an independent python FAT32 reader, all three
  crash-abort points, reboot persistence, bare-image regression.
- **HW-VERIFIED end-to-end (v0.4.223)**: after the one bootstrap card swap
  (v0.4.222), pi_flash.py deployed v0.4.223 with zero card handling -- push,
  pull-back verify, fatswap (3048 clusters, 3-way sha match), watchdog
  reboot, banner check -- AND a cold power-cycle boots it (the firmware
  cold-reads the rewritten FAT, the risk that mattered).
- Found en route (pre-existing): the ext2 write path corrupted one push by
  EXACTLY 124 bytes (= 1024-900) at a block boundary -- a partial-block RMW
  merged against a stale pre-read during a 129s stall quantum. pi_flash
  works around it (__get pull-back compare + retry; never trust a bare
  `__put ok` for MB files). Also: the 32.4s-multiple freezes fire with NO
  keyboard attached (updates the Source-B picture); `wc -c < FILE`
  stdin-redirects return garbage (fd routing); sbase ls aborts the whole
  listing on one missing arg.

## v0.4.221 (2026-06-12)
The 32.4-second whole-system freeze hunt (the marathon: ~18 kernel swaps of
layered instrumentation -- pipe message -> reap phase -> thread-cleanup
sub-phase -> per-page per-op -> kernel invocation -> kernel unmapPage phases).
The freezes pin to the kernel's `dsb; tlbi vae1; dsb; isb` stalling in exact
~10.8s quanta (32.4/43.2/64.8/97.2s observed) with IRQs off, freezing
EVERYTHING including the timer tick. TWO independent triggers found on
BCM2711:
- **Source A -- WFI retention (FIXED)**: a core that has executed WFI
  subsequently stalls TLBI+DSB. Kernel idle thread now spins (`yield`) instead
  of WFI (deps/kernel idle.S, captured in deps/patches/seL4-kernel.patch).
  HW-verified decisive A/B: PCIe-off + WFI stalls, PCIe-off + no-WFI is
  completely clean (0/12, and baseline spawn latency HALVED to ~0.3s -- the
  retention micro-stalls were inside every spawn).
- **Source B -- VL805 keyboard split-transaction DMA (MITIGATED, open)**: with
  PCIe up, the xHCI controller's periodic LS-keyboard polling DMA (fires every
  interval even with no typing) collides with TLBI/DSB the same way. Live
  evidence: stalls stop the instant the keyboard unplugs. Immune to: SMP 1 vs
  4, the TLBI-keepalive hammer, UBUS/retry timeout registers. Mitigation: LS/FS
  polling clamped to 32ms (4x fewer trigger windows than 8ms). A quantum that
  fires with the keyboard attached still kills it (32s unpolled = LS wedge).
  REAL FIX (next session): why VL805 inbound DMA vs TLBI/DSB collides on AIOS
  but not Linux -- DMA pool mapping attributes / brcmstb inbound window / MSI.
- Interim config: KernelMaxNumNodes=1 (everything is pinned to core 0 anyway);
  restore SMP=4 with the A/B harness in hand. New src/servers/tlbi_probe.c
  (TLBI keepalive + measurement probe). sel4utils_free_reservation NULL-res
  guard (latent crash found during the hunt; deps/patches/seL4_libs.patch).
  [pipe]/[reap] >250ms probes remain permanent. Diag kernels v0.4.206-217
  were investigation-only (per-phase brackets, since removed).

## v0.4.203 (2026-06-12)
- Keystroke-rate fix (the "sticky keyboard / missed keys at fast typing"
  reports). Two per-keystroke costs were holding the USB driver thread long
  enough to drain the 32-deep interrupt ring under load (drained ring =
  unpolled keyboard = dropped keys): (1) a 40-char `[xhci-kbd]` printf to the
  POLLED mini UART (~3.5 ms busy-wait per key, plus an HDMI line render and a
  full screen scroll once the display is full) -- now OFF by default, runtime
  togglable via `cat /proc/xhci.debug.1|0`, with a key-event counter kept for
  the `/proc/xhci` summary (the `[xhci-mouse]` per-motion printf is gated the
  same way); (2) the tty server ran the whole echo chain (polled-UART putchar
  + a blocking DISP_CONSOLE call into the display server) BEFORE replying to
  SER_KEY_PUSH/TTY_INPUT -- it now replies first and echoes after, so the
  driver is released in microseconds and echo cost stays on the tty thread.
  QEMU: fb_scroll 4/4, v3d 7/7, xhci typematic PASS.

## v0.4.202 (2026-06-12)
- **V3D Phase 1 HW-VERIFIED on a real Pi 4**: `.power` -> `.mmu` -> `.fault`
  first light. The MMU programmed cleanly (PT@0xfb000000, 1M PTEs,
  MMU_CTL=0x060d0c01), and the deliberate-fault probe hit every exit
  criterion: CT1 kicked at the unmapped GPU VA 0x20000000, the hub latched
  PTI (`hub_int_sts=0x10`), `CT1CA=0x20000001` proved the render thread
  genuinely fetched at the target VA, and IDENT still passed after the
  dump-and-reset. Core clock was already 250 MHz from firmware. One open
  detail for Phase 2: `VIO_ADDR raw=0x04000018` does not decode to the
  fault VA with Linux's `<<8` (CT1CA confirms the address independently).
- Stall investigation (the "USB keyboard dies / system freezes" reports):
  episodic 20-160 s whole-system stalls were proven KERNEL-INDEPENDENT by
  A/B (v0.4.199 pool-on stalled, v0.4.200 pool-off stalled, v0.4.201/202
  boots clean) -- a per-boot environmental fault, NOT the V3D Phase 1
  change. Disproven along the way: the V3D pool grab (A/B + QEMU
  force-enable), eMMC timeouts (none logged), the periodic flusher
  (FS_SYNC is a no-op with dirty=0), EDID (errors on clean boots too).
  Evidence points at the pipe server going dark (serverstats: pipe 51 ms
  avg vs 19 us healthy) with per-boot USB-keyboard health as the leading
  correlate. Left in place to catch the next natural occurrence: permanent
  diag probes that print `[pipe] SLOW msg label=N badge=N took Nms` for
  any pipe-server message over 250 ms and `[reap] SLOW` with phase
  breakdown for slow teardowns. Healthy-boot baseline measured: fork
  261 ms, exec 314-970 ms per spawn (the "sticky keyboard" feel).

## v0.4.199 (2026-06-11)
- V3D GPU Phase 1 -- MMU + deliberate-fault probe (continues the HW-verified
  Phase 0 power/IDENT bring-up; see `docs/DESIGN_V3D_IMPLEMENTATION.md` §8).
  New `v3d_mem_reserve()` sets aside an 8 MB phys-contiguous, non-cacheable GPU
  pool (4 MB single-level page table + a 4 KB illegal-address scratch + a bump
  region) the same way the xHCI/GENET DMA pools are carved -- EARLY in boot,
  before the fs cache and display FB consume the low frames (it must be
  contiguous). The MMU is programmed lazily, never in the boot path.
- Two new `/proc/v3d` verbs: `cat /proc/v3d.mmu` builds the page table (all
  entries invalid, then one writeable 4 MB data window at GPU VA 0x00100000),
  programs the V3D MMU in Linux's exact order (PT_PA_BASE -> MMU_CTL with
  PT_INVALID/WRITE_VIOLATION/CAP_EXCEEDED abort+int -> ILLEGAL_ADDR ->
  MMUC_CONTROL enable -> two-step flush), and reports the registers.
  `cat /proc/v3d.fault` kicks the render thread (CT1) at the deliberately
  UNMAPPED GPU VA 0x20000000, polls the hub interrupt status for the
  page-table-invalid latch (the IRQ stays masked -- we read the status), prints
  VIO_ADDR/VIO_ID + the candidate byte decodings, then dump-and-resets (clear
  hub+core INT, flush) and re-checks IDENT. Best-effort `v3d_ensure_clock()`
  sets the V3D core clock (the CLE needs it to fetch; `.power` only clocks the
  register domain). All MMU/CLE/PTE register offsets transcribed VERBATIM from
  Linux `drivers/gpu/drm/v3d` (rpi-6.6.y) into `v3d_regs.h`, never from memory.
- QEMU has no V3D model, so both new verbs refuse gracefully (has_v3d=0);
  `scripts/v3d_qemu_test.py` extended to 7/7 (adds `.mmu`/`.fault` graceful-
  refusal + the unchanged display-path regression check). First light is
  HW-only: kernel8 staged for a real-Pi `.power` -> `.mmu` -> `.fault` run.

## v0.4.198 (2026-06-11)
- Reproducible build environment (sweep item 1d -- the wrap-up before sharing
  the repo). New `./build_environment.sh` takes a fresh clone to a booted QEMU
  system: host-tool check (with exact brew/apt commands for anything missing),
  clones every dependency at the commit pinned in the new `DEPS.md`, applies
  the captured patch set from `deps/patches/` (seL4 DTS, musl GCC-15
  visibility, seL4_libs morecore/vspace, elfloader serial/boot, tcc arm64,
  mbedtls config -- plus vendored dash `config.h` / zsh `termcap.h` /
  elfloader `diag_fb_debug.h`), configures + builds (add `--rpi4` for the Pi
  tree), and smoke-boots QEMU to the login prompt. Idempotent; never clobbers
  an existing checkout; `--capture-patches` re-exports the patch set for
  maintainers. Verified end-to-end on a configured machine: 39 ok / 0 fail.
- The build scripts (`build_apps.py`, `build_sbase.py`, `build_zsh.py`,
  `build_mbedtls.py`) no longer hardcode `~/Desktop/github_repos/...`: sibling
  deps resolve relative to the repo parent, overridable with `AIOS_DEPS_ROOT`.
- README: new "Get going fast" section up top; Quick Start points at the
  script with the manual steps kept as reference.

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
