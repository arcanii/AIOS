# SEED PROMPT -- session 11: the PRINCIPLED stall cure = BLOCKING SLEEP (kill the yield-spin)

Paste the SEED PROMPT block at the bottom into a fresh session. Everything above it is grounding.
The stall is a **MAJOR OPEN CONCERN -- never frame it as solved** ([[feedback_stall_open_concern]]).

---

## WHY THIS IS THE CURE (the chain, fully settled by session 10)

The ~32.4s wedge is **localized register-exact** (s10, HW): a **cold L1I/L2 instruction-cache refill of a
blocked thread's resume line** (deterministically `0x44c574` = netconsole's resume point) on the
`eret`->first-user-fetch path after idle. Clocked-but-stalled (`cyovf=1`), one instruction (`iovf=0`),
parked fabric; the TCB-restore `ldp` (D-side) was EXONERATED by the stage-13 splitter (5/5 I-side, 0/5
D-side). The fetch misses L1I, misses L2, goes to the **parked SCB fabric -> hangs to the ~32.4s
silicon force-complete**.

**Why the line is cold:** AIOS has **NO blocking sleep -- every "sleep" is a yield busy-wait.** So core 0
is NEVER idle: the prio-200 servers (`serverstats`, `flush`, the `xhci` event loop) yield-spin
continuously, and each `seL4_Yield` is a syscall whose kernel scheduler footprint cycles the cache. Over
the ~30s idle this **EVICTS the blocked threads' resume lines from L2**. At resume -> L2 miss -> parked
fabric -> hang.

