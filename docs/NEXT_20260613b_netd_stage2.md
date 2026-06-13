# NEXT: finish netd Stage 2 (skeleton process) -- 2026-06-13

Seed prompt for a fresh session. The de-monolithization work stream (move the
net subsystem out of the root task into an isolated `netd` process) is the
active thread. Stages 0 and 1 are DONE + verified; Stage 2 is half-built.

---

## Paste-this-style brief

AIOS (research OS on seL4, repo `~/Desktop/github_repos/AIOS`, branch `main`,
now at **v0.4.230**). Continue the netd de-monolithization. Read
`docs/DESIGN_NETD.md` first -- especially **§9 Stage 2**, **§3 capability
handoff**, **§8 boot handshake/degrade**, and **§10 failure/restart** (the
reply-cap ground truth + the reply-slot sweep). Then the memories
`project_demono_netd`, `feedback_qemu_test_hygiene`, `feedback_rebuild_order`,
`feedback_sel4_nested_call`.

**Task: finish netd Stage 2 -- the netd SKELETON process (one commit).** The
foundation is already in the working tree (uncommitted); complete it, verify on
QEMU, and commit as one v0.4.231 Stage-2 commit. Do NOT start Stage 3 (the real
net cutover).

Already done this session (in the working tree, UNCOMMITTED):
- `src/apps/netd.c` -- the skeleton: argv cap parse (`[0]`=main EP, `[1]`=ctrl/
  fault EP, `[2]`=ntfn, decimal strings); **self-bind ntfn to TCB slot 5
  (SEL4UTILS_TCB_SLOT) + assert**; `DEVD_READY`(100) Send on the ctrl EP (Send,
  never Call); Recv loop with `SVC_PING`(5)->reply MR0=0 and `NETD_CRASH`(96)->
  null deref. Compiles + links clean on BOTH trees.
- `projects/aios/CMakeLists.txt` -- `AiosChildApp(netd)` + `option(AIOS_NETD ...
  OFF)`. Flag OFF = parity (netd binary builds but is not bundled/spawned).

Still to do (the intricate part -- this is why it was left for a fresh session):
1. **`src/boot/spawn_netd.c`** -- custom spawn modeled on `spawn_util.c`
   (`process_config_*`, cnode 12 bits, prio 200, `process_config_fault_endpoint`
   = the ctrl EP), retain a global `netd_proc`. Root creates the main EP +
   ctrl/fault EP + a notification; copy all three into netd via
   `sel4utils_copy_cap_to_process`; pass their child slots as decimal argv.
   Start a **dedicated fault-listener thread (prio 200, core 0) Recv-ing on the
   ctrl EP BEFORE resume** -- it decodes faults (label <=6 on aarch64) vs
   `DEVD_READY`(100)/`DEVD_FAIL`(101), AIOS_LOGs them, and on a netd fault logs
   PC/addr and proves containment (system keeps serving). Then resume; root
   waits for READY with a cntpct-bounded `seL4_NBRecv`+Yield poll (~10s), degrade
   -and-continue on timeout/FAIL.
2. **The `CNode_Move` reply-sweep experiment (THE kernel-semantics bet, §10).**
   Add `NETD_BLOCK`/`NETD_KICK` selftests: netd `SaveCaller`s a parked caller's
   reply cap into a RESERVED slot in its own cnode (the guarded slot-1 cnode cap
   resolves plain slot numbers -- §3 row 6; reserve by bumping
   `proc.cspace_next_free` past the donations in spawn_netd). Then root
   `CNode_Move`s that saved reply cap out of netd's cnode into a root scratch
   slot and `seL4_Send`s it -- DOES THAT WAKE THE PARKED CALLER? Prove or kill
   it here; it is the foundation of the crash-recovery sweep. (Driver for the
   park: a userland caller -- `/bin/netdiag` Calling the main EP, or root.)
3. Build `/bin/netdiag` now (works against the in-root path too) and add it to
   `scripts/build_apps.py` so it ships on the disk before any Stage-3 HW cutover.
4. Boot wiring in `boot_services.c` behind `#ifdef AIOS_NETD` -> `spawn_netd()`,
   and gate the CPIO inclusion of netd on `AIOS_NETD` so flag-OFF is bit-identical.
5. **`scripts/netd_qemu_test.py`** -- flag-ON: netd spawns, `SVC_PING` ok,
   block/kick, reply-sweep result, `NETD_CRASH` -> fault-listener log line ->
   shell/fs/pipe still alive.

Verify (QEMU only, no flashing needed): both trees build (`ninja -C build-04`
AND `ninja -C build-rpi4`). Flag OFF (default) = full existing suite passes,
behaviour identical (`scripts/ssh_qemu_test.py`, `scripts/netcon_qemu_test.py`,
`scripts/net_socket_qemu_test.py`). Flag ON (reconfigure build-04 with
`-DAIOS_NETD=ON`, or a scratch build dir): the netd_qemu_test selftests +
crash containment + a vka_audit before/after spawn page count + boot-latency
cntpct delta. Optional final HW smoke is NOT needed for Stage 2 (no net code
moves yet).

