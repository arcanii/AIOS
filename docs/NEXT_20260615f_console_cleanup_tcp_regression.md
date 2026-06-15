# NEXT (session seed): TCP-regression recovery + HDMI console cleanup + keyboard LEDs

Paste the brief below into a fresh session. Then read `HANDOVER.md` (top), `MEMORY.md`,
and this doc in full. Repo `~/Desktop/github_repos/AIOS`, branch `main`.

## CRITICAL STATE FIRST -- the Pi network is BROKEN (my regression)

The real RPi4 is running a flashed **v0.4.253 (build 2421)** kernel that has a **HW-only
TCP regression**: the new sender-retransmission + graceful-close (commit `3e3e26a`) RSTs
live connections on real hardware -- SSH short sessions deliver 0/20 output, and
netconsole gets `Connection reset by peer`. The LOCAL HDMI console + keyboard WORK
(the bug is the net stack only). QEMU could NOT catch it: SLIRP is lossless, so the
loss/retransmit/RST path never ran. (`/proc/version` over netconsole still worked once,
which is how we know the kernel booted; then connections started getting RST.)

**ROLLBACK (do this to get the network back):** the known-good **cube kernel
v0.4.252 (build 2385)** is staged at `disk/kernel8.img` (sha `b3aa70d…`) AND backed up at
`/tmp/card_kernel8_cube_backup.img`. Network flash is IMPOSSIBLE (netconsole RSTs), so it
MUST be the physical card: Bryan mounts the SD on the Mac (`/Volumes/AIOSBOOT`), `cp
disk/kernel8.img /Volumes/AIOSBOOT/kernel8.img`, verify sha, `diskutil eject`, reinsert,
power-cycle. That reverts BOTH my net + usb flashes to a clean working state (nothing is
lost -- all work is committed in git).

## TASK 1 (highest priority) -- diagnose + fix the TCP regression, HW-test-FIRST

Commit `3e3e26a` (net_server.c): adds `snd_una`, a 4KB retransmit ring, `net_tcp_rto_check`
in the main loop, a DEFERRED graceful close (hold the FIN until data is ACKed), and a
give-up that sends **TCP_RST** + frees on `TCP_RTX_MAX(8)` or a 10s `close_deadline_ms`.
Two read-only reviews + the full QEMU suite (socket 8/8, ssh 6/6, reconnect 6/6, netd
10/10) all passed -- but ALL of QEMU is lossless, so none of this exercised the new code.

Symptoms on HW: live connections get RST; SSH last-command output 100% lost. The RST can
only come from `net_tcp_rto_check`'s give-up path -- so on HW either the deferred close
never completes (snd_una not advancing as expected) and every socket hits the give-up, OR
rto_check fires spuriously and force-RSTs healthy sockets.

**Prime suspects to instrument (add serial logging in net_server.c, reflash, watch):**
- `net_now_ms()` on the A72 (cntpct_el0/cntfrq_el0): is `rtx_due_ms`/`close_deadline_ms`
  computed sanely, or does bad timing make `now >= rtx_due_ms` immediately true ->
  rapid retransmit -> rtx_count hits 8 -> RST? (net_dhcp.c uses the same cntpct pattern
  and works, so the read is probably fine -- but verify the ms math.)
- The ACK-consume advancing `snd_una`: on HW, does the deferred close ever reach
  `snd_una == snd_nxt` so `net_tcp_maybe_send_fin` fires? If not, the FIN never goes and
  the socket sits until the 10s deadline -> RST.
- rto_check running EVERY main-loop iteration over all sockets: any path that re-arms +
  immediately re-fires (busy) -> rtx_count saturates -> RST.

**KEY: get a LOSS test before re-flashing.** Try QEMU with packet loss so the loss path
runs without HW: `qemu ... -netdev user,id=n0,...` does not drop, but a `tap`+`netem`
(`tc qdisc add dev tap0 root netem loss 30%`) or socket-netdev with a lossy shim would.
If a lossy QEMU repro is too hard, instrument + reflash via the CARD (network is dead)
and read the serial/HDMI. Consider a SAFER design: do NOT send RST on give-up (just free);
or do NOT defer the FIN -- send it immediately but keep retransmitting unACKed DATA after
it (simpler, no close-handshake change). The data-retransmit core itself reviewed sound;
the deferred-close + RST is the suspect. Spec/history: `docs/NEXT_20260615c`.