**Two cure classes are now REFUTED on HW** (don't re-try them):
- **Prevent the fabric parking** (every keep-warm s4-8, incl. 5.3 GB/s coherent) -- REFUTED. The park is
  the A72 cluster's own ACE master quiescing (ACINACTM); software can't hold it active.
- **Keep the at-risk line warm** (s10 cure B, L2 keep-warm via `AT`+physmap touch) -- REFUTED. The
  `[IWARM]` diagnostic showed `inring=0` for EVERY wedge despite 65M touches: **netconsole blocks via the
  seL4 FASTPATH (`fastpath_reply_recv`), which bypasses `setThreadState`** where the capture hook lived,
  so the wedging thread was never captured. And the eviction is BROAD (a 2nd in-kernel wedge site
  appeared) -> targeted keep-warm is whack-a-mole.

**=> The ONLY principled cure left: STOP THE EVICTION by letting core 0 actually idle.** Replace the
yield-spin "sleeps" with real BLOCKING waits so the servers block; core 0 then falls to the
**tiny-footprint kernel idle loop** (`idle.S`, a few instructions + a heartbeat stamp), which does NOT
evict the resume line. The line stays in L2 -> the post-idle resume FETCH HITS L2 -> never reaches the
parked fabric -> **no cold I-fetch -> no stall.** A cache hit never touches the fabric, so this works
*regardless* of how deeply the fabric quiesces (which is why the old "no-WFI" mitigation was
counterproductive -- it never prevented the stall, it just churned the cache and CAUSED the cold line).

**This is a HYPOTHESIS to TEST, not a guaranteed cure.** It could under-deliver if (a) the `idle.S`
busy-loop itself evicts the line over 30s (unlikely -- tiny footprint), (b) some other periodic activity
still churns the cache, or (c) deep idle loses L2 state (A72 L2 should retain through WFI). The whole
point of building it is the netstall A/B.

---

## THE BUILD (3 parts; A is the prerequisite, B is the cure, C is optional)

### Part A -- a TIMER SERVICE for blocking timed waits (prerequisite)
AIOS (non-MCS seL4) has no userspace timer. Build one using the **BCM2711 System Timer** (free-running
1 MHz at `0xFE003000`: `CS`@+0x00, `CLO`@+0x04, `CHI`@+0x08, `C0..C3`@+0x0C..+0x18). **Use compare
channel 1 or 3 -- channels 0 and 2 are owned by the VPU firmware.** Each compare match raises a
peripheral IRQ routed to the GIC.
- **CHECK FIRST for a head start:** `deps/seL4_libs` / `deps/util_libs` **platsupport** very likely has a
  BCM2711 system-timer driver + the `ltimer` interface already (AIOS links seL4_libs). Reuse it rather
  than writing register pokes from scratch. Grep platsupport for `bcm`/`system_timer`/`ltimer`.
- **Map the MMIO:** add the system timer to the device map (`include/aios/device_map.h` +
  `src/boot/boot_device_map.c`, the same pattern as `dev_uart_vaddr` etc.).
- **Claim + bind the IRQ:** resolve the GIC INTID for the system-timer compare channel from
  `tools/dts/rpi4.dts` (BCM2711 system-timer compare IRQs are VC peripheral IRQs 0-3 -> GIC SPIs; pick
  the free channel's INTID). Bind it to a notification with the SAME IRQHandler->notification pattern
  AIOS already uses (see the root's UART IRQ bind in `src/aios_root.c` / the GENET IRQ bind in
  `src/boot/boot_net_init.c` + `src/net/net_driver.c`).
- **Server design (classic seL4 deferred-reply timer):** a BLOCKING server thread that binds the timer
  IRQ notification to its TCB and `seL4_Recv`s on a request endpoint (the bound-notification pattern --
  it wakes on EITHER a client request OR the timer IRQ). A "sleep N ms" client does
  `seL4_Call(timer_ep, deadline)`; the server saves the reply cap in a deadline-sorted queue, programs
  `C1`/`C3` for the EARLIEST deadline, and on the IRQ replies to all expired entries + reprograms. The
  Call IS the block -- the client sleeps until the reply. The server itself only wakes on real timer
  IRQs (minimal footprint -> it does NOT churn the cache).
- Pin it to core 0 like the other servers, but it BLOCKS (this is the whole point).

### Part B -- convert the yield-spinners to blocking timed waits (THE CURE)
Replace each yield-spin "sleep" with a blocking Call to the timer service. Do these and netstall-test
INCREMENTALLY (measure which evictors actually matter):
1. **`src/servers/serverstats.c`** `probe_sleep()` (yield-spin loop, ~line 73) -- the periodic
   full-server PING touches every server's code = likely a big evictor. Convert to a blocking wait for
   `PROBE_PERIOD_SEC`.
2. **`src/servers/flush_server.c`** `flush_sleep()` (yield-spin, ~line 50) -- blocking wait for
   `FLUSH_PERIOD_SEC`.
3. **`src/lib/posix_time.c`** `nanosleep`/`__aios_nanosleep` (currently a yield busy-wait) -- make it a
   real blocking sleep. Fixes ssh's nanosleeps + any future user. This is the general fix.
4. **`src/aios_root.c`** main loop -- already blocks on `main_ntfn_cap` (UART IRQ) when
   `irq_uart_active` (~line 599-608); confirm it genuinely blocks during the netstall idle (it should).
5. **`src/usb/xhci.c`** `xhci_kbd_driver_fn` (continuous busy event-loop, ~line 1175) -- the trickiest.
   IDEAL: block on the xHCI interrupter IRQ (event-driven). PRAGMATIC (probably enough for the cure):
   make it a TIMER-DRIVEN poll (wake every 1-2 ms via the timer service, drain the event ring, sleep)
   instead of a continuous spin -- collapses its footprint-cycling from continuous to 1-2 ms. Convert
   LAST, only if netstall still stalls after 1-4.

### Part C -- (optional, later) let core 0 WFI in `idle.S`
Once the servers block, core 0 falls to `idle.S` (currently a no-WFI busy-loop, deliberately). The cure
does NOT require WFI -- the `idle.S` footprint is tiny, so a warm line survives even with busy-idle. But
re-enabling WFI saves power + is now safe under the new understanding (a warm-line resume is a cache
HIT, independent of fabric quiescence). Test busy-idle FIRST (smaller change); try WFI only after the
cure is confirmed. NOTE: `AIOS_SIBLING_TIMER_MASK` keeps secondaries in `idle.S` -- leave it.

---

## VALIDATION (the A/B)
- **Gold A/B:** `scripts/netstall.py --idle 30 --trials 10` + a timestamped `ping 192.168.0.8` oracle.
  Baseline (v0.4.294, yield-spin) = stalls reliably (~5-6/10, deterministic `0x44c574`). CURE = stalls
  drop toward 0.
- **Confirm core 0 actually idles** (the cure mechanism), three independent ways:
  1. `/proc/cpuacct` busy% should DROP toward 0 during the netstall `sleep 30` (servers now blocking).
  2. The `[STAGECP]` `idle_lag` for **core 0** changes from `-1` (it "never idles" today) to a real
     value -- i.e. core 0 reaches `idle.S` and stamps its heartbeat.
  3. If a stall still fires, the `[STAGECP]`/`[IWARM]` line shows whether `0x44c574` is still going cold.
- The localization instrumentation is all in place + default-on (`[STAGECP]` PMU + breadcrumb + the
  stage-13 splitter). The refuted keep-warm is `AIOS_IWARM 0` (errata.c) -- leave it off.

---

## STATE AT THE START OF SESSION 11
- **Board: v0.4.294 build 2833** (192.168.0.8), keep-warm OFF, all diagnostics kept, HEALTHY
  (power-cycled after a fork-exhaustion). AIOS SD in the Pi. Spare SD has the e1 reproducer (RPi OS was
  Etchered over -- re-flash RPi OS if you want the e3 WFI test).
- **All session-10 work committed on `main`** (Bryan pushes): `59ebc9e` (localization), `baae1fe` (e1
  cold-I-fetch test), `3843593` (cure attempts refuted, v0.4.294). seL4 diff in
  `deps/patches/seL4-kernel.patch` (1761 lines). Full detail: `docs/NEXT_20260622_stall_session10_relocalize.md`
  + memory [[project_stall_not_dvm_idle]].
- **Also still open (independent, high-value):** FORK-EXHAUSTION auto-recovery -- the board
  fork-exhausted AGAIN under heavy netstall hammering this session. Wire the dead `reap_check()`
  (`src/process/reap.c:113`) as sweep-on-shortage + retry in `do_fork`, + a getty crash-loop
  circuit-breaker. Do this if the blocking-sleep work stalls or as a warm-up.

## METHOD / DISCIPLINE (hard-won)
- Stall = MAJOR OPEN CONCERN, never conclude it ([[feedback_stall_open_concern]]).
- HW: `scripts/sercap.py /tmp/x.log` BEFORE any stall test (ONE serial reader); deploy over the net with
  `scripts/pi_flash.py --build` (it rebuilds + mkkernel8 + push + reboot + verifies the banner); wait for
  PASS; gold A/B = pingmon + `netstall --idle 30 --trials 10`. **Do NOT over-hammer netstall -- it
  fork-exhausts the box (-> power-cycle).** Pace it; short runs.
- FULL QEMU gate before every flash: `smp_qemu_test.py` (4/5 baseline), `net_socket_qemu_test.py` (8/8),
  `netd_qemu_test.py` (10/10), `shmring_qemu_test.py` (25/26). seL4 changes live in the sibling repo
  `/Users/bryan/Desktop/github_repos/seL4` (symlinked `deps/kernel`); regenerate the patch with
  `git -C ../seL4 diff > deps/patches/seL4-kernel.patch` before committing. Commit on `main`; Bryan pushes.

---

## >>> SEED PROMPT (paste this) <<<

Implement the PRINCIPLED cure for the RPi4 ~32.4s stall: replace AIOS's everything-yield-spins design
with real BLOCKING SLEEP so core 0 idles and stops evicting blocked threads' resume lines. This is a
MAJOR OPEN CONCERN -- NEVER frame it as solved ([[feedback_stall_open_concern]]).

READ FIRST: docs/NEXT_20260622_stall_session11_blocking_sleep_seed.md (full plan above), then
docs/NEXT_20260622_stall_session10_relocalize.md (the localization + the two refuted cure classes), then
memory [[project_stall_not_dvm_idle]] + [[project_stall_hunt]] + [[feedback_stall_open_concern]].

SETTLED (s10, do NOT re-derive): the wedge is a COLD L1I/L2 INSTRUCTION-cache refill of a blocked
thread's resume line (deterministically 0x44c574 = netconsole) after idle -- I-side, clocked, the TCB
ldp exonerated by the stage-13 splitter. It's cold because AIOS has NO blocking sleep: the prio-200
servers yield-spin continuously, churning the cache (via the per-yield kernel-scheduler footprint) and
evicting the resume line over the 30s idle; at resume the fetch misses L2 -> parked SCB fabric -> hangs.
BOTH other cure classes are REFUTED on HW: prevent-fabric-park (keep-warms, s4-8) and keep-the-line-warm
(s10 cure B -- the [IWARM] diag proved netconsole blocks via the seL4 FASTPATH, bypassing the
setThreadState capture; eviction is broad -> whack-a-mole). The ONLY principled cure left = stop the
eviction by letting core 0 actually idle.

DO (the cure):
1. Part A -- build a TIMER SERVICE for blocking timed waits (prerequisite). Use the BCM2711 System Timer
   (0xFE003000, free channel 1 or 3 -- 0/2 are VPU-owned) bound to a notification via AIOS's existing
   IRQHandler->notification pattern (see the UART IRQ bind in aios_root.c / GENET in boot_net_init.c).
   CHECK deps/seL4_libs + deps/util_libs platsupport for an existing BCM2711 system-timer / ltimer driver
   FIRST (big head start). Classic deferred-reply server: client seL4_Call(timer_ep, deadline) blocks
   until the server replies at the deadline; server binds the timer IRQ ntfn to its TCB + Recv on the
   request EP, queues reply caps by deadline, programs the compare reg for the earliest, replies to
   expired ones on the IRQ.
2. Part B -- convert the yield-spinners to blocking Calls, INCREMENTALLY + netstall-test each:
   serverstats.c probe_sleep, flush_server.c flush_sleep, posix_time.c nanosleep, confirm aios_root.c
   already blocks on UART IRQ; xhci.c busy event-loop LAST (timer-driven 1-2ms poll, or full IRQ-driven).
3. Part C (optional, later) -- re-enable WFI in idle.S once the cure is confirmed (busy-idle works too;
   the idle.S footprint is tiny). Leave AIOS_SIBLING_TIMER_MASK on.

VALIDATE: netstall --idle 30 --trials 10 + ping oracle -- stalls should drop toward 0 (baseline v0.4.294
= ~5-6/10 at 0x44c574). Confirm core 0 idles via /proc/cpuacct busy%->0 during the sleep AND the
[STAGECP] core-0 idle_lag changing from -1. This is a HYPOTHESIS test (a warm line is a cache hit -> no
fabric -> no hang, regardless of fabric quiescence); it may under-deliver -- measure honestly.

METHOD: stall = MAJOR OPEN CONCERN, never conclude. Board on v0.4.294 build 2833 (192.168.0.8), healthy.
HW: sercap BEFORE any stall test; pi_flash.py --build to deploy (wait for banner PASS); don't over-hammer
netstall (fork-exhausts -> power-cycle). FULL QEMU gate before every flash (smp 4/5, socket 8/8, netd
10/10, shmring 25/26). seL4 -> deps/kernel (sibling repo); regenerate deps/patches/seL4-kernel.patch
before committing. Commit on main; Bryan pushes. Also open (warm-up / fallback): fork-exhaustion
auto-recovery (reap_check reap.c:113 + getty crash-loop circuit-breaker).