---

## State at handoff (verify before trusting -- point-in-time)

- **Repo `main`:** Stage 1 committed `1616e0f` (v0.4.230), **ahead 1 of
  origin/main** (Stage 0 `000203a` was pushed by Bryan; ASK before pushing
  Stage 1). Stage 2 foundation UNCOMMITTED (`src/apps/netd.c` new,
  `projects/aios/CMakeLists.txt` modified). Both trees build at v0.4.230.
- **The Pi (192.168.0.127, 4-core):** running **Stage 1 build 2150** (flashed
  this session via kernel8.img swap on the mounted AIOSBOOT). Net HW-verified
  (DHCP bound, GENET IRQ-RX climbing, ping 40/40 0% loss, netconsole works).
  Revert path: `kernel8-prev.img` (build 2125) is on the card + `disk/kernel8-
  prev-2125.img` in the repo. **sshd is left down** by diagnosis (see queue).
  Serial: `/dev/cu.usbserial-0001` @115200, single reader. netconsole wedges on
  back-to-back conns -- rest ~45s.

## What shipped this session (for HANDOVER context)

- **netd Stage 0** v0.4.229 (`000203a`, PUSHED): `include/aios/net_proto.h`
  (centralized NET labels + static-assert mirror), RST/close reply-slot
  poisoning fix (`net_sock_wake_reset`/`drop_parked`/`net_park_caller`),
  cleanup-proxy (SPSC ring + sacrificial thread in pipe_server), serverstats
  dead-state, connect() lazy-PID, `src/apps/nettest.c` + `scripts/
  net_socket_qemu_test.py`. QEMU socket suite 8/8 (incl bulk_burst) + ssh 6/6.
- **netd Stage 1** v0.4.230 (`1616e0f`): merged net driver+server into one
  thread; `plat_net_drain()` (net_hal.h, per-platform) + bound RX IRQ ntfn +
  NAPI re-check; badge=1 IRQ / badge=2 kick mints; removed net_srv_ntfn + the
  driver thread. QEMU 8/8 + ssh 6/6, **HW-verified on the real Pi**.
- **ext2-builder "bug" was a MISDIAGNOSIS** -- a one-time concurrent write to
  `disk/disk_ext2.img` (another session) corrupted a fresh build; the builder is
  deterministic + clean. See `feedback_qemu_test_hygiene`. Cost a long hunt;
  diagnose a mysteriously-corrupt fresh disk with homebrew e2fsprogs + a scratch
  rebuild before suspecting the builder.

## Queue (after Stage 2)

1. **sshd on the Pi does NOT listen on port 22** (build 2150; boot-time AND a
   manual `sshd` restart both leave 22 unanswered, while ping/netconsole/DHCP/
   IRQ-RX all work). sshd-SPECIFIC, NOT the net stack and NOT Stage 1 (QEMU ssh
   6/6 on the same kernel). Multi-session was already FIXED v0.4.178 -- this is a
   DIFFERENT, newer bug; most likely the Pi's disk `/bin/sshd` is STALE (the
   v0.4.178 fix was a userspace `pi_filexfer` deploy, reverted by a later card
   reflash). NEXT: compare on-disk `/bin/sshd` sha vs `build-04/sbase/sshd`,
   redeploy, then diagnose `socket()`/`bind(22)`/`listen()` (is socket()
   returning -ENOTSUP -- no net_ep in the getty-forked sshd?). See
   `project_ssh_recovered`.
2. **netd Stage 3** -- the real net cutover behind `AIOS_NETD` (`#ifdef
   NETD_BUILD` prov/dev split in the two drivers, frame/DMA retention, stripped
   prov MAC query, retry-for-low DMA, stats page + /proc/net, the §8 handshake).
   The big one; gated on Stage 2's reply-sweep result.

## Gotchas (learned/re-confirmed this session)

- **Concurrent Claude sessions share the checkout + `disk/disk_ext2.img`.**
  Before a disk build, `lsof disk/disk_ext2.img` + `pgrep -f mkdisk|build_apps`;
  a mysteriously-corrupt fresh disk is concurrency, not the builder.
- **QEMU boot is slow under VKA/morecore pressure** (pre-existing, not the net
  merge): boot can exceed 150s to login. `net_socket_qemu_test.py` uses a 240s
  netconsole deadline. The slow boot also fragments the serial password prompt
  (boot-log spam) -- drive HW/QEMU tests over netconsole-TCP, not serial login,
  where possible.
- Build BOTH `build-04` AND `build-rpi4` after shared-code changes. Bryan: no
  apostrophes in C comments; commit only when asked; never amend/force-push.
