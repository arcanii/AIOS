# NEXT: session seed -- 2026-06-13c (post netd Stage 2 + stability/HW arc)

Paste-this-style brief for a fresh session. Read `HANDOVER.md` (top + the two
DONE sections dated 2026-06-13) and the memory index `MEMORY.md` first.

---

## Paste-this brief

AIOS (research microkernel OS on seL4, repo `~/Desktop/github_repos/AIOS`, branch
`main`, at **v0.4.235**, **ahead-3 of origin** -- Bryan pushes via GitHub Desktop;
commit only when asked, never amend/force-push, no apostrophes in C comments).
Develop + verify on QEMU; flash the real RPi4 only at milestones (over the LAN or
balenaEtcher). Last session finished netd de-monolithization **Stage 2** (the
isolated `netd` skeleton -- the **reply-sweep kernel bet is PROVEN**: root can
CNode_Move a netd-parked reply cap out and Send it to wake the caller) plus three
stability follow-ups, and HW-verified them on the real Pi.

**HW-verified this session (real Pi, v0.4.234/235):** A (quiet per-spawn serial
logging), B (DHCP lease renewal -- option-51 parse from the real router), and the
RPi4 thermal cap (`arm_freq=600`, boots stable + stall-free at 600MHz). **NOT
fixed:** C (GENET real-MAC read -> the Pi takes lease `.127` not `.8`) -- two
attempts failed, root-caused, BACKLOGGED (harmless).

## Pick a thread (all need the Pi except Stage-3 dev, which is QEMU-first)

1. **netd Stage 3 -- the real net cutover** (the headline arc; QEMU-first, lots to
   do before any HW). Move the net stack into the isolated `netd` process behind
   `option(AIOS_NETD)`: `#ifdef NETD_BUILD` prov/dev split in net_virtio.c +
   net_genet.c, frame/DMA retention in a root `driver_handoff_t`, the stats page +
   `/proc/net`, the §8 boot handshake, the crash-recovery reply-sweep (Stage 2
   proved it works). Read **`docs/DESIGN_NETD.md` §9 Stage 3** + §3/§8/§10. Gated
   on Stage 2 (done). The big one; QEMU-exhaustively first, then an SD-shuffle HW
   pass. `scripts/netd_qemu_test.py` + the socket/ssh/netcon suites are the gates.

2. **C -- GENET real-MAC fix** (contained HW bug; `.127` is harmless so low
   urgency). The mailbox MAC read (`genet_mbox_call`) returns `ret=-1` EVERY time
   (confirmed by a fully-settled post-boot `cat /proc/genet.mac`), while
   `display_vc`'s `mbox_call` to the SAME VC mailbox SUCCEEDS. So net_genet's call
   is broken, NOT a timing race. Prime suspect: tag-buffer region/coherency --
   display pins its tag low at `0x3A000000` (the v0.4.168 fix); net_genet uses
   `genet_dma+0x10000`. **First: instrument `genet_mbox_call` (log which poll
   fails + `buf[1]`) OR point read_mac_from_mailbox at display_vc's tag region,
   then one reflash.** ALSO revert the v0.4.234 retry + v0.4.235 deferred re-read
   (they waste ~14s of boot polling, all failing). Full plan: **BACKLOG.md "GENET
   real-MAC read fails"** + memory `feedback_genet_umac_swinit`.

3. **DVFS governor + /proc/temp** (heat follow-up). The static `arm_freq=600` cap
   shipped; next is load-driven scaling (idle = low clock, NEVER WFI -- WFI
   re-opens the v0.4.228 TLBI stall) via the VC-mailbox SET_CLOCK_RATE helper
   already in `src/gpu/v3d.c`, plus a `/proc/temp` (mailbox GET_TEMPERATURE
   0x00030006) to quantify the win. Plan: **BACKLOG.md "RPi4 power/thermal --
   DVFS"**.

4. **sshd port-22** (queued regression). sshd does not answer on `:22` on the Pi
   (build 2150+). Likely a stale on-disk `/bin/sshd` or socket()/bind/listen in
   the getty-forked sshd. See memory `project_ssh_recovered`. HW-gated.

Recommendation: **Stage 3** if you want the headline architectural progress
(QEMU-developable now). **C or DVFS** to close this session's HW loops (both
HW-gated). Either way, **ask Bryan to pick** before committing to the big one.

## Gotchas / lessons learned this session (re-confirmed the hard way)

- **HW verification = capture the SERIAL boot log, NOT netconsole.** The v1
  netconsole (`:2323`) WEDGES on repeated/rapid connections (I wedged it several
  times; recovery = power-cycle). For HW verify: power-cycle the Pi and capture
  `/dev/cu.usbserial-0001` @115200 with a pure pyserial reader from boot start
  (the boot log carries version, MAC, DHCP lease, console cleanliness, stall
  markers). If you MUST use netconsole, ONE held connection, a patient banner
  wait (~25s), a few commands, then STOP -- do not reconnect.
- **The headline checks are passive.** "Which IP did the Pi take" (`.8` vs `.127`)
  is just a `ping` -- no console needed. `/proc/genet.ip` (one line) and
  `/proc/genet.mac` (forces a fresh mailbox read) survive the lossy mini-UART.
- **A real-Pi boot can outrun your capture.** Start the serial capture BEFORE
  power-on (or in the background, then power-cycle). `[tlbi] alive rounds=N`
  climbing by 15 every 30s = the system is alive and NOT stalled (a ~32s gap =
  the stall).
- **A failed mailbox/poll fix can silently add boot latency** -- each
  `genet_mbox_call` failure burns its full 2s deadline; a retry loop multiplies
  it. Don't ship retry loops around a call that fails deterministically.
- Drive HW/QEMU tests over netconsole-TCP / serial-capture, never the serial
  LOGIN (the password-prompt step is flaky under boot-log spam -- pre-existing,
  unrelated to A).

## State to verify before trusting (point-in-time)

- Repo `main` v0.4.235, clean tree, ahead-3: `706a820` (v0.4.235 C deferred read
  -- a FAILED fix, revert pending), `60f7fb1` (arm_freq=600 heat cap), `ad7c438`
  (C backlog). Origin at `6f241d0`. Both trees build.
- Pi: powered off at handoff; last ran v0.4.235 @ 600MHz at `192.168.0.127`
  (sshd `:22` was down). The SD image `disk/sdcard-rpi4.img` is v0.4.235 +
  arm_freq=600. Revert path / older kernels per `feedback_kernel8_deploy_verify`.
- Key refs: `docs/DESIGN_NETD.md` (Stage 3), `BACKLOG.md` (C, DVFS), memories
  `project_demono_netd`, `feedback_genet_umac_swinit`, `project_dhcp_client`,
  `project_stall_hunt`, `feedback_qemu_test_hygiene`.
