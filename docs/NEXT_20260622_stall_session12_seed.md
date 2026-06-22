# SEED / HANDOVER -- session 12: make core 0 actually idle (convert the last 2 spinners), then the cure is DECISIVE

Paste the SEED PROMPT block at the bottom into a fresh session. Everything above is grounding.
The stall is a MAJOR OPEN CONCERN -- never frame it as solved ([[feedback_stall_open_concern]]).

---

## WHAT SESSION 11 ESTABLISHED (HW-measured, RPi4 192.168.0.8; commits ba1174e + 14d1522 on main)

The s11 plan = "replace yield-spins with blocking sleep so core 0 idles -> warm resume line ->
no cold I-fetch -> no stall." Outcome:

1. **PARTIAL WIN (real): the root-loop timer-poll HALVED the stall (~5-6/10 -> ~2/8).** The seed
   MISSED the dominant core-0 spinner: the ROOT EVENT LOOP (src/aios_root.c ~line 611) yield-spun
   CONTINUOUSLY because the RPi4 mini-UART is polling-mode (no usable IRQ -- shared with AUX SPI),
   so irq_uart_active=0. Converting it (+ the xhci kbd loop, also no usable IRQ) to a TIMER-DRIVEN
   poll (block on the system timer between polls) cut the stall ~in half. /proc/rootpoll +
   /proc/xhci.poll knobs, default-on.

2. **The blocking-sleep cure did NOT reduce below the root-loop fix (2-4/8).** Because **core 0
   STILL never reaches idle.S** -- /proc/cpuacct over a 30s netstall sleep showed core 0 =
   **core0_hb 78% + pipe 21%, (idle) 0%**, and [STAGECP] idle_lag stays -1 at every config.

3. **THE BLOCKER IS LOCALIZED to 2 remaining core-0 spinners (this is the s12 lever):**
   - **core0_hb (78%)** = the MVD-1 watchdog's core-0 heartbeat (`wd_core0_hb_fn`,
     src/servers/watchdog.c:189) -- a prio-200 yield-spin that stamps g_core0_hb_tick. Its OWN
     comment: "this is also why the kernel idle never runs on core 0." DELIBERATE (the watchdog
     detects a wedge by the stamp freezing). NOTE: disabling it (/proc/watchdog.0) alone did NOT
     fix idle_lag or the stall -> it is NOT the sole blocker.
   - **pipe (21%)** = the pipe server burning 21% during an IDLE sleep (should be blocked on
     Recv). UNEXPLAINED -- find why (a poll loop? the netconsole relay path? the SLOW-msg path?).

