# AIOS HANDOVER

Self-contained briefing for a fresh development session. Read this end-to-end,
then the latest `docs/NEXT_*.md` and the memory index (`MEMORY.md`) for deeper
background. Older session arcs (v0.4.110 -> v0.4.168) live in
`docs/HANDOVER_HISTORY.md`.

---

## Quick orientation

* **Project**: AIOS (Open Aries) -- microkernel research OS on seL4.
* **Repo**: `~/Desktop/github_repos/AIOS`, branch `main`. The V3D cube is DONE +
  HW-VERIFIED + PUSHED (`9e543c6`, v0.4.252). NOTE: DHCP lease BOUNCES `.8`<->`.250`
  per boot -- ARP-sweep MAC `dc:a6:32:1c:2e:e1` if `.8` is dark.

* **CURRENT STATE 2026-06-17c -- NEXT = close the RPi4 idle-teardown TLBI stall via A72 CPUECTLR/cluster
  config (the "Linux gap").** Pi runs **v0.4.261 build 2596** at 192.168.0.8 (SHM-ring complete +
  HW-validated; tlbi_probe removed). The remaining open item is the ~8% idle-teardown TLBI/DVM freeze
  (33-66s; `netstall.py --idle 8` = ~2/24). **Bryan's key reframing: it is NOT a BCM2711 HW limitation --
  Linux on the same Pi4 has no such freeze -- so AIOS/seL4 is MISSING A72/cluster config Linux does.**
  Grep-proven gap: AIOS sets NOTHING in the A72 IMP-DEF regs (`CPUECTLR_EL1`/`L2CTLR_EL1`/`CPUACTLR_EL1`)
  -- it relies entirely on the armstub. SMPEN is effectively ON (the SHM-ring cross-core test was
  byte-exact, which needs coherency), so the suspect is the **cluster/L2 retention / DVM config** left at
  reset: even with the no-WFI idle, the fabric goes "cold" so the FIRST post-idle `tlbi vae1` DVM
  completion hangs to the UBUS timeout. Plan: probe `CPUECTLR_EL1` on AIOS (may be EL3-trapped), diff vs
  Linux's A72 setup + the RPi armstub, A/B the delta, re-soak to 0/30. **Full seed:
  `docs/NEXT_20260617_a72_cpuectlr_stall.md`.** Ruled out already (don't re-do): tlbi_probe, idle-core
  quiescence (corewarm WORSE), the fastpath hook, dsb-scope, tlbi-count reduction. The residual is
  ACCEPTED + BACKLOGGED (BACKLOG.md top, `22f6a1b`) -- this session is the focused attempt at it.
  [[project_stall_hunt]]. version.h = **261**; LOCAL commits ahead of origin (Bryan pushes).

* **CURRENT STATE 2026-06-16b (v0.4.258 SHM-ring session) -- direct SPSC SHM-ring pipes DONE on
  QEMU + adversarially reviewed + COMMITTED (`b113844`, local, ahead of origin; Bryan pushes).**
  A ring-mode pipe is a single-producer/single-consumer lock-free ring the writer AND reader both
  map (one 4 KB cacheable-inner-shareable frame) -- data flows in USERSPACE on the producer/consumer
  cores, `pipe_server` touched only at empty/full (the only fix for IPC-bound pipelines that Stage-S
  distribution alone can't give). **DEFAULT OFF** behind `/proc/shmring`. QEMU green:
  `shmring_qemu_test` 26/26 (data EXACT, sha256 match), `smp` 7/7 (ceiling 30, OFF byte-identical),
  net_socket 8/8, netd 10/10, all 4 trees build at v0.4.258. A 20-agent review found 8 bugs total
  (2 bring-up + 6 review), ALL FIXED + re-gated; its data-path barrier complaints were REFUTED +
  independently re-adjudicated as correct (`load idx; dmb ishld; load data` = canonical ARM MP
  acquire; a stale index is conservative-safe both ways). **HW: kernel flashed + verified (build 2573,
  4-core A72); server-mediated ring path HW-verified data-exact.** Then the big find (`629628c`/`8ff9445`):
  the direct ring NEVER engaged for real pipelines because the fast path was wired only into raw
  read()/write(), NOT the stdio backend or writev/readv (how filter tools do pipe I/O); the QEMU
  `map_ok=33` was the netconsole RELAY. **FIXED + COMMITTED `9836f67`: 3-path ring-ification + the
  direct-reader EOF bug (a 0-length first iov from musl buffered stdio mistaken for EOF). `seq|wc` now
  uses the direct ring (map_ok=8, push=0 writer-direct, exact, no hang; shmring 26/26, smp 7/7, socket
  8/8).** **Then the #1 risk -- A72 CROSS-CORE COHERENCY -- was VALIDATED on real HW (2026-06-17): no
  re-flash (libaios-only fix; kernel 2573 current), pushed seq/wc/sha256sum to /tmp, armed shmring.1 +
  coresched.1 (writer+reader on different A72 cores), ran seq 1 100000|wc -l ==100000 (x3) + wc -c
  ==588895 + seq 1 5000|sha256sum == HOST ref (BYTE-EXACT); map_ok=15, ~99.7% of ~1.77MB flowed DIRECT
  across cores. The cacheable-inner-shareable + dmb ishst/ishld/ish barriers are correct on the A72; the
  all-NUL/stale-index class is RULED OUT. Driver scripts/shmring_hw_xcore.py (robust 300s-timeout chunked
  push; read /proc/shmring AFTER disarming coresched -- counter persists, netconsole wedges under load).**
  **THE SHM-RING IS COMPLETE: pipelines span cores, kernel out of the data path, byte-exact on silicon.**
  NEXT epics: multi-end SPSC auto-fallback (ring assumes 1 reader+1 writer), the throughput/ceiling-win
  measurement (TCG can't show it), perf (bigger ring / wake batching), then the multikernel re-arch (BACKLOG).
  Seeds: `docs/NEXT_20260616e_shm_ring_pipe_hw.md` (HW plan + the full fix record),
  `docs/NEXT_20260616d_shm_ring_pipe.md` (original). [[project_shm_ring]].
  - **v0.4.261: removed the tlbi_probe keepalive -- HW A/B SOAK DONE (it is REDUNDANT).** The v0.4.216
    core-0 unmap/map hammer (+ its `[tlbi] alive` console noise) was belt-and-braces. HW A/B on the real
    Pi (`scripts/netstall.py --trials 24 --idle 8`): tlbi REMOVED = 3/24 stalled; tlbi RESTORED = 3/24
    stalled -- IDENTICAL, so it makes NO difference -> removed for good (Pi flashed **v0.4.261 build
    2596**). Version churn: 259 removed / 260 false-alarm restore / 261 re-removed (db5d543, 6d24258,
    re-remove). QEMU smp 7/7 + shmring 26/26, boot console `[tlbi]`-free.
  - **NEW OPEN ISSUE the soak surfaced (separate, pre-existing): a ~3/24 (12.5%) idle-teardown residual
    stall** -- `sleep 8; echo` still freezes 33.5s (=3x10.8s TLBI quanta) ~12.5% of the time on the
    CURRENT tree, REGARDLESS of tlbi_probe. The recorded "build 2518 = 0/30+" does NOT reproduce, so the
    per-ASID masked shootdown (32dbc39) reduced (6/16->3/24) but did NOT fully cure the idle-teardown
    freeze. Prime suspect for the regression since 2518: the Stage-S fastpath residency hook (06e0edd,
    a kernel change in the exact TLB-shootdown residency path). **Follow-up queued** (spawn_task +
    [[project_stall_hunt]]). **version.h = 261.** Pi runs **v0.4.261 build 2596**.

* **CURRENT STATE 2026-06-16 (v0.4.257 SMP session) -- USB hotplug epic done; RPi4 remote-TLBI
  STALL FIXED on HW; opt-in multi-core; SHM-ring pipe groundwork.** The Pi runs **v0.4.257 build
  2523** at 192.168.0.8 (4-core SMP; DHCP bounces `.8`/`.250`/`.197`; ARP `dc:a6:32:1c:2e:e1`).
  LOCAL commits ahead of origin/main (Bryan pushes; 9cc6fe4 + 32dbc39 already pushed):
  - `9cc6fe4` **USB-MSC bulk-STALL recovery** (Stage 5): RESET_EP + ClearFeature + SET_TR_DEQ on a
    bulk `cc=6` so a SuperSpeed first-replug enumerates clean. QEMU 9/9, HW-verified.
  - `32dbc39` **per-ASID residency-masked TLB shootdown -- the RPi4 ~32s remote-TLBI stall is FIXED
    on real HW** (0/48 teardown-after-idle freezes vs 6/16 baseline). Cause found via a core-warmer
    A/B (`/proc/corewarm`): the broadcast shootdown hung core 0 in `ipi_wait` on quiesced idle cores
    1-3; the fix skips cores that never ran a vspace. Kernel change is in the seL4 working tree +
    `deps/patches/seL4-kernel.patch`. QEMU smp 7/7 + socket 8/8 no-regress. **The big win.**
  - `06e0edd` **Stage S: opt-in per-process core distribution** (`/proc/coresched`, default OFF) +
    a fastpath residency hook. Default OFF because distribution regresses IPC-bound pipelines
    (ceiling 30->6, seL4 BKL contention) but gives a **HW-proven 3.77x CPU-bound speedup**.
  - `4903fb9` **pipe SHM-write coalescing** (4KB writes; groundwork for SHM-ring pipes) +
    BACKLOG multikernel re-arch entry. Correct (smp 30/30) but NO win on compute-bound `seq|wc`
    (the pipe-write was never the bottleneck). UNFLASHED (Pi runs 2523, no coalescing).
  - **USB hotplug epic** (Path A+B, default-ON) was done just before this session (origin/main).
  **NEXT = the full SHM-ring pipe** (let a pipeline span cores -- the only fix for IPC-bound
  scaling that distribution alone can't deliver): **`docs/NEXT_20260616d_shm_ring_pipe.md`**.
  Stall-hunt + SMP detail: `docs/NEXT_20260616c_smp_tlb_stall_fix.md`. version.h = **257**.

* **SESSION 2 (2026-06-15, this session): USB-MSC HW bring-up + >2TB (READ16).** Verified the
  commit plan, then HW-tested the USB driver and added 64-bit-LBA support.
  - **USB driver HW-PROVEN.** Flashed build-rpi4-netd over the network (pi_flash); a real **4TB
    Buffalo External HDD** behind the VL805 hub enumerated (slot 3, SuperSpeed), INQUIRY +
    READ_CAPACITY + READ(10) LBA0 all worked, 512-byte blocks, keyboard coexisted, zero faults.
    `/mnt/usb` correctly declined (drive is GPT, not ext2). Serial-only signal (`/proc/xhci` does
    NOT show MSC); capture via `aios_console.py monitor <dev> --mirror <file>` then read the file.
  - **>2TB (64-bit LBA) DONE + HW-PROVEN.** The 4TB drive saturated READ_CAPACITY(10) at 0xFFFFFFFF
    (-> reported 2TB). Added in `xhci.c`: `scsi_read_capacity_16` (0x9E/SA0x10) as a saturation
    fallback, `scsi_rw16` (READ(16) 0x88 / WRITE(16) 0x8A), `scsi_blk_rw` dispatcher (LBA>0xFFFFFFFF
    -> 16-byte; <2TB byte-identical), 64-bit `g_msc_req.lba` + un-truncated `usb_blk_*`, and a
    read-only last-LBA self-test. HW (build 2464): `USB MSC ready: 7814037168 sectors x 512 = 3815447
    MB` (true 4.0TB) + `last-LBA(16) @7814037167: OK` (READ(16) of LBA 7.8e9 returned real data).
    QEMU: new `scripts/usb_msc_big_qemu_test.py` 4/4 (sparse 2.36TB image), usb_msc 5/5 +
    usb_msc_mount 6/6 no-regress, all four trees build. Adversarial 3-lens review = 0 bugs; it
    caught a PRE-EXISTING latent WRITE(10)-self-test buffer overlap (pat@+1024 vs read-back@+128
    collide for blocksize>896) -> fixed (pattern moved to the separate `msc_io` page).
  - **MOUNT HW-BLOCKED by firmware (Bryan diagnosed).** An ext2 USB drive present at boot makes the
    RPi4 bootloader try to boot FROM it and hang (no serial, no net) on cold AND warm reboot; the
    4TB data drive (NTFS) booted past via pi_flash's warm reboot, an ext2 drive does not. Recover
    by REMOVING the drive. So drive-at-boot is impractical -> **USB hub-port HOTPLUG is now required**
    (insert-after-boot; the drive is behind the VL805 hub so it needs the hub status-change EP, the
    backlogged hub-hotswap). A built + QEMU-verified ext2 image (`/tmp/usbstick.img`, AIOS-builder,
    `usb_stick_qemu_check` 5/5) is ready to `dd` once a path exists. [[project_usb_msc]].
  - **NETD/netconsole fragility seen again:** post-boot ~32s TLBI stall -> netd wedges (root alive,
    net dead); netconsole wedges under back-to-back connections. Known [[project_stall_hunt]] +
    [[project_netconsole]]; possibly aggravated by the always-polling USB driver thread.

* **SESSION 1 (2026-06-15, earlier): TCP regression FIXED + console cleanup + the USB driver (stages 1-4).**
  - **TCP fix (v0.4.254, DEPLOYED + HW-VERIFIED).** Root cause (5-lens diagnosis + a NEW
    lossy-QEMU repro -- the thing the lossless suite could never run): `3e3e26a`'s
    deferred-close froze a FIN_WAIT socket only on `(ACK) && !(FIN)`, so a real peer's
    COALESCED FIN+ACK lingered to the 10s give-up which sent a RST (a RST makes the peer
    DISCARD its unread data = the 0/20), and the give-up left stale rto/close state that
    POISONED the reused slot ("worked once, then every connect RSTs"). Fix (net_server.c):
    free on `fin_sent && snd_una>=snd_nxt` (accept FIN+ACK), `net_sock_tx_init` the SYN-child
    + on free, and **do NOT RST on give-up** (free silently). New `scripts/tcp_loss_qemu_test.py`
    (txdrop/findrop/ackdrop hooks + a `tcp_rst_sent` counter) reproduced it (rst_sent=1) and
    proved the fix (rst_sent=0, give-up still fires). [[feedback_qemu_cannot_model_loss]].
  - **Console cleanup (DEPLOYED).** Boot banner ([boot]/[dtb]/[fs]/[net]/[gpu]) -> serial
    (aios_root.c `printf`, off fb_console -- it cannot reach fb_console); getty version banner
    removed; netconsole startup banner removed; sntp quiet-by-default (`-v` to restore).
    getty/netconsole/sntp are DISK apps -> deployed by `pi_filexfer push` (no flash).
  - **Keyboard LEDs -- HW-tested, ring-resume WRONG, DISABLED safely.** The Stop-Endpoint +
    SET_REPORT half WORKS (the LED changes on `/proc/xhci.led.N`), but my interrupt-ring
    RESUME re-delivers a stale report (stuck 'r' -> dead keyboard). `#if 0`'d so it cannot
    wedge typing; corrected resume DESIGNED (doorbell-resume + drain the Stop event, NO ring
    reset) -- needs a serial-capture HW session. [[project_usb_hid]].
  - **USB Mass Storage / external HDD (NEW, QEMU-verified, UNCOMMITTED, v0.4.255 WIP).** A
    Bulk-Only-Transport/SCSI driver in `src/usb/xhci.c`: enumerate class-8 -> INQUIRY +
    READ_CAPACITY -> READ(10)/WRITE(10) -> **MOUNT at /mnt/usb** (read+write files that
    persist). Runtime-concurrency crux solved with an FS-thread -> xHCI-driver-thread request
    queue (the event ring is single-consumer). `blk_cache` extended to drive 2; mount via
    `usb_msc_mount()` (boot_fs_init.c). Tests: `usb_msc_qemu_test.py` 5/5 +
    `usb_msc_mount_qemu_test.py` 6/6; all four trees compile. REMAINING: HW test (a real
    drive), stall recovery, multi-sector. [[project_usb_msc]], docs/NEXT_20260615g.
  - **Designs captured** (read-only workflow) for the LED resume + the VL805-downstream hub
    hotplug: docs/NEXT_20260615g + the `usb-next-phase-design` workflow output.
* **PRIOR session -- V3D GPU hardware 3D bring-up (Phases 2 -> 4a), all HW-verified
  on the real RPi4. Seed: `docs/NEXT_20260615b_v3d_phase4_cube.md`. Memories:
  `project_v3d_phase2`, `project_v3d_phase3`, `project_v3d_phase4`,
  `project_v3d_design`:**
  1. **Phase 2 -- GPU clear (first GPU pixels). DONE + HW-verified + PUSHED
     (`920a5d2`, v0.4.250).** A solid orange clear renders to the live HDMI FB via
     the V3D CLE. `/proc/v3d.test` + `fbshow --gpu-clear`; pixel probe PASS. Two
     HW-only fixes: `rb_swap=0` (AIOS BGR FB, clear color fed pre-swapped through the
     Clear Colors packet) + the non-MCS bound-notification wake delivered badge 0
     (drain on any wake).
  2. **Phase 3 -- GPU rainbow triangle (GL Shader State path, the design's
     "highest-risk" phase). DONE + HW-verified (`26ac58b` A / `8cfc66e` B /
     `15ea47f` C, v0.4.251).** Byte-exact host golden-CL gate (8 objects incl. the
     36-B shader record + attr records + tile list); kernel `v3d_submit_triangle`
     reusing the extracted `v3d_run_cls` submit core. Visually confirmed: red BL,
     green BR, blue top -> `rb_swap=1` for the RGBA-shader -> BGR-FB path (DIFFERS
     from the clear). The hardest part was RESCUED from /tmp: the Random06457 (MIT)
     reference is now self-contained in `tools/v3d_ref/` (regenerates every golden).
  3. **Phase 4a -- spinning cube (THE PROJECT DELIVERABLE). DONE + HW-VERIFIED
     (`89a3769` A / `cf1d904` B / 4a-C v0.4.252, build 2385). The V3D deliverable is
     COMPLETE: clear -> triangle -> cube, all GPU-rendered on real V3D silicon.**
     `/proc/v3d.cube.600` ran **600 frames, 0 OUTOMEM, 0 MMU faults, 0 resets,
     status=OK PASS** (~1.1 ms/frame GPU, paced ~60 fps) and Bryan visually confirmed a
     SOLID spinning 3-D cube. It looked inside-out at first; the ROOT CAUSE was ONE
     mis-wound face -- `+Y top` was `{3,2,6,7}` (first-triangle normal pointed INWARD)
     while the other five wound outward, so backface culling showed its far side ("some
     faces correct, some inverted"). Found by a host replica of the transform
     (cross-product . face-centre per face), fixed by reversing it to `{7,6,2,3}` (normal
     +Y, outward) in `src/gpu/v3d_cube.c`. The cull winding is `0x01` (cw=0, CCW-front --
     the `cull=1` branch of CFG_BITS in `src/gpu/v3d_cl.c`), NOT `0x05`: the brief's "flip
     the cw bit" hypothesis was a red herring -- the bug was geometry, not cull direction,
     and flipping cw alone never fixed it. Reuses the triangle pipeline with `cull=1,
     skip_z=1` (convex cube, NO depth buffer); 36 CPU-transformed verts/frame (FP-free).
     New backface-cull diagnostic `/proc/v3d.quad[.N]` -- a flat square drawn twice with
     opposite winding (RED + BLUE); with culling only one colour shows at a time, flipping
     each half-turn. `v3d_submit_cube_frame` is now a thin wrapper over the shared
     `v3d_submit_geom_frame(ax,ay,nverts,quad,res)`. `/proc/v3d.cube[.N]` +
     `fbshow --gpu-cube [N]` + `DISP_V3D_CUBE`.
  Adversarial multi-agent reviews ran before each flash (0 confirmed bugs each).
  Gates every milestone: `python3 scripts/v3d_clcheck.py` (host golden, 8/8 +
  cube-transform sanity), `python3 scripts/v3d_qemu_test.py` (15/15 graceful
  refusal), all four build trees green. **Optional V3D follow-up**: Phase 4b
  (double-buffer via mailbox panning -- tear reduction + the backlogged HDMI
  scroll-perf fix, design sec 8). Single-buffer 4a (tearing accepted) is shipped.
  The PRIOR session's arcs (netd Stage 4 default-ON, the DVFS governor, CPU
  accounting, getty respawn-supervisor, FAT config-over-network -- v0.4.241-244) are
  in the memory index + `docs/NEXT_20260614b_dvfs_cpuacct.md`, not inline here.
* **Target**: AArch64 (qemu-system-aarch64 + Raspberry Pi 4).
* **Host**: macOS Apple Silicon, cross-compile to aarch64-linux-gnu.
* **Developer**: Bryan -- prefers Python patch scripts over sed/heredocs; no
  apostrophes in C comments (zsh copy-paste breaks); commits via GitHub Desktop
  (commit only when asked; never amend / force-push / skip hooks).

---

## Workflow discipline -- READ THIS FIRST

The project goal is **"deploy over the network, flash only for major milestones."**
Honor it; this session learned the hard way what happens when you do not.

* **Develop + verify on QEMU.** The QEMU net harness NATs UDP, so even SNTP works.
  Smoke: `python3 scripts/netcon_qemu_test.py`; boot command under "Build and boot".
* **Push userspace over the LAN.** `python3 scripts/pi_filexfer.py push <local>
  /bin/<tool> 192.168.0.8`, then run it over netconsole. Reboot the Pi IN PLACE
  (`reboot` over netconsole -> BCM2711 watchdog). NO reflash for userspace apps.
* **Flash only for KERNEL / root-task changes, and only at real milestones.** Batch
  several changes into ONE flash. A userspace-only app does NOT bump `version.h`.
* **When HW debugging needs iteration, use SERIAL -- never flash-iteration.** This
  session burned 4 flashes chasing a netconsole-v2 HW bug because netconsole lives
  on disk AND its wedge killed network access (the one case that breaks in-place
  update). Bail to QEMU / serial early.
* **QEMU cannot model:** RPi4 cache attributes, the VC mailbox, eMMC single-block
  write latency, GENET timing, and the fork/pipe/socket event-loop path. Verify
  those on the Pi -- but via push-over-net + serial, not reflashes.

---

## DONE: netd Stage 3 CUTOVER -- net runs in netd -- HW-VERIFIED v0.4.237-239 (2026-06-13e/f)

The behavioral cutover (`DESIGN_NETD` s3/s8/s9/s10). With `AIOS_NETD=ON` the net
stack -- socket server + TCP/UDP/DHCP + the NIC driver dev half -- runs in the
MMU-isolated `netd` CPIO process; root keeps every allocator-touching duty
(prov) and serves the SAME `net_ep` object so the client ABI is unchanged.
flag-OFF stays byte-identical (the cutover is all `#ifdef AIOS_NETD`/`NETD_BUILD`;
build-04 socket suite 8/8 confirms it). The first real netd boot WORKED on the
first try. Full runway: `docs/NEXT_20260613e_netd_stage3_hw.md` + the
`project_demono_netd` memory.

* **3b boot cutover (`a6b6473`, v0.4.237, PUSHED).** `spawn_netd.c`: `netd_prov()`
  (root-side DMA/IRQ/MAC + retained frame caps; sets `net_hw_present`, runs in
  `boot_net_init` before the banner) + `spawn_netd(net_ep)` (donate `net_ep` +
  ctrl/fault EP + IRQHandler + the unbadged RX ntfn; COPY+MAP the MMIO/DMA frame
  sets into the netd vspace non-cacheable; reserve 24 SaveCaller reply slots;
  cntpct-bounded `DEVD_READY` wait -> publish `net_ep_cap`+`net_available`, else
  degrade-and-continue). `netd.c` real main (argv parse, self-bind ntfn TCB slot
  5, `plat_net_dev_attach`, `plat_net_init`, `DEVD_READY` Send BEFORE DHCP, then
  `net_server_fn`). `boot_services.c` `#ifdef AIOS_NETD` select (else = the
  in-root thread; the cleanup proxy is factored to `start_net_cleanup_proxy` and
  runs on both paths). `net_virtio` `plat_net_dev_attach` adds `slot*0x200`
  (QEMU per-slot base); `net_genet` `dma_init` gets the retry-for-low `<1GB` loop
  (RPi4-only). New `include/aios/netd_ctrl.h` (replaces `netd_skel.h`).
* **3c stats page + 3d crash recovery (`532fccd`, v0.4.238, pending push).** 3c:
  `include/aios/netd_stats.h` -- one cacheable-both single-writer frame netd
  writes each loop iter (`netd_stats_update`); `/proc/net` renders it IPC-free
  (the only hung-netd detector); `serverstats` SRV_NET stops SVC_PING-ing netd --
  it Signals the badge-2 kick (idle netd still beats) + reads the heartbeat
  IPC-free. 3d: the fault listener (in `spawn_netd.c`) zeroes
  `net_ep_cap`/`net_available`, sweeps the 24 reply slots (`CNode_Move` + Send
  `-EIO`), and clears the IRQ. Crash trigger: `cat /proc/netd.crash` ->
  fs-thread `NBSend` `NET_DIAG` -> `net_server` null-derefs (`NETD_BUILD` only).
* **Capacity gate (`bc590ef`, pending push).** `smp_qemu_test.py` vs build-netd:
  clean parallel-pipeline ceiling = **30** (W=32 Cannot fork), matching the
  build-04 ~30 -- netd's CPIO footprint does NOT erode the ceiling on QEMU.
* **HW pass + prov-UMAC fix (`b0a34fc`, v0.4.239, pending push).** The first
  netd-on-real-Pi boot caught a HW-only bug QEMU cannot: in the flag-ON path root
  runs `plat_net_prov`, and `read_mac_from_mailbox` wrote `UMAC_MAC0` with
  `genet_regs` NULL (root does not map GENET at prov) -> fault at `0x80c`. Fixed
  with a runtime `genet_in_prov` flag gating the prov UMAC writes (prov reads MAC
  bytes only; netd programs UMAC after its own SWINIT release). flag-OFF
  unaffected. **Bonus: the prov mailbox read succeeding with the retry-for-low
  <1GB DMA resolved the long-standing `.127` fallback -> the Pi now takes the real
  MAC `.8`** (the C/GENET-MAC backlog -- the tag-buffer bus alias `|0xC0000000`
  needs a <1GB physical addr; see `feedback_genet_umac_swinit`).

**Verified (QEMU, flag-ON):** `netd_qemu_test.py` 10/10 (bring-up + `/proc/net` +
serverstats + the s10 crash demo over SERIAL); socket suite 8/8; ssh 6/6;
no-`--net` -> netd never spawns (zero delta); 30-pipeline ceiling 30. **flag-OFF
parity:** build-04 socket 8/8. All three trees build.

**HW-VERIFIED on a real RPi4 (2026-06-13f, v0.4.239 at 192.168.0.8):** every gate
passed -- netd provisions on real GENET (retry-for-low DMA `phys=0x3880000, <1GB,
6 rejects`; real-MAC mailbox read), the GENET register sequence runs INSIDE netd,
DEVD_READY handshake, **DHCP `.8` with the real MAC**, bidirectional ping,
`/proc/net` heartbeat + socket occupancy, serverstats net `ok` (heartbeat-fed, no
SVC_PING), netconsole + ssh both served by netd, and the **s10 crash demo on real
GENET** (`cat /proc/netd.crash` -> fault contained + reply-sweep woke the parked
caller + IRQ cleared -> root + shell alive, net `dead`; clean `reboot` -> netd
back up clean, no re-crash). The Pi is left healthy at 192.168.0.8 on v0.4.239.
Rollbacks on disk: `kernel8-oncard-v235-backup.img`, `kernel8-flagoff-rollback.img`.

**Forced-degrade gate DONE** (`f4aeda9`, the `AIOS_NETD_TEST_DEGRADE`/
`NETD_TEST_NO_READY` hook): a netd that spawns but never sends DEVD_READY ->
spawn_netd bounded wait times out -> "degrade (network off, boot continues)" ->
login reached. The last open QEMU gate; all QEMU gates now closed.

**Remaining = item 4 only** -- see the dedicated DONE section just below for items 1-3.

---

## DONE: RPi4 DVFS governor + CPU accounting -- v0.4.244 (2026-06-14)

The power-lever arc. **The governor WORKS + is HW-verified (build 2245 on the Pi).**

**DVFS mechanism (works).** `config.txt arm_freq_min=300` (added via a physical SD
mount at `/Volumes/AIOSBOOT` -- AIOS cannot edit the FAT itself; general FAT mount is
backlogged) opened the ARM clock floor. The VC mailbox `SET_CLOCK_RATE` (id 3) now
PINS 300/450/600 (HW-confirmed); before, `arm_freq=600` pinned both ends and every set
clamped to 600. Manual control: `/proc/cpufreq.set.MHZ` (`hw_arm_clock_set` in v3d.c,
DVFS Phase 0 `f12eddc`). The firmware does NOT auto-boost back under load (a 400k dash
loop stayed 21s at 300), so an explicit governor IS required. cntpct is
clock-independent, so lowering the clock never breaks timeouts.

**Governor (WORKING -- `src/cpu_gov.c`, v0.4.244).** A root-main-loop tick samples
`aios_acct_busy_permille` once a second and sets 300 (idle) / 600 (load) via the VC
mailbox; raise-fast, lower-gently (LO/HI permille + idle-tick thresholds, runtime
tunable via `/proc/cpufreq.tune.LO.HI.IT`). Two HW-only bugs (caught over two flashes,
both fixed): (1) **`sets=0` -- never actuated**: it Called the mailbox only on a
transition vs `gov_target` (init 600), but the firmware boots at 300 so `want==target`
forever -> fix actuates against the clock LAST set (`gov_set_mhz`, init 0). (2) the
original `total-minus-background` metric read the **no-WFI idle spin as work** -- seL4
`track_utilisation` books a TCB only at switch-OUT, so the always-running root/tlbi
spinners under-report (their cycles are in the PMCCNTR total, not their TCB; `bmin`
stuck ~442 over a disconnected idle) -> fix is a POSITIVE sum of the event-driven work
servers (pipe/fs/exec/net), which block + read ~0 at idle. HW proof (build 2245):
`gov_dbg sets=5 bmin=0 set=600` under the relay load, idle->300 in the boot trace.
**Cooling CONFIRMED + governor fully verified** (`scripts/gov_cooling.py`, 2026-06-14):
governor-idle ~64C vs forced-600-held ~68.6C (still rising) = a conservative ~4-5C
delta (passive board, idle power not clock-dominated -> modest, true delta larger). The
decisive, observer-effect-immune proof: `gov_dbg sets` increments +2 per 72s sample with
the governor ON (downclock-on-disconnect + boost-on-reconnect) but FREEZES with it OFF,
and +2-exactly proves it HOLDS 300 (no periodic bounce); `bmin=0` throughout. The wedges
during the run were all the ~32s TLBI stall (NOT a mailbox race; the VC mailbox is
lock-serialized) -- the `gov_cooling.py` driver rode them out (fresh conn -> escalating
backoff past 32s -> reboot last resort, never blind pkill). Known gap: a pure-compute-
no-IO loop will not boost (no work-server traffic).

**CPU accounting (works -- `5b09d6f` v0.4.243), built to diagnose the governor.**
`KernelBenchmarks=track_utilisation` (settings-rpi4.cmake + settings.cmake) makes the
seL4 scheduler accumulate cycles + schedules per TCB on every context switch.
`src/cpuacct.c` = a name->TCB registry (root + every long-lived root thread, registered
at spawn via `start_server_thread` + the individual sites) + `/proc/cpuacct`, which
renders cycle DELTAS since the last read (`seL4_BenchmarkGetThreadUtilisation`) + idle
+ an unaccounted remainder. Gate-verified (netd 10/10, socket 8/8, ssh 6/6); HW-flashed
(build 2231). **HOG-HUNT VERDICT: NO single hog.** tlbi_probe (the stall-cure TLBI
keepalive) is only ~11%, EQUAL to root/serverstats/flush; the ~50% core-0 "stall"
confounding the governor is (1) the netconsole RELAY (a connected session = pipe ~42%
+ unaccounted ~32% -- the observer effect, proven) and (2) the collective background
threads. **The two measurement bugs are now RESOLVED (v0.4.244):** (a) the PMCCNTR `%`
total is 32-bit and WRAPS ~7s -> `/proc/cpuacct` is wrap-aware (reads cntpct; past 7s
the pct base falls back to the accounted sum and unaccounted reads n/a). (b) the idle
thread reading 0 is EXPECTED, not a bug: the RPi4 root SPINS (`irq_uart_active` stays 0
on RPi4 -> the main loop takes the `seL4_Yield` branch, never `seL4_Wait`), so the
core-0 idle thread never runs. The earlier "suggests it BLOCKS" read was WRONG -- the
no-WFI spin (and the stall cure that depends on it) stands. The deeper finding driving
the governor redesign: those spinner cycles are mis-attributed (`track_utilisation`
books at switch-out, so an always-running spinner under-reports), which is why the
governor sums the event-driven work servers positively, not idle/background. Seed
`docs/NEXT_20260614b_dvfs_cpuacct.md`, memory `feedback_rpi4_thermal_clock`.

---

## DONE: netd Stage 4 items 1-3 -- re-home device diag + SVC_PING -- HW-VERIFIED v0.4.241 (2026-06-14)

The Stage-4 re-home prep (DESIGN_NETD s6/s9), flag-gated; `AIOS_NETD` default still
OFF. Commit `eeb2785` (ahead of origin). All three items QEMU-verified AND
HW-verified on the real Pi (v0.4.241 deployed flash-free).

* **Item 1 -- `/proc/genet` root-local rewrite, UMAC/MDIO-free.** Under `AIOS_NETD`
  root is prov-only (`genet_regs` NULL) but keeps its own GENET MMIO mapping at
  `dev_genet_vaddr` (from `prealloc_rpi4_devices` -- valid in BOTH paths, so no new
  mapping). `genet_diag_cmd` is `#ifdef AIOS_NETD` -> `genet_diag_readonly()` = a
  READ-ONLY HW view (SYS/EXT/RBUF/INTRL2/RDMA/TDMA ctrl+ring + RX descriptors,
  `.peek.OFF`), NEVER touching UMAC (`0x800-0xFFF`) or MDIO/PHY. Software state +
  MAC + IP render from `/proc/net`. flag-OFF keeps the full active diag (`#else`).
* **Item 2 -- active ops moved to `/bin/netdiag`.** peek/poke/mr/mw/tx/reinit/
  irqon/irqoff/mac off `/proc/genet` into the userland netdiag tool, which Calls
  `net_ep` with `NET_DIAG`(103) + a `NETD_DIAG_*` op (`netd_ctrl.h`). net_server
  (netd) dispatches to a new `plat_net_diag()` HAL (net_genet.c live driver;
  net_virtio.c peek/poke/tx/mac, `-2` for GENET-only ops). The fs thread NEVER
  Calls netd -- netdiag is the sacrificial userland caller. New `aios_net_diag()`
  lib helper + `NET_DIAG_L` mirror.
* **Item 3 -- explicit `SVC_PING` -> 0 reply** in net_server (was falling through to
  the unknown-op `-1`).

**Verified QEMU:** all 4 trees build; NEW `scripts/netdiag_qemu_test.py` 6/6
(peek=virtio magic, mac round-trip, mdio `-2`, tx); `netd_qemu_test` 10/10 (crash
demo intact -- the NET_DIAG edit shares that path); `net_socket` 8/8 flag-ON +
flag-OFF; `ssh` 6/6. **HW-VERIFIED on real GENET (v0.4.241, 2026-06-14):**
`cat /proc/genet` = the read-only view (`SYS rev=06000000`, `EXT oob=f10050`,
RDMA/TDMA `prod==cons`, RX descriptors, no UMAC); netdiag on the live device --
`mr 1 2`=`600d` + `mr 1 3`=`84a2` (live MDIO reads the BCM54213 PHYID!),
`mr 1 1`=`794d` (link up), `mac`=`dc:a6:32:1c:2e:e1`, `peek 0`=`06000000`, `tx`
ret 0; ping 0%, netd heartbeat advanced (no crash). The new netdiag ELF was PUSHED
to the Pi `/bin/netdiag` (`pi_filexfer`) since the kernel swap does not update disk
apps -- the durable copy lands on the next SD rebuild.

**Remaining = item 4 (the FINAL Stage-4 step):** flip `AIOS_NETD` default ON both
targets (CMake `option` OFF->ON) + re-run the full gate suite (note the OFF/ON test
matrix then needs an explicit `-DAIOS_NETD=OFF` tree); after one stable release
delete the in-root net path + retire the flag. The behavioral + HW work of Stage 4
is DONE; only the default flip + eventual in-root deletion remain. Seed:
`docs/NEXT_20260614_netd_stage4_flip.md`.

---

## DONE: netd Stage 3 FOUNDATION + handoff plumbing -- v0.4.236 (2026-06-13b)

The compile/link/gating foundation for the Stage-3 net cutover (`DESIGN_NETD` s9),
in 5 reviewable, **flag-OFF-inert** commits (`ec20d24`..`7cbee78`). Flag-OFF QEMU
socket suite 8/8 + flag-ON `netd_qemu_test` 9/9 at EVERY step; both trees build;
net_genet runtime is HW-deferred (QEMU has no GENET). **Only the behavioral cutover
(3b/3c/3d) remains.** Full file-level runway: the seed
`docs/NEXT_20260613d_netd_stage3_cutover.md` + the `project_demono_netd` memory.

* **v0.4.236 (`ec20d24`)** -- reverted the FAILED GENET MAC-read fixes (v0.4.234
  retry + v0.4.235 deferred re-read): they burned ~14s of boot polling and never
  worked on HW (root cause backlogged in `BACKLOG.md`). Clears the ground for the
  prov/dev refactor.
* **1/n (`815659b`) + 2/n (`4f9b84c`) -- driver prov/dev split.** Two compile
  defines: prov half `#ifndef NETD_BUILD` (slot resolve / DMA alloc / IRQ bind,
  extracted into helpers CALLED IN PLACE so the monolithic order is byte-identical),
  dev half `#ifndef NETD_PROV`. New `plat_net_prov()` (root) + `plat_net_dev_attach()`
  (netd latches MMIO/DMA/IRQ/MAC from argv). net_genet got the STRIPPED prov MAC
  query (UMAC writes gated `#ifndef NETD_PROV` -- the v0.4.151 SWINIT-halt fix); its
  HW-verified register sequence is byte-identical (`git diff --ignore-all-space`).
* **3/n (`e3a5418`) -- net stack compiles + LINKS into netd (`NETD_BUILD`).**
  net_server.c + src/net/*.c + the platform driver dev half link into the netd
  binary on BOTH platforms -- the isolation proof (a leaked root-only symbol = link
  error; netd references no `vka`). net_server.c SaveCaller cnode is a
  `NET_REPLY_CNODE` macro; new `src/apps/netd_shim.c` defines the root-owned net
  globals for netd. CMake adds the net stack to the netd target, built
  unconditionally so every build compile+link-checks the path. netd `main` is STILL
  the Stage-2 skeleton (the cutover swaps it).
* **4/n (`7cbee78`) -- prov handoff plumbing.** `include/aios/netd_handoff.h`
  (`driver_handoff_t`); `plat_net_prov(driver_handoff_t*)` fills it; frame caps
  RETAINED (probe `vio_frame_caps[4]`, boot_device_map `dev_genet_frame_caps[16]`,
  the 32 DMA caps per driver). Still uncalled = inert.

**Refinements vs DESIGN_NETD:** `NETD_PROV` is left UNUSED -- root keeps the full
in-root driver for the flag-OFF path; the `#ifndef NETD_PROV` guards stay dormant,
available for a Stage-4 prov-only root. retry-for-low DMA is a marked TODO in
net_genet `dma_init`, DEFERRED to the cutover/HW pass (HW-only-verifiable). version
stays v0.4.236 across the flag-OFF-inert sub-commits.

**Remaining = the cutover (one ATOMIC arc; first real netd boot needs build-netd
debugging):** 3b spawn cap-handoff (copy+map the MMIO/DMA frame sets into netd, 24
reserved SaveCaller slots, the s3 argv protocol) + netd real `main` + the s8 READY
handshake + boot_services `#ifdef AIOS_NETD` select; 3c stats page + `/proc/net`
(serverstats reads it, no SVC_PING); 3d fault-listener reply-sweep [PROVEN in
Stage 2]. Then the Step-4 HW pass.

---

## DONE: netd de-monolithization Stages 0-2 -- v0.4.229-231 (2026-06-13)

Moving the net subsystem out of the root task into an isolated `netd` process
(`docs/DESIGN_NETD.md`, staged 0-4). Stage-2 seed (now fulfilled):
`docs/NEXT_20260613b_netd_stage2.md`. **Next-session seed (state + thread menu):
`docs/NEXT_20260613c_handoff.md`.** The big next thread is Stage 3 (the real net
cutover); the contained follow-ons are C (GENET MAC fix), DVFS, and sshd-22.

* **Stage 0** v0.4.229 (`000203a`, PUSHED) -- in-root hardening, zero behaviour
  change: `include/aios/net_proto.h` (centralized NET IPC labels 90-103 +
  `_Static_assert` against the posix `_L` mirror); the RST/close reply-slot
  poisoning fix in net_server.c (`net_sock_wake_reset`/`net_sock_drop_parked`/
  `net_park_caller` -- delete-first + rc-check at all SaveCaller sites);
  cleanup-proxy (64-entry SPSC pid ring + sacrificial proxy thread in
  pipe_server, so a wedged net server cannot freeze process management);
  serverstats net row enabled=1 + `dead` state; connect() lazy-PID-resolve; new
  `src/apps/nettest.c` + `scripts/net_socket_qemu_test.py`. QEMU socket suite
  8/8 (incl a 200KB bulk-RX burst) + ssh 6/6.
* **Stage 1** v0.4.230 (`1616e0f`, ahead-1, ASK before pushing) -- merged the
  dedicated RX driver thread into net_server: `plat_net_drain()` (new in
  `src/plat/net_hal.h`, per-platform in net_virtio.c + net_genet.c) at the
  loop top + in dhcp_poll_rx; the RX IRQ notification is BOUND to the net_server
  TCB; NAPI re-check folded into the drain; badge=1 IRQ / badge=2 kick mints;
  net_srv_ntfn + the driver thread + its /proc row removed. QEMU 8/8 + ssh 6/6,
  and **HW-VERIFIED on the real RPi4** (build 2150: DHCP, GENET IRQ-RX climbing,
  ping 40/40 0% loss, netconsole). GENET register sequences unchanged.
* **Stage 2 (skeleton process) DONE** v0.4.231 -- the netd SKELETON: an
  MMU-isolated CPIO process (`src/apps/netd.c`) spawned by a custom
  `src/boot/spawn_netd.c` (own cnode/vspace, prio 200, fault EP = a dedicated
  ctrl EP; `include/aios/netd_skel.h` is the shared throwaway protocol). Proves
  the leaf-driver mechanism before any net code moves: argv-cap parse, ntfn
  self-bind, DEVD_READY handshake to a dedicated root fault-listener thread,
  deferred replies via child-cnode SaveCaller (reserved slots via
  `cspace_next_free`), and crash containment. **THE KERNEL BET (DESIGN_NETD s10
  reply-sweep) IS PROVEN:** on non-MCS seL4, root CAN `seL4_CNode_Move` a
  netd-saved reply cap out of netd's cnode into a root slot and `seL4_Send` it to
  WAKE the parked caller (verified end-to-end, token 0xd00d) -- so the
  crash-recovery sweep is viable, and Stage 3 can proceed on that footing.
  Everything is gated behind `option(AIOS_NETD OFF)`: flag OFF = behavioral
  parity (CPIO + boot path unchanged; spawn_netd.c compiles to nothing). New
  `scripts/netd_qemu_test.py` 9/9 (in-netd self-wake, reply-sweep, NETD_CRASH ->
  fault listener -> shell/fs/pipe/net still serve). `/bin/netdiag` shipped (a
  socket-liveness probe today; the Stage-3 NET_DIAG home). VERIFIED: build-04 +
  build-rpi4 build flag-OFF; flag-ON `build-netd` 9/9; flag-OFF parity ssh 6/6 +
  net_socket 8/8; netdiag REACHABLE. (netcon_qemu_test flaked on its serial-login
  prompt under boot-log spam -- a PRE-EXISTING fragility, flag-OFF-inert;
  netconsole itself is proven by net_socket's 8/8. The spawn page-cost counter
  reads delta 0 because a sel4utils CPIO spawn allocates via its own vspace path,
  invisible to vka_live_frames -- capacity is the >=30-pipeline gate, deferred.)
  Committed `7cf262e` (pushed to origin).
* **Lesson:** an "ext2 image-builder bug" this session was a MISDIAGNOSIS -- a
  one-time concurrent write to `disk/disk_ext2.img` from another session. The
  builder is deterministic. See `feedback_qemu_test_hygiene`.

---

## DONE: stability follow-ups + RPi4 thermal cap -- v0.4.232-235 (2026-06-13)

Wins after netd Stage 2 (each its own commit). **A, B, and the heat cap are
HW-VERIFIED on the real Pi; C is NOT fixed (two attempts failed) and is
BACKLOGGED.** Push state: A/B (`e1aaf75`/`fc68cc2`) + the C-attempt (`b2e7a58`) +
the docs commit (`6f241d0`) are on origin; v0.4.235 (`706a820`) + heat cap
(`60f7fb1`) + the C backlog (`ad7c438`) are **ahead-3 (pending push)**.

* **A -- quiet per-spawn serial logging** (v0.4.232, `e1aaf75`). **HW-VERIFIED.**
  Every spawn/exec/fork printed ~4-5 routine stat lines at INFO (text
  cached/shared/lazy pages, BSS lazy pages, cow_setup, the per-exec elf size); on
  the lossy mini-UART that buries real `[WRN]`/`[ERR]`. Demoted to
  `AIOS_LOG_DEBUG` (gated off at INFO; live counts stay in /proc/vka + /proc/cow,
  exec paths in /proc/filehits). A real-Pi boot capture shows **0 per-spawn spam
  lines**. **If you miss those lines, set that module's `LOG_LEVEL` to DEBUG.**
  (Did NOT fix `netcon_qemu_test`'s serial-login flake -- a separate pre-existing
  harness issue; net_socket drives over netconsole-TCP to avoid serial login.)

* **B -- DHCP lease renewal** (v0.4.233, `fc68cc2`). **HW-VERIFIED (lease parse).**
  AIOS bound once at boot and never renewed -> an idle Pi dropped off the LAN at
  expiry. Now parses the lease (option 51) + renews at T1 (50%) from the
  net_server loop (woken >= every 5s by the serverstats SVC_PING, so no timer
  thread). Renewal REQUEST = the proven boot packet; `net_dhcp_input` gained a
  `dhcp_renewing` ACK path (extends + re-arms T1, bounded retry lease/8).
  /proc/netstat shows dhcp_acks/dhcp_renews/dhcp_lease_secs; `cat
  /proc/netstat.renew` forces one. VERIFIED: `dhcp_renew_qemu_test.py` 3/3 + a
  real-Pi boot log `lease=86400s` from the router. (Renewal round-trip on real
  GENET is still QEMU-only-proven -- a long-uptime soak would confirm it.)
  Follow-up: non-blocking re-acquire on a renewal NAK.

* **Heat cap -- RPi4 `arm_freq=600`** (`60f7fb1`). **HW-VERIFIED stable.** The Pi
  ran hot because AIOS idle-SPINS all 4 cores (no WFI -- the v0.4.228 stall cure
  needs the SCU clocked), so the firmware pins the A72 at its 1500MHz max doing no
  useful work. Cap arm_freq in the generated config.txt (mksdcard.py, tunable
  `ARM_FREQ_CAP_MHZ`): the cores still spin (stall-safe) but at a lower clock +
  voltage. v0.4.235 boots **STABLE + stall-free at 600MHz** (regular 30s tlbi
  heartbeat, no 32s gap). No on-chip temp readout yet to quantify it. Follow-up
  (BACKLOG): load-driven DVFS governor (idle=low clock, never WFI) + a /proc/temp
  readout (mailbox GET_TEMPERATURE).

* **C -- GENET real-MAC read -- FAILED, BACKLOGGED (harmless).** Goal: the Pi
  takes lease `.8` (real MAC) not `.127` (fake fallback dc:a6:32:01:02:03). TWO
  attempts FAILED on real HW: v0.4.234 (`b2e7a58`, retry 3x) + v0.4.235
  (`706a820`, deferred re-read before DHCP). DECISIVE: a fully-settled post-boot
  `cat /proc/genet.mac` returns `ret=-1`, so the mailbox read fails EVERY time --
  NOT a boot-timing race -- yet display_vc's mbox_call to the SAME mailbox
  succeeds. net_genet's CALL is broken (prime suspect: tag-buffer
  region/coherency vs display_vc's pinned-low 0x3A000000). HARMLESS -- .127 works.
  Full diagnosis + real-fix plan + the **~14s-boot-latency cleanup owed (revert
  both attempts)** are in **BACKLOG.md "GENET real-MAC read fails"**.

---

## DONE: USB HID follow-ups + scroll diagnostic -- v0.4.186 (2026-06-10)

Four additive follow-ups on the standalone USB keyboard (`docs/NEXT_20260609_usb_followups.md`),
all on the shared tree, both trees build, QEMU suite 9/9. Default HW behaviour unchanged
(polling, single keyboard). Full handover + open items: **`docs/NEXT_20260610_usb_followups_status.md`**.

- **Lock LEDs** (Task 1) -- **HW-VERIFIED** (Num+Caps lights work on the Pi). Made event
  consumption endpoint-aware (`evt_dispatch` routes interrupt-IN reports by slot+ep), fixed a
  latent EP0-ring wrap bug, added STALL recovery. `/proc/xhci` live diagnostic + `.led.N` poke.
- **Ctrl modifier** -- Left Ctrl + C now sends 0x03 (was plain 'c'); `ctrl_char()` folds
  Ctrl+letter/`@[\]^_?` to control codes. QEMU-verified; HW confirm pending.
- **Multi-device** (Task 3) -- single-device globals -> `struct usb_dev g_devs[8]`; enumerate
  all hub + root ports; dispatch by slot+endpoint. QEMU kbd+mouse verified; HW = single kbd.
- **Mouse** (Task 4) -- boot-report decode + `/proc/mouse` consumer. QEMU-verified.
- **IRQ-driven xHCI** (Task 2) -- opt-in `/proc/xhci.irq.1`, default polling. QEMU INTx path
  verified end-to-end (GIC IRQ 37). RPi4 brcmstb MSI is HW-pending (`plat_pcie_xhci_irq()`
  returns -1 -> stays polling, safe).

**OPEN (priority): the HDMI console FREEZES on the first scroll on real HW** -- the cacheable
framebuffer scroll (3 MB memmove + per-page clean) wedges `display_server`, cascading through
`tty_server` to the USB driver (keyboard dead, screen frozen, net still pings). QEMU (ramfb)
runs the same `fb_console.c` scroll fine (182 scrolls), so it is HW-cacheable-specific, NOT a
logic bug, and PRE-EXISTING (unrelated to the USB code -- the keyboard just made it reachable).
Diagnostic shipped: `cat /proc/fbcon` over netconsole after a freeze pinpoints the phase
(memmove vs which flush page vs IPC). See the NEXT doc for the repro + fix plan.

---

## DONE: RPi4 4-core SMP -- HW-VERIFIED v0.4.179 (2026-06-07)

4-core SMP works on real hardware. `settings-rpi4.cmake KernelMaxNumNodes=4`: the
elfloader spin-table brings up all 3 secondaries (`Boot cpu id = 0x0` ->
`Core 1/2/3 is up with logic id N`), the kernel bootstraps 4-core SMP, AIOS boots,
DHCP 192.168.0.8, ping 0% loss; `/proc/hw` cores=4, `/proc/version` "4-core SMP".

The long-blamed `smp_boot.c:119` "hang" was NEVER an SMP bug -- it was invisible
and conflated with an unrelated boot break. Two causes, now fixed:
1. The elfloader had **no registered console**: `bcm-uart.c bcm2835_uart_init`
   skipped `uart_set_out(dev)`, so `plat_console_putchar` was NULL and every
   elfloader printf no-op'd. Removing the `common.c` printf gate was necessary
   but not sufficient.
2. The v0.4.178 `dtoverlay=disable-bt` "make-it-visible-on-PL011" attempt only
   DISCONNECTED the mini-UART console from the cable (the elfloader console is
   serial1=mini-UART per the build-time DTB) AND broke the kernel boot (root task
   drives the mini-UART at 0xFE215000) -> Pi unreachable, looking like a SMP hang.

**Fix (committed v0.4.179):** the TRACKED commit is `settings-rpi4.cmake`
(MAX_NUM_NODES=4) + `version.h` 0.4.179 + `mksdcard.py`/`hw/rpi4/config.txt`
reverted to the known-good mini-UART config (no disable-bt, `core_freq=250`,
115200) + docs. The elfloader fix lives in **gitignored `deps/`** (re-apply if a
deps reset wipes them): `bcm-uart.c` now calls `uart_set_out(dev)` (registers the
mini-UART console; putchar already bounded `for t<100000`), `common.c` printf gate
removed, `pl011-uart.c` bounded putchar (now unused -- the mini-UART is the
console). Capture the trace at 115200: `scripts/aios_console.py monitor
/dev/cu.usbserial-0001`. Details: `project_rpi4_smp` memory +
`docs/NEXT_20260607_rpi4_smp.md`.

---

## DONE: process capacity + ELF demand-text -- v0.4.180-182 (2026-06-07)

Built on top of SMP, same session. **The Pi now runs v0.4.182** (HW-verified;
currently at 192.168.0.127 -- see the MAC note), 4-core SMP, demand-text working.

- **v0.4.180** -- parallel-pipeline ceiling 6 -> 22: `MAX_ACTIVE_PROCS` 16 -> 48
  (+ `MAX_PIPES`/`MAX_ZOMBIES`/`PROC_MAX`/`MAX_WAIT_PENDING`, all coupled). New
  regression harness `scripts/smp_qemu_test.py` (boots SMP-4 QEMU, drives over
  netconsole to dodge the sntp serial-login spam, fork-width probe + race tests).
  The feared "BSS-shift hazard" was a myth.
- **v0.4.181** -- ELF DEMAND-TEXT: each disk-exec'd proc keeps resident only the
  code it executes, not the whole statically-linked binary (seq=455 KB=114 pages).
  `pipe_server.c setup_demand_text` unmaps the read-only (R+X) ELF segment +
  registers a file_vma, so the first instruction fetch pages it in from the
  executable via the v0.4.146 file-fault engine. Table raised 48 -> 64 (ceiling
  ~30). morecore was ALREADY demand-paged (`BSS lazy pages=1580`); do NOT re-fix.
- **v0.4.182** -- I-CACHE FIX (HW-critical): v0.4.181 booted on QEMU but killed
  every disk-exec'd proc on the real A72 -- demand-text loaded code via DATA writes
  without invalidating the I-cache -> stale instructions -> crash. Added
  `seL4_ARM_Page_Unify_Instruction` (both the write + the exec mapping) in
  `handle_file_mmap_fault`. HW-VERIFIED: netconsole + sshd come up, pipelines
  correct. **LESSON: any path that loads code via data writes (demand-text, JIT,
  COW-of-text) MUST Unify_Instruction; QEMU's a53 model cannot catch a missing
  one.** Full detail: the `project_proc_capacity` memory.

**Pre-existing flake (NOT this work):** the Pi sometimes takes the GENET fallback
MAC `dc:a6:32:01:02:03` (mailbox MAC read intermittently fails) -> DHCP gives
**.127 instead of .8**. Harmless -- the Pi works either way; check both IPs. Small
future item (net_genet mailbox MAC retry).

## DONE: Phase 2 shared `.text` (HW-verified) + USB HID stack (QEMU) -- v0.4.183 (2026-06-08)

**Phase 2 shared read-only `.text` -- HW-VERIFIED.** Root keeps a per-boot
`{binary -> RO .text frames}` cache (`setup_shared_text`/`clear_shared_text` in
pipe_server.c): ONE ~75-page copy for N same-binary procs instead of N. Flashed +
verified on the Pi -- netconsole/sshd come up, pipelines run, /proc/version
v0.4.183 (the I-cache fill-unify reasoning holds on the A72). QEMU smp test 7/7.
Bug fixed in-session: cookie=NULL shared pages MUST be explicitly unmapped before
destroy (sel4utils `free_page` no-ops them) or each leaks a root cslot per proc ->
"Cannot fork" under storms; mirrors `cow_release_proc`. See `project_proc_capacity`.

**USB HID keyboard (HCI) -- A/B/C QEMU-complete; D.1 (PCIe + VL805 detection)
COMPLETE ON REAL HW.** A USB keyboard types into the AIOS shell on QEMU: PCIe ECAM ->
xHCI -> enumeration -> HID -> keymap -> SER_KEY_PUSH (Phases A
`src/plat/qemu-virt/pcie_ecam.c`, B `src/usb/xhci.c`, C enum+HID+driver --
QEMU-verified, `scripts/xhci_key_qemu_test.py` PASS). Layers 2-5 are platform-
independent; only Layer 1 (PCIe) differs.

**Phase D.1 (RPi4 real HW), 2026-06-08 -- DONE.** The brcmstb PCIe link trains AND
the VL805 xHCI ENUMERATES on real hardware:
`[pcie] bus1 dev0: VID=1106 PID=3483 class=0c0330` + `VL805 xHCI DETECTED`. Seven
bugs fixed across ~16 HW boots: reset-ordering SError (NOT a power-gate -- the early
theory was wrong), PERST polarity inverted, CRS retry, NOTIFY-is-a-reset-not-a-loader,
outbound MEM window, SSC via the internal MDIO bus (link now 5.0 Gbps x1, matches
U-Boot), and **the final fix: RC bridge BUS-NUMBER forwarding** (set RC sec=1/sub=1 at
base+0x18 so the RC forwards config to bus 1 -- U-Boot's generic PCI core does this,
the driver probe does not). Proved the chip works by running the user's local U-Boot
(`../u-boot`, prebuilt `u-boot.bin`: `Bus xhci_pci: 2 USB Device(s) found`), then made
our driver match. `src/plat/rpi4/pcie_brcmstb.c`; src default PROBE_LEVEL=0 (safe, no
controller MMIO); `disk/kernel8.img` = PROBE_LEVEL=1 (detects). Flash-free kernel
updates: `scripts/mkkernel8.py` ([[feedback_flashfree_kernel]]). Full saga + all 7
fixes: **`docs/NEXT_20260608_usb_phase_d.md` "FINAL STATUS"** + `project_usb_hid`
memory. Uncommitted; version stays 0.4.183 (bump at the keyboard, D.2).

### Next step (USB HID) -- D.2: the actual keyboard
D.1 (detection) is done. For a key to type: in `pcie_bringup_and_detect` program the
VL805 BAR0 in the PCI window + set `pcie_xhci_present`; then the seL4 bcm2711 kernel
change to expose the PCIe outbound window >4GB (the BAR is at CPU 0x6_00000000, above
the 4GB device-untyped top) so `xhci_init()` (`src/usb/xhci.c`, Layers 2-5, already
QEMU-verified) can map it; the existing polling driver feeds keys via SER_KEY_PUSH.
Then bump version.h -> 0.4.184 + commit. Steps in `docs/NEXT_20260608_usb_phase_d.md`
"REMAINING -> D.2". The brcmstb path is HW-only (QEMU has no brcmstb).

## Where we left off (v0.4.178 -> SSH hardened: reconnect + self-heal + scp/sftp)

SSH now survives unlimited sequential connections per boot, self-heals a hung
shell, and supports file transfer (`sftp` + modern `scp`). All committed
(`654d722`, `ab27f84`, `8df0a58`) and the sshd is deployed to the Pi (push, no
flash). `scripts/ssh_qemu_reconnect.py` (QEMU -smp 4) = **6/6 PASS**. See the
`project_ssh_recovered` memory.

* **scp / sftp (commit `8df0a58`, `src/ssh/ssh_sftp.c`).** Minimal SFTP v3
  subsystem inside sshd (ls/get/put/mkdir/rm/mv...). Does fs<->socket I/O
  directly, NOT through the shell pipe, so it dodges the A72 drain race. QEMU:
  sftp+scp byte-verified rc=0. Pi: sftp rc=0 byte-verified; scp transfers
  byte-verified but rc=1 on HW (the deferred drain race drops the final
  exit-status packet -- transfer is correct). Also fixed `pwrite64`/`pread64`
  in libaios_posix: pwrite64 capped at 3000 B and packed all into MRs ->
  seL4_MessageInfo_new(>127) HALTED the system on any >1 KB pwrite, and ignored
  the offset; pread64 only worked for <=4 KB cached files. Both now chunk via
  fetch_pwrite/pread + honor offset.
* **Relay self-heal (commit `8df0a58`).** On client disconnect the shell relay
  SIGKILLs the child before waitpid instead of blocking forever -- a hung shell
  (e.g. `zsh` over the cooked PTY relay) can no longer wedge the
  one-connection-at-a-time sshd for all future clients. Verified on QEMU (0.8s
  recovery) + Pi.

The reconnect fix itself (the original v0.4.178 work) was **two userspace leak
fixes, NOT the fork-free spawn** the plan called for -- detail below.

* **The fork was the THIRD wrong "proven" theory** (after COW and the SMP race). I
  implemented the full fork-free spawn from `docs/NEXT_20260606b_forkfree_ssh.md`
  (PIPE_SPAWN_PIPED + aios_spawn_piped) and the reconnect test STILL failed identically.
  So I reverted it entirely (kept the standard fork+exec) and root-caused the real bug.
* **Real cause = two pre-existing leaks** (both common to fork & fork-free; found via a
  verbose serial capture showing conn2 fails at the SOCKET layer, not crypto):
  1. **O_NONBLOCK fd-slot leak** -- `channel_relay` leaves the socket non-blocking and
     `aios_fd_alloc` did not zero reused fd slots, so conn2's socket inherited
     `is_nonblock=1` and `sock_read_exact` got EAGAIN (a fatal read error). FIX:
     `aios_fd_alloc` memsets the slot (`src/lib/aios_posix.c`) -- helps every app.
  2. **Auth session leak** -- sshd took an auth_server session per login (4-slot table)
     but never released it. FIX: sshd calls `AUTH_LOGOUT` on disconnect
     (`src/ssh/ssh_auth.c` + `ssh_session.h` + `sshd_main.c`).
* **Userspace-only fix -> NO FLASH needed.** Deploy by `pi_filexfer.py push
  build-04/sbase/sshd /bin/sshd 192.168.0.8` + `reboot` over netconsole. The same
  aarch64 sshd runs on QEMU and the Pi; no `build-rpi4`, no reflash. The committed
  affinity pin (889caa1) is NOT needed for this -- defer it to a future flash milestone.

### Prior SSH work (v0.4.177, still valid) -- recovered, always-on
SSH was RECOVERED + made always-on earlier this arc. Six commits on main
(`f0078b4`, `ddb80cb`, `0b21761`, `2cc5c01`, `889caa1`, `60c234b`).

* **SSH recovered** (`f0078b4`). The only blocker was the lost (gitignored)
  `build-04/libmbedcrypto.a`; the SSH server (`src/ssh/`) had ZERO drift v0.4.84->177.
  NEW `scripts/build_mbedtls.py` rebuilds it (mbedTLS v3.6.3 `src_crypto`, 82 obj,
  ~1.2 MB, ~1.3s; idempotent config verify/apply). `scripts/build_apps.py` now also
  links sshd + `test_mbedtls` (same clean-build gap psutil/nslookup had). Verified:
  QEMU `scripts/ssh_qemu_test.py` 5/5 + real Pi (full KEX + AES-256-CTR/HMAC + password
  auth + interactive shell on the A72).
* **A72 relay EOF fix** (`0b21761`). The shell relay closed its pipe write end AFTER
  first output (pre-v0.4.143 model); on the A72 that raced dash output -> spurious EOF
  -> channel torn down after ONE command. Now closes at fork (netconsole pattern).
  LESSON: test the PTY path (`ssh -tt`), not just `-T`.
* **Always-on sshd** (`ddb80cb`). getty fork+execs `/bin/sshd` at boot (fd1=tty);
  deployed to the Pi by push+reboot over netconsole (getty is a disk ELF -- NO reflash).
  sshd is verbose, so its ~140 diagnostics are gated behind `sshd -v` (default OFF) or
  they garble the shared login console.
* **SMP allocator-race hardening** (`889caa1`). Root servers share a lock-free
  allocman/vka with no lock; pinned all root threads to core 0. Latent SMP fix -- it is
  NOT the reconnect bug.
* **reconnect -- RESOLVED v0.4.178 (see the top section).** Was NOT the fork. Two
  userspace leaks (O_NONBLOCK fd-slot reuse + auth session not released). 6/6 on
  `scripts/ssh_qemu_reconnect.py`. Uncommitted; not yet pushed to the Pi.

**Current state (SSH):** the Pi runs **v0.4.176** + a pushed one-session/boot sshd at
192.168.0.8. The repo working tree has the **v0.4.178 reconnect fix** (4 files,
uncommitted): `src/lib/aios_posix.c`, `src/ssh/ssh_auth.c`, `src/ssh/ssh_session.h`,
`src/ssh/sshd_main.c`, plus the new `scripts/ssh_qemu_reconnect.py`. To deploy the fix:
rebuild apps (`python3 scripts/build_apps.py`), then push the new sshd over netconsole
+ reboot -- no flash. Log in: `ssh -p 2222 -o StrictHostKeyChecking=no
-o UserKnownHostsFile=/dev/null root@192.168.0.8` (password `root`).

## Where we left off (v0.4.176 -> v0.4.177) -- tools, DNS, Tier-1 hardening

Tools + a hardening sweep + the first network resolver. All committed.

* **pidof / pkill / killall** (psutil, `cd46048`). One source dispatched by argv[0];
  pure userspace -- reads `/proc/status`, signals via `kill(2)`. QEMU 7/7 + HW-verified
  (pkill killed a live netconsole2). `kill()` only reaches REGULAR procs (in
  `active_procs`); boot SERVERS appear in /proc/status but `kill()` returns ESRCH (they
  are root-task threads) -- the tool honestly reports "FAILED on <pid>".
* **build_apps fix** (`48ec84f`). `scripts/build_apps.py` (the full orchestrator) was
  SKIPPING the aios-cc apps -- netconsole, netconsole2, psutil, nslookup -- which are
  NOT in `projects/aios/CMakeLists.txt`, so a clean `rm -rf build-04` dropped them.
  Now it builds them before mkdisk.
* **Tier-1 driver hardening** (v0.4.177, `bbc4dc5`, QEMU-verified). A sweep found the
  v0.4.176 eMMC iteration-count-timeout anti-pattern in 4 more drivers. 8 poll loops
  time-bounded via a NEW shared `include/aios/mono_wait.h` (cntpct_el0; one-line
  for-header swap). `display_vc.c` (VC mailbox, HDMI), `net_genet.c` (MDIO + mailbox
  MAC read), `blk_virtio.c`, `display_ramfb.c`. display_vc + net_genet are RPi4-only ->
  a flash confirms HDMI + GENET still init (happy path unchanged). See the
  `emmc-completion-timeout-hw` memory.
* **DNS resolver** (`f27bb45`, HW-verified). `src/apps/nslookup.c` -- UDP A-record query,
  mirrors sntp.c. `nslookup <host> [server]`, default 8.8.8.8. QEMU (SLIRP DNS + public
  via NAT) AND the real Pi (8.8.8.8 + gateway 192.168.0.1) both resolve. Follow-ons:
  capture the DHCP DNS server (net_dhcp option 6 + expose via /proc) for a LOCAL default,
  and wire `gethostbyname()`/`getaddrinfo()` into libc so `ssh user@host` resolves.
* **Infra survey.** SSH server is fully written but BLOCKED on building `libmbedcrypto.a`
  (mbedTLS source present, no build script; the cross-build has real gaps -- arm_neon.h,
  platform config). A dedicated effort -- seed in `docs/NEXT_*ssh*`. RPi4 SMP is DISABLED
  (MAX_NUM_NODES=1); enabling it hangs in the elfloader secondary-core release (spin-table,
  v0.4.135) -- HW-gated, defer. Bluetooth/HCI design-only, low priority.

**Current state:** the Pi runs **v0.4.176** on the LAN at 192.168.0.8 (v1 netconsole on
2323). The repo is at **v0.4.177**; `disk/sdcard-rpi4.img` is STAGED with v0.4.177
(Tier-1 hardening) + the new tools (psutil, nslookup) -- flash it to verify HDMI/GENET
and land the tools on disk. After flashing, the Pi is at v0.4.177.

## Where we left off (v0.4.175 -> v0.4.176) -- netconsole2 + the eMMC stall RESOLVED

The big result: the HW "relay stall" that killed the reverted netconsole v2 AND stalled the
v0.4.175 netconsole2 is now understood and FIXED -- it was never a relay bug, it was the RPi4
eMMC driver. Full lesson: `emmc-completion-timeout-hw` memory.

* **netconsole2 debug sibling (v0.4.175, `f8a92cc`).** Retry of the reverted v2 as a SEPARATE
  binary on port 2324 (v1 keeps 2323 as the reliable deploy channel) with a serial-INDEPENDENT
  trace to `/tmp/nc2.trace` (pulled over the v1 channel) -- the instrument that cracked the bug.
  Plus a length-guarded non-blocking accept in `net_server.c` (old 1-MR callers stay blocking).
  QEMU smoke 9/9 (`scripts/nc2_qemu_test.py`).
* **eMMC completion timeout = the root cause (v0.4.176, `eff80dc`, HW-VERIFIED).** `blk_emmc.c`
  polled every completion with a fixed `for (t < 10000000)` INT_STATUS loop. That is an
  iteration count, not a timeout: on the A72 a MISSED status bit (normal for polled SDHCI)
  burned all 10M MMIO reads ~= 32.6s before proceeding (QEMU finished instantly). A write-back
  CMD25 flush that hit it stalled the whole block layer 32.6s. The netconsole2 trace showed the
  EXACT, repeated 32632ms gap = the 10M count. Fix: time-based waits (`cntpct_el0`,
  `emmc_wait_int` + `emmc_deadline`, EMMC_CMD_MS 1s / EMMC_DATA_MS 2s; a missed bit now costs
  <=2s). HW: netconsole2 commands ~0.5s, ZERO 32s stalls. LESSON: HW poll loops need real-time
  bounds, never iteration counts.
* **netconsole2 relay works on HW.** With the eMMC fix the fork/pipe/socket relay runs fast and
  correct -- the reverted v2 was a victim of the eMMC stall, not broken. OPEN: a robust launch.
  Launching `netconsole2 >FILE 2>&1` LEAKS the child output to the file (AIOS `dup2` does not
  re-route fd1 file->pipe; child `dup2(pipe,1)` no-ops the routing). The relay is fine with fd1 =
  a PIPE (no-redirect launch, proven) or a TTY -- so **getty auto-start (fd1=tty, like the
  working v1) is the clean deploy path**; that wiring is the next netconsole2 step. Do NOT
  daemonize netconsole2 by closing 0/1/2 -- `start_command` forks pipes that rely on those fds
  staying occupied (tried + reverted v0.4.176).

**Current state:** Pi runs **v0.4.176** (eMMC fixed) on the LAN at 192.168.0.8; v1 netconsole on
2323. Repo at v0.4.176, committed, clean. netconsole2 ships in the disk (traced) but is NOT yet
getty auto-started. `pkill`/`killall`/`pidof` do NOT exist on AIOS -- to swap a running
netconsole2, power-cycle.

## Where we left off (v0.4.172 -> v0.4.174)

Net result this session: a big WRITE-SPEED win (write-back cache, HW-verified), a
quick `ls -l` mtime win (QEMU), and a netconsole multi-session rewrite that FAILED
on hardware and was REVERTED. Full lesson: `netconsole-push-speed-hw` memory.

* **Write-back block cache (v0.4.172, `7f7b4a9`, HW-VERIFIED).** File writes were
  ~21-23 KB/s: `blk_cache.c` was write-through and `blk_emmc.c` did one single-block
  CMD24 per 512 B sector (~1000+ synchronous flash writes per 296 KB; the inode
  rewritten on every `ext2_pwrite`). A LOCAL `cp` was as slow as a network push --
  the WRITE path, not the network, was the wall (proven by a local-cp test; the
  first guess blaming netconsole / the 32 KB rx ring was WRONG). Fix: drive-0
  WRITE-BACK (dirty bit; flush at a 16-line/64 KB threshold + on eviction + on
  shutdown/reboot; drive 1 log stays write-through for crash durability) +
  `plat_blk_write_multi` = CMD25 multi-block (one eMMC transfer per 4 KB line) +
  flush-before-shutdown/reboot in `aios_root.c`. HW: local cp 296 KB **12.6 s ->
  2.8 s (4.5x)**; CMD25 bytes correct (cp of /bin/dash byte-identical after a cold
  reboot); persistence works.
* **`/proc/version` real (v0.4.172).** Was hardcoded "0.4.x"; now `AIOS_VERSION_FULL`
  + build + date (the same macros `uname` uses, so they cannot drift). `uname -r`
  was already real (`fs_server` FS_UNAME -- a separate path).
* **netconsole v2 -- ATTEMPTED + REVERTED (v0.4.173 `023b5b7` -> revert `769d634`).**
  A non-blocking MULTI-SESSION event-loop rewrite (`docs/DESIGN_NETCONSOLE_V2.md`
  Option B) + a `net_server` non-blocking accept (NET_ACCEPT EAGAIN). Passed EVERY
  QEMU test (smoke 9/9, reconnect-stress 10/10, concurrency, no-wedge) but STALLED
  EVERY COMMAND on the real RPi4 -- the forked-dash -> output-pipe -> socket relay
  never delivered over GENET/A72. Found + fixed one real bug (the forked child
  closed fork-shared session sockets via NET_CLOSE_SOCK, tearing down the parent's
  connections) but it did NOT restore HW function. After 4 flashes, REVERTED to the
  v1 single-client netconsole. **Retry needs SERIAL debugging, not flash-iteration.**
* **`ls -l` mtimes -- READ path (v0.4.174, `48b28aa`, QEMU-verified).** v0.4.171
  WRITES `i_mtime` on create/mkdir; now the READ path shows it. Threaded mtime
  through `fs_stat`/`vfs_stat` -> `ext2_vfs_stat` (`i_mtime`) -> `fs_server` FS_STAT
  (MR3, was a reserved 0) -> libaios `fetch_stat_m` -> `statx`/`fstatat` fill. `ls
  -l` now shows real 2026 dates, no epoch. Backward-compatible (old binaries ignore
  MR3); only sbase rebuilt. No flash -- rides the next milestone image.

**Current state:** the Pi runs **v0.4.172** (write-back + old netconsole) on the LAN
at **192.168.0.8:2323** -- working; drive it with ~4 s settle delays between
connections. The repo is at **v0.4.174** (+ `/proc/version`, mtimes). The mtime
change and any future work batch into the NEXT milestone flash.

---

## Where we left off (v0.4.169 -> v0.4.171) -- COLOUR + 3D + NETWORK DEPLOY (condensed)

The display + network-deploy arc before this session (committed through `38d1f6c`):
RPi4 HDMI colour fix (`SET_PIXEL_ORDER=0`/BGR -- we write LE 0x00RRGGBB), a 1024x768
logo + a CPU software 3D spinning cube (`fbshow --cube`), then a large-file
network-deploy stack: netconsole robustness (the surgical single-client subset --
non-blocking + per-op deadlines), a net_server TCP receive flow-control fix, ext2
double-indirect WRITE (files >268 KB), a 32 KB rx-ring (the "speed" bump that this
session proved does NOT help HW push -- the wall was the write path), and
`aios_console.py monitor` (passive serial tap). HW-verified except that 32 KB-ring
speed claim. Detail: git history + the memories.

---

## Earlier arcs (v0.4.110 -> v0.4.168)

RPi4 HDMI (Phase B VC mailbox), GENET networking (DHCP + bidirectional ping),
network control (netconsole, watchdog reboot, file push/pull, SNTP wall-clock), COW
fork, demand-paged BSS, block-layer cache, TCC self-host, and more -- condensed
records are in **`docs/HANDOVER_HISTORY.md`**, with full per-session detail in
**`docs/NEXT_*.md`** and the memory index.

---

## Build and boot

### Full rebuild (when CPIO contents change)

`AIOS_NETD` now defaults **ON** (netd is the production net path). `build-04` is the
flag-OFF regression tree, so it needs an explicit `-DAIOS_NETD=OFF`; a bare configure
(or `build-netd`) builds the default-ON netd path.

```
cd ~/Desktop/github_repos/AIOS
rm -rf build-04 && mkdir build-04 && cd build-04
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- -DAIOS_NETD=OFF ..
ninja
```

### Incremental (most edits)

```
cd build-04 && ninja
```

`tty_server.c`, `auth_server.c` are in CPIO -- changing them needs
full rebuild. Everything in `src/aios_root.c`, `src/boot/*`,
`src/servers/*`, `src/process/*`, `src/lib/*` is in the root task
binary -- ninja handles incrementally.

### Disk image (after editing programs in `src/apps/` or `disk/rootfs/`)

```
python3 scripts/mkdisk.py disk/disk_ext2.img \
    --rootfs disk/rootfs \
    --install-elfs build-04/sbase \
    --aios-elfs build-04/projects/aios/
```

### Sbase

```
python3 scripts/build_sbase.py
```

Runs after `rm -rf build-04` (which deletes sbase binaries).

### Dash (rebuild after libaios_posix.a changes)

```
DASH=~/Desktop/github_repos/dash/src
./scripts/aios-cc \
    $DASH/main.c $DASH/eval.c $DASH/parser.c $DASH/expand.c \
    $DASH/exec.c $DASH/jobs.c $DASH/trap.c $DASH/redir.c \
    $DASH/input.c $DASH/output.c $DASH/var.c $DASH/cd.c \
    $DASH/error.c $DASH/options.c $DASH/memalloc.c \
    $DASH/mystring.c $DASH/syntax.c $DASH/nodes.c \
    $DASH/builtins.c $DASH/init.c $DASH/show.c \
    $DASH/arith_yacc.c $DASH/arith_yylex.c \
    $DASH/miscbltin.c $DASH/system.c \
    $DASH/alias.c $DASH/histedit.c $DASH/mail.c $DASH/signames.c \
    $DASH/bltin/test.c $DASH/bltin/printf.c $DASH/bltin/times.c \
    -I $DASH -include $DASH/config.h -DSHELL -DSMALL -DGLOB_BROKEN \
    -o build-04/sbase/dash
```

### ZSH (rebuild after libaios_posix.a changes)

```
python3 scripts/build_zsh.py
```

### aios-cc apps (netconsole, netconsole2, psutil, nslookup)

These use the aios-cc wrapper and are NOT in `projects/aios/CMakeLists.txt`, so ninja
does not build them. `scripts/build_apps.py` now builds them all (before mkdisk); after
a clean `rm -rf build-04` run it, or build one manually:

```
python3 scripts/build_apps.py                                  # all of them + the full build
./scripts/aios-cc src/apps/psutil.c   -o build-04/sbase/pidof  # + cp to pkill, killall
./scripts/aios-cc src/apps/nslookup.c -o build-04/sbase/nslookup
```

`nslookup <host> [server]` (default 8.8.8.8) -- DNS A-record resolver; QEMU+HW verified
via `scripts/dns_qemu_test.py`.

Pure userspace (reads `/proc/status`, signals via `kill(2)`). QEMU test:
`python3 scripts/psutil_qemu_test.py` (7/7). HW-verified (`pkill nsole2` killed
a running netconsole2). Note: kill() only works on REGULAR processes (in
`active_procs`); boot SERVERS appear in `/proc/status` but kill() returns ESRCH
for them (root-task threads), and the tool reports "FAILED on <pid>".

### Boot QEMU (with both drives -- log file persists)

```
cd ~/Desktop/github_repos/AIOS
qemu-system-aarch64 \
    -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G \
    -nographic -serial mon:stdio \
    -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -drive file=disk/log_ext2.img,format=raw,if=none,id=hd1 \
    -device virtio-blk-device,drive=hd1 \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

Login: `root` / `root`.

### Boot without log drive (test recovery mode)

Same command, drop the `hd1` drive. See "AIOS RECOVERY MODE" banner.

---

## Session protocols

### bump-patch at start

```
./scripts/bump-patch.sh
./scripts/version.sh
```

Always at the start of new work. `make bump-minor` is for major
milestones only.

### Commit

User prefers GitHub Desktop for commits, BUT we can do `git commit`
directly when explicitly asked. Format:

```
v0.4.XXX: short title

3-5 bullets / paragraphs of why and what changed.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

NEVER skip hooks. NEVER amend. NEVER force-push.

### Code edits

* Use Python heredoc to /tmp script then `python3` invocation when
  the user is running scripts in their terminal
* When you're operating directly via tools, use `Edit` / `Write`
* Verify changes with `grep`/`Read` after editing
* Single-quote apostrophes in C comments break zsh copy-paste --
  spell out instead (use ascii dashes etc)

---

## Useful files for context

* `docs/AI_BRIEFING.md` -- full architecture reference
* `docs/HANDOVER_HISTORY.md` -- older session arcs (v0.4.110 -> v0.4.168)
* `docs/DESIGN_NETCONSOLE_V2.md` -- multi-session rewrite (reverted; retry WITH serial)
* `docs/DESIGN_RPI4_3D.md` -- V3D hardware-3D plan; `DESIGN_COW_FORK.md` -- VM design
* `include/aios/root_shared.h` -- IPC labels, active_proc_t
* `include/aios/blk_cache.h` -- block cache (write-back) interface
* `src/blk_cache.c` + `src/plat/rpi4/blk_emmc.c` -- write-back + CMD25 (v0.4.172)
* `src/servers/fs_server.c` + `src/ext2.c` + `src/lib/posix_stat.c` -- fs + stat/mtime
* `src/apps/netconsole.c` -- v1 single-client netconsole (current; v2 in git history)
* `src/servers/net_server.c` -- TCP/UDP socket server
* `src/servers/pipe_server.c` -- central IPC hub, fault dispatcher
* `src/process/fork.c` -- eager-copy fork

---

## Known gotchas

* **tty_server is in CPIO** -- changing it requires full rebuild
* **dash + zsh + sbase rebuild** needed after `libaios_posix.a`
  changes; ninja does NOT rebuild them
* **dual virtio-blk warmup** required: a dummy
  `plat_blk_read(2, ...)` after `plat_blk_init_log()` or system
  disk reads silently fail (see `feedback_virtio_blk_warmup.md`)
* **EXEC_RUN_BG fault EP** must be minted into pipe_ep, otherwise
  no one polls it and the process hangs on first fault. Set
  `ap->fault_on_pipe_ep = 1` after minting.
* **morecore_area = 6 MB** static BSS per process. Now lazy via
  v0.4.106 unmap+fault. Adjust `LibSel4MuslcSysMorecoreBytes` in
  `settings.cmake` if you need more.
* **VKA pool = 8000 pages** total. With demand-paged BSS, that
  comfortably handles 5+ concurrent processes. Without it: 3
  max.
* **fork is eager** -- big writable regions duplicated on fork.
  COW design ready in `DESIGN_COW_FORK.md`.
* **TCC self-host works for libc-free programs only** -- archive
  parser issues on libc.a / libc_min.a. See `NEXT_20260501a.md`
  for the 5 fix options.
* **Block cache is WRITE-BACK on drive 0** (v0.4.172). Dirty pages flush at a
  16-line/64 KB threshold + on eviction + on shutdown/reboot (`blk_cache_flush`).
  A HARD power-cut loses the last unflushed writes (expected); `reboot`/`shutdown`
  flush first. eMMC line flushes use CMD25 multi-block. Drive 1 (log) stays
  write-through for crash-log durability.
* **Drive the Pi over netconsole GENTLY.** The v1 single-client netconsole wedges
  under RAPID back-to-back connections: use ONE held-open connection for many
  commands and a ~4 s settle between SEPARATE connections (push/pull/reboot). A
  fresh connection per command, or retry-without-close, reliably wedges it -- and a
  wedge blocks network access, so recovery needs a power-cycle.
* **close() on a socket fd sends NET_CLOSE_SOCK, and fork shares socket_id**
  (`posix_file.c`). A forked child closing a session socket tears down the PARENT's
  connection -- the netconsole-v2 HW trap. dash's EXIT drops inherited fds WITHOUT
  NET_CLOSE_SOCK, so a child INHERITING sockets is fine; CLOSING them is not.
* **QEMU transfer/write speed lies.** The 32 KB rx-ring "speedup" (v0.4.171) was
  QEMU-only; on HW the write path (now fixed) then the receive path are the walls,
  not the TCP window. Measure transfer/write SPEED on the Pi, never trust QEMU.

---

## What works "out of the box" right now

After boot + login:

```
ls                              # 104 sbase tools in /bin
ls -l /tmp/somefile             # REAL mtimes now (v0.4.174), not the epoch
echo "hello"                    # builtin
cat /proc/vka                   # accurate live page count
cat /proc/meminfo               # real MemTotal + Pool*
cat /proc/log | tail -50        # ring buffer log
cat /var/log/aios.log | tail    # persistent log
cat /proc/cachestats            # block-cache hit rate / size
cat /proc/filehits              # top accessed files (profiler)
cat /proc/serverstats           # ping-based server health (v0.4.121)
cat /proc/cow                   # COW per-frame refcount (v0.4.122)
cat /proc/cmdline               # platform-aware boot env summary (v0.4.131)

zsh                             # interactive, ZLE working
                                # (compctl warning is cosmetic)
                                # (rebuild after libaios_posix.a edits!)

ls /bin > /tmp/o; wc -c /tmp/o  # file redirect across exec works
echo abc | wc -c                # pipe across fork+exec works
cat /etc/passwd | head -1       # head limit works correctly

test_mprotect                   # mprotect R/O, PROT_NONE, PROT_EXEC,
                                # munmap, re-mmap round trip (v0.4.126-128)
ftruncate $file $size           # real fs-side truncate (v0.4.130)

/tmp/tcc2 -o /tmp/t /tmp/t.c    # native tcc (libc-free programs)
tcc /usr/include/hello.c -o /tmp/h  # native tcc with libc (v0.4.117)
/tmp/h; echo $?                 # libc programs run

pidof dash; pkill netconsole2   # process tools: pidof/pkill/killall (v0.4.176)
# kill foreground with Ctrl-C twice (two-stage SIGINT)
# logout via Ctrl-D from getty
```

---

## Suggested next sessions

**Recent: SSH RECOVERED + always-on (6 commits this session, see "Where we left off"). The Pi
runs v0.4.176 + a pushed sshd (works, one session per boot). TOP NEXT: fix SSH reconnect via the
FORK-FREE SHELL SPAWN -- the cause is PROVEN (the shell `fork()` corrupts a living sshd; COW and
the SMP race are both DISPROVEN, do not re-chase), and the complete fix design is in
`docs/NEXT_20260606b_forkfree_ssh.md`. It is a root-task change + a milestone flash (which also
lands the committed affinity pin -> bump-patch then). Other picks:**

1. **getty auto-start netconsole2 (the clean launch).** netconsole2's relay works on HW now, but
   its robust LAUNCH is the open piece: a `>FILE` redirect leaks the child output to the file
   (AIOS `dup2` does not re-route fd1 file->pipe), and a no-redirect `&` launch wedges v1. The
   relay is fine with fd1 = a PIPE or a TTY, so launch netconsole2 from getty (fd1=tty, exactly
   like the working v1 netconsole) -- a small `getty.c` change + reflash -- then drive 2324
   (multi-session, concurrent clients) over the LAN. Optionally fix the AIOS `dup2` file->pipe
   routing in `posix_file.c` (see [[is-tty-routing]]) so file-redirect launches work too. The
   reverted big-bang v2 (`023b5b7`/`769d634`) is obsolete -- netconsole2 superseded it.
2. **Deploy PUSH speed.** Still ~21 KB/s -- the netconsole RECEIVE path (900 B socket reads +
   per-read window-ACK chatter), NEVER solved (write-back fixed the WRITE side; the receive
   side is now the wall). Fix = a `net_server` bulk-receive (shared frame so netconsole reads
   KBs per syscall, mirroring `__get`) and/or throttle the per-900 B window update
   (`net_server.c:572`). HW-only to verify. Needs a WORKING netconsole first (so: after #1).
3. **Milestone flash (procedure reference).** The Pi now runs **v0.4.176** (eMMC fix + mtimes +
   netconsole2, flashed + HW-verified this session). For future root-task/kernel changes, batch
   them: `ninja -C build-04 && ninja -C build-rpi4` -> rebuild userspace (sbase/dash/zsh/netconsole
   if libaios changed; netconsole2 via `./scripts/aios-cc src/apps/netconsole2.c -o
   build-04/sbase/netconsole2`) -> `mkdisk` -> `mksdcard` (defaults are correct: mem 4096, the
   build-rpi4 kernel, disk_ext2.img) -> balenaEtcher.
4. **getty netconsole auto-respawn.** Tried + REVERTED (AIOS fork-of-fork fails). Needs a getty
   `waitpid(-1)` event loop that does not block on serial login-auth.
5. **kernel-over-network** -- write `kernel8.img` to the FAT boot partition + reboot (the last
   flash-elimination piece). Needs FAT-partition WRITE (AIOS mounts/writes only ext2). Meaty.
6. **hardware 3D (V3D)** -- `docs/DESIGN_RPI4_3D.md` (~3-6 weeks; minimal register-level driver;
   the IV-vs-VI trap + A72<->V3D cache coherency are the load-bearing risks).
7. **RPi4 SMP bring-up** -- v0.4.135's SMP=4 hangs in the elfloader spin-table. HW-gated.

**Lower-priority:** scp/sftp (blocked on the lost mbedTLS; the SSH server exists);
Bluetooth/HCI (`docs/DESIGN_BLUETOOTH_HCI.md`, console-safe PL011 UART but needs a blob + stack).
The deferred VM backlog (COW Steps 3-5, block-cache write-back-for-log, swap) is in
[BACKLOG.md](BACKLOG.md).

**If hardware is unavailable:** most logic is QEMU-testable (the net harness NATs UDP, even
SNTP works) -- write-back correctness, mtimes, the netconsole protocol, fs/VM. But the
fork/pipe/socket relay, eMMC write speed, GENET timing, and cache attributes are HW-only.

---

## Final notes

The system is in a strong place: stable boot on QEMU + real RPi4, demand paging,
real GENET networking (DHCP, ping), a netconsole control channel (drive the Pi over
the LAN -- run commands, push/pull files, reboot), real wall-clock time via SNTP, a
write-back block cache (4.5x faster file writes), `ls -l` mtimes, working shell + ZLE,
TCC for simple programs. Drive the live Pi over `scripts/pi_filexfer.py` / a held-open
socket to `192.168.0.8:2323` (with settle delays) instead of the lossy mini-UART. The
deploy PUSH is functional but slow (~21 KB/s, receive-path bound -- the next target);
PULL is fast (~1 MB/s).

When in doubt:
* Check `cat /proc/log` and `cat /var/log/aios.log` for traces
* `cat /proc/vka` to see if memory pressure is the culprit
* `cat /proc/genet.ip` (RPi4) for one-line network status
* Look at `[INF]` / `[WRN]` / `[ERR]` tagged lines on serial -- the
  module name (boot, fs, blk, exec, pipe, vka, gpu, net, etc.) tells you
  which subsystem to read

Good luck.