## TASK 2 -- quiet the HDMI console (boot/service text -> log+serial only)

Bryan's screenshot: the HDMI shows [boot]/[dtb]/[fs]/[net]/[gpu] lines, `[INF] auth:`
messages, and a GARBLED netconsole startup line interleaved with `AIOS login:` (netconsole's
startup printf races getty's prompt on the shared fb_console). He wants the HDMI clean
(login prompt + shell only); diagnostics belong on serial + `/var/log`.
- Boot status: `src/aios_root.c:420-445` uses `fb_console_printf(...)` for the [boot]/[dtb]/
  [fs]/[net]/[gpu] banner -- route these to serial/log, not fb_console (or gate behind a
  verbose flag).
- `[INF]` service messages: `AIOS_LOG_INFO` (and service `printf`) reach fb_console --
  trace the printf/AIOS_LOG routing (fb_console_putc in src/boot/fb_console.c) and make
  INFO-level + service chatter serial/log-only; keep the fb_console for the tty/shell.
- netconsole gibberish: netconsole's startup banner prints to the tty/fb_console and
  interleaves with the login prompt -- suppress it / send to log (`src/apps/netconsole*.c`;
  it is getty-forked, fd1=tty). Mind [[netconsole-redirect-fd-bug]] (do not break fd1).

## TASK 3 -- remove the stale version banner

`src/apps/getty.c:356-358` prints `"===== " AIOS_VERSION_FULL " ====="` -- but
`AIOS_VERSION_FULL` is baked into the getty BINARY at build time, and the on-disk getty is
an OLD deploy (shows `v0.4.244 build 2263` while the kernel is 0.4.253) -> confusing. Bryan:
"get rid of it, not needed." REMOVE the getty version banner (lines 356-358). The kernel's
real version still prints at `aios_root.c:420` and via `/proc/version`. (getty is a DISK
app -> deploy by `pi_filexfer.py push` once the net is fixed, NOT a flash.)

## TASK 4 -- keyboard lock LEDs (NumLock / CapsLock)

The lock state is tracked (num_lock/caps_lock affect the keymap) but the PHYSICAL keyboard
LED does not light -- "lock-LED software-only; Stop-Endpoint LED fix backlogged"
([[project_usb_hid]]). `set_leds()` / the SET_REPORT path is in `src/usb/xhci.c` (g_led_request
channel ~527, set_leds ~579). Make Caps/Num Lock toggles actually light the LED: debug why
the SET_REPORT (HID Output report, lock bitmap) does not update the device LED -- likely
the backlogged Stop-Endpoint interaction. HW-only to verify (real keyboard + watch the LED).

## State to verify (point-in-time, 2026-06-15)

- `main` is ahead of origin. Commits THIS session: `9e543c6` (V3D cube 4a-C, HW-VERIFIED,
  Bryan pushed), `295999c`+`6be31d1`+`ac4610e` (docs/BACKLOG), `3e3e26a` (TCP retransmit --
  **HW-REGRESSED, do not redeploy as-is**), `07fa756` (xHCI keyboard hotswap -- QEMU-verified,
  root-port only; the Pi keyboard is hub-downstream so its hotswap needs the VL805
  interrupt-IN follow-up). `include/aios/version.h` is at 253 UNCOMMITTED (the flashed but
  regressed kernel) -- leave it / set per what actually ships next.
- xHCI hotswap test: `scripts/xhci_hotswap_qemu_test.py` (PASS). TCP gates: the net_socket /
  ssh / netd suites (QEMU-green but loss-blind).
- The Pi: running the BAD 253 until rolled back (above). All four USB ports are behind the
  one VL805 hub. Drive netconsole gently once restored (~50s rest between connections).
- Lesson: a change to the net path that QEMU cannot exercise (loss/timing) MUST be HW-tested
  (or lossy-QEMU-tested) BEFORE flashing -- two clean reviews + a green lossless suite were
  not enough here. See [[feedback_qemu_cannot_model_loss]] (write it).