4. **Part A timer service = HW-VALIDATED, works** (a reusable building block). Two HW-only bugs
   fixed (QEMU-virt has no BCM system timer so the gate couldn't catch them): a boot self-test +
   **a bound-notification wake's MessageInfo LABEL is GARBAGE (observed 4, not 0) -- discriminate
   the IRQ by BADGE, never label.** Reusable for any AIOS bound-notification server.

5. **Core-agnostic kernel = BACKLOGGED** (docs/BACKLOG_core_agnostic_kernel.md): proven the seL4
   BKL is RELEASED before eret-to-user, so a user-mode wedge holds no lock; the cluster freezes
   only because all servers are on the wedging core; the ONLY barrier to multi-core servers is the
   single-owner allocman/vka. A blast-radius strategy (survive the wedge), Bryan's call when to un-shelve.

---

## SESSION 12 = THE DECISIVE TEST (cheap, high-information)

Make core 0 ACTUALLY reach idle.S, then re-run the netstall. This DECIDES the blocking-sleep cure:

1. **Convert `wd_core0_hb_fn` (watchdog.c:189) from a yield-spin to a TIMER-POLL** -- stamp
   g_core0_hb_tick every ~100ms via `aios_timer_sleep_us` (the timer service works now), idle
   between. It STILL freezes on a wedge (the core-1 watchdog still detects it), but stops eating
   78% of core 0. (Tunable knob like /proc/rootpoll.)
2. **Find + fix the pipe server's 21%** during an idle sleep -- read pipe_server.c for any poll
   loop / busy path; it should be blocked on seL4_Recv. Possibly the net-cleanup-proxy or a
   periodic check. Convert it to block (or timer-poll if it must be periodic).
3. **Re-check /proc/cpuacct -> (idle) should be > 0 and idle_lag != -1.** THEN re-run the cure
   netstall (timersleep.1 + the new sleep; reasonable polls). **DECISIVE:**
   - core 0 idles AND stalls drop toward 0 => the blocking-sleep cure is VINDICATED (ship it).
   - core 0 idles BUT stalls persist => the cure is TRULY refuted; the wedge is idle-FUNDAMENTAL
     regardless of who's running -> pivot to core-agnostic (blast-radius) or IRQ-driven UART/xHCI.

Either outcome closes a 12-session-old question. Do this FIRST.

FALLBACKS (if the cure is refuted): (a) un-backlog the **core-agnostic kernel** (blast-radius;
the BKL-released finding makes it viable). (b) **Real IRQ-driven mini-UART** so core 0 blocks on
input instead of polling (hard: the mini-UART IRQ is shared with AUX SPI; needs the AUX IRQ +
demux). These are the only ways to a *usable* fully-idle core 0.

---

## STATE / METHOD (hard-won)
- **Board: build 2872** (192.168.0.8), root+xhci timer-poll default-on, watchdog RE-ENABLED.
  Runtime knobs reset to defaults on reboot (timersleep OFF, polls 20ms = the ~2/8 partial-win
  config). USB disconnected (reconnect anytime). Commits ba1174e + 14d1522 on main (Bryan pushes).
- HW: `scripts/sercap.py /tmp/x.log` BEFORE any stall test (ONE serial reader). Deploy a kernel
  with `scripts/pi_flash.py` (network, rides stalls) OR a FULL new disk via balenaEtcher of
  `scripts/mksdcard.py` output (disk/sdcard-rpi4.img). **pi_deploy of one /bin binary FAILS** --
  the rename-over-existing is EPERM on the AIOS fs. A fresh netconsole CONNECTION spawns+reaps a
  shell -> that teardown-after-idle IS a stall trigger; the board is CALM when left alone (don't
  mistake probe-induced stalls for a wedge). Don't over-hammer netstall -> fork-exhausts -> power-cycle.
- FULL QEMU gate before every flash: smp 4/5, socket 8/8, netd 10/10, shmring 25/26. seL4 changes
  -> deps/kernel (sibling repo); regenerate deps/patches/seL4-kernel.patch before committing.
- Diagnostics: /proc/cpuacct (read TWICE -- first primes), /proc/timer, /proc/freezes, the
  [STAGECP] idle_lag/allidle in the serial. netstall: `scripts/netstall.py --idle 30 --trials 8`.

---

## >>> SEED PROMPT (paste this) <<<

Make core 0 ACTUALLY reach idle.S on the RPi4, then re-run the netstall -- this DECIDES the
session-11 blocking-sleep cure (a MAJOR OPEN CONCERN; never conclude it -- [[feedback_stall_open_concern]]).

READ FIRST: docs/NEXT_20260622_stall_session12_seed.md (this), then memory [[project_stall_session11]]
+ [[project_stall_hunt]] + [[project_stall_not_dvm_idle]] + docs/BACKLOG_core_agnostic_kernel.md.

SETTLED (s11, HW, do NOT re-derive): the ~32.4s wedge is a cold I-fetch of a blocked thread's
resume line after idle. The s11 cure = convert every core-0 yield-spinner to a blocking/timer-poll
wait so core 0 idles (warm line). Done: serverstats/flush block on a new HW-validated BCM2711
system-timer service (compare ch1, GIC SPI 97; discriminate its bound-notification IRQ by BADGE
not label); the ROOT EVENT LOOP (the seed-missed dominant spinner, mini-UART polling) + the xhci
kbd loop now timer-poll (/proc/rootpoll, /proc/xhci.poll) -> HALVED the stall (~5-6/10 -> ~2/8);
userspace `sleep` blocks (/proc/timersleep.1, new sleep on disk). BUT core 0 STILL never idles:
/proc/cpuacct over a 30s sleep = core0_hb 78% + pipe 21% + (idle) 0%, idle_lag=-1. So the cure is
BLOCKED, not refuted.

DO (decisive):
1. Convert the watchdog core-0 heartbeat (wd_core0_hb_fn, src/servers/watchdog.c:189) from a
   prio-200 yield-spin to a TIMER-POLL (stamp g_core0_hb_tick every ~100ms via aios_timer_sleep_us,
   idle between; it still freezes on a wedge so the core-1 watchdog still detects it).
2. Find + fix the pipe server burning 21% of core 0 during an idle sleep (it should block on
   seL4_Recv; read src/servers/pipe_server.c for a poll loop / busy path).
3. Confirm /proc/cpuacct (idle) > 0 and [STAGECP] idle_lag != -1, then re-run
   `scripts/netstall.py --idle 30 --trials 8` with timersleep.1. DECISIVE: core-0-idle + stalls->0
   = cure vindicated; core-0-idle + stalls persist = cure truly refuted (wedge is idle-fundamental)
   -> pivot to the core-agnostic kernel (backlogged) or real IRQ-driven mini-UART.

METHOD: stall = MAJOR OPEN CONCERN, never conclude. Board build 2872 (192.168.0.8), watchdog on,
healthy. sercap BEFORE any stall test; flash via pi_flash (network) or balenaEtcher of mksdcard.py
(pi_deploy of one binary FAILs -- rename EPERM); a netconsole connection itself can trigger a stall;
don't over-hammer netstall (fork-exhausts -> power-cycle). FULL QEMU gate before every flash. seL4
-> regenerate deps/patches/seL4-kernel.patch. Commit on main; Bryan pushes.
