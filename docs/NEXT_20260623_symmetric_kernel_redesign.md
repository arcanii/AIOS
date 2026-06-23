# Symmetric / core-agnostic kernel — REDESIGN (2026-06-23, session 12)

Supersedes the framing in `docs/BACKLOG_core_agnostic_kernel.md` (keep it for the original
research; this doc is the actionable plan, sharpened by the session-12 HW result). The stall
is a **MAJOR OPEN CONCERN** — this design does not cure it; it bounds its blast radius.
Memory: [[project_stall_session12]] · [[project_stall_session11]] · [[project_stall_hunt]] ·
[[project_stall_not_dvm_idle]] · [[feedback_stall_open_concern]].

---

## 1. WHY WE PIVOT (the session-12 result)

The ~32.4s RPi4 freeze is a **per-core, idle→wake, silicon-fundamental wedge**: a core that idles
~30s and then resumes does a cold I-fetch of the resume line through the parked BCM2711 SCB fabric
and hangs ~32.4s. Eleven sessions refuted every *prevention* lever (keep-warm ×N, clock, voltage,
registers, TLB/ASID redesign). Session 12 tested the last prevention idea — **let core 0 idle so
its resume line stays warm** (blocking sleep instead of yield-spin) — and it is dead two ways:

1. **Core 0 cannot be made to idle.** `idle_lag = -1` at both 20 ms and 500 ms poll intervals.
   Timer-polling merely *relocates* the spin into the timer server + the per-tick wake/re-park
   cycle; idle.S never runs.
2. **Blocking is self-defeating.** To idle core 0 the I/O relays must *block* rather than
   yield-spin — but "block" means **block on the system-timer service, which lives on core 0 and
   freezes with it during a wedge**. After a wedge the relay's blocked nap is never serviced →
   netconsole/SSH wedge permanently → the board becomes un-driveable and eventually dies. The old
   yield-spin relays survived a wedge (they resume) but kept core 0 busy.

**The load-bearing lesson: a cross-core *blocking* dependency propagates a wedge.** This kills
"prevent the wedge" and directly shapes "survive the wedge." We stop trying to stop the wedge and
instead make a one-core wedge a *one-core* event.

---

## 2. THE GOAL

Today every server, the net stack, the shell, the keyboard loop, and the timer all live on
**core 0** (ROOT_CORE), so the per-core wedge that hits core 0 freezes the whole cluster. The goal
is **blast-radius reduction**: when a core wedges for ~32 s, the *other* cores keep serving — an
interactive session on a healthy core stays responsive, and the wedged core rejoins after ~32 s.

This is viable because (proven, see §6) **the seL4 big kernel lock is released before `eret`-to-user**
(`c_traps.c:57` / `fastpath.h:181`), so a core wedged on the post-`eret` user-side resume fetch holds
**no lock** — other cores can enter the kernel and schedule freely. The cluster-freeze is an
*architecture* problem (all work on one core), not a lock-ownership problem.

Non-goals: curing the wedge (impossible); making a wedge *invisible* (that needs replication/failover,
§5 Phase C); multi-core *throughput* (the BKL still serializes kernel entry — distribution is for
resilience, and IPC-bound work will contend, measured 30→6, see `spawn_util.c:47-51`).

---

## 3. DESIGN PRINCIPLES (session-12-informed)

1. **No cross-core blocking dependency on a single-core service.** If a thread on core N blocks
   (IPC `Call` or timer `sleep`) on a service hosted on core M, a wedge of M stalls N for ≥32 s
   (and risks the permanent-wedge failure mode we hit). Every *universal* blocking dependency must
   be **per-core**.
2. **Per-core instances for the universal deps.** The two every thread hits: the **timer** (every
   `sleep`/poll) and the **process manager + allocator** (every fork/exec/exit). Give each core its
   own. This is the core of the redesign — and exactly what session 12 proved is required (a shared
   core-0 timer is what wedged the relays).
3. **Sacrificial proxies for unavoidable cross-core blocking.** The true hardware singletons (one
   block device, one NIC) can't be replicated. A core that must `Call` them does so through an
   *expendable* proxy thread, so a wedge of the singleton's core parks only the proxy, not the
   caller's main loop. This pattern already exists and works: `net_cleanup_proxy_fn`
   (`pipe_server.c`) absorbs a blocking net `Call` exactly this way ("a wedged net server parks
   only this expendable thread, never the pipe_server loop").
4. **Generalize the survive primitive.** The MVD-1 watchdog (`watchdog.c`) already keeps core 1
   alive through a core-0 wedge: timer-mask the core in the kernel (`AIOS_SIBLING_TIMER_MASK`,
   `boot.c:298-318`) + pure-userspace loop + out-of-band MMIO. The symmetric version wants *every*
   core able to keep serving while a peer wedges — same three ingredients, applied symmetrically.
5. **Confine, don't share, the wedge trigger.** The wedge fires on *teardown-after-idle* (a process
   exit / page-unmap after the cores idled). Process teardown runs on the **process manager's**
   core (it reaps + unmaps). So sharding the process manager per-core (principle 2) also shards the
   *teardown trigger* per-core: a teardown-wedge on core N is confined to core N's session.

---

## 4. CURRENT BARRIERS (grounded, file:line)

| # | Barrier | Where | Status for redesign |
|---|---------|-------|---------------------|
| B1 | **Lock-free single-owner allocman/vka** — all root threads share one `vka`/`allocman` with **zero locking**; SMP tears the CSpace-slot bitmap RMW (v0.4.178 SSH-2nd-conn bug). Enforced purely by the core-0 pin. | `aios_root.c:46-49`,`:235-242`; comment `boot_services.c:21-35`; pins at `boot_services.c:48,82,203,257`,`timer_server.c:299`,`watchdog.c:304` | **THE hard barrier.** Lock it (ticket/spin) or give each core an allocman sub-pool (§5). |
| B2 | **Singleton hardware/state owners** create cross-core blocking deps: **timer** (one systimer ch1 IRQ→one TCB, `timer_server.c:52,291-300`), **fs** (one ext2 ctx + block dev), **net** (one NIC + RX IRQ), **pipe** (one process table + fork/signals/reap). | `timer_server.c`, `fs_thread_fn`, `net_server_fn`, `pipe_server.c` | Timer + pipe → **per-core** (§5). fs/net → singletons reached via **proxies** (§5 Phase C). |
| B3 | **Keyboard/console input on the core-0 root loop** (polls mini-UART; blocks on the core-0 timer between polls). | `aios_root.c:526-634`,`:629` | Move per-session input off the root loop, or accept console on a "home" core. |
| B4 | **BKL serializes kernel entry** — distribution contends rather than parallelizes for IPC-bound work (30→6 measured). | `c_traps.c` NODE_LOCK; `spawn_util.c:47-51` | Accepted: resilience, not throughput. Keep distribution opt-in / coarse. |

**Already core-agnostic / done** (do not redo): IPC endpoints are global caps callable from any
core (`root_shared.h`; coresched relies on it); user-proc distribution exists (`aios_assign_core`,
`spawn_util.c:41-72`, default-off for throughput only); multi-core teardown is correct
(residency-masked TLB shootdown + ASID-generation, `vspace.c`; `AIOS_ASID_GEN` gated non-HYP/RPi4);
the BKL is released before `eret` (§6); the timer-masked pure-userspace survive model works
(`watchdog.c` + `boot.c` `AIOS_SIBLING_TIMER_MASK`). **The Pi ships `KernelMaxNumNodes=4`**
(`settings-rpi4.cmake:43`) — the multi-core paths are live on hardware.

---

## 5. PROPOSED ARCHITECTURE + PHASED PLAN

The end state: **N "home cores," each self-contained** — its own allocator sub-pool, its own timer,
its own process manager, and the interactive sessions assigned to it — so a wedge on a home core
stalls only that core's sessions. A small set of true singletons (fs, net) is reached through
sacrificial proxies. Get there in phases, each independently testable; stop at any phase that buys
enough resilience.

### Phase A — lock the allocator, un-pin, distribute (the backlog's "Isolation"; partial)
1. Add a lock around allocman/vka access (ticket or CLH-style spinlock; or a coarse mutex on the
   alloc path). **Host-test the slot-bitmap-tear repro first** (the SSH-2nd-connection scenario,
   v0.4.178) to prove the lock closes it.
2. Drop the `seL4_TCB_SetAffinity(…, ROOT_CORE)` pins (B1 list) and distribute servers across cores.
3. Measure: force a core-N wedge (a `sleep 30; teardown` on core N) and confirm a server on another
   core still answers.

**Honest limit of Phase A alone:** it does *not* deliver isolation, because the universal blocking
deps (timer on core 0, pipe on core 0) remain single-core — a wedge of *their* core still freezes
every caller. Phase A is necessary groundwork (un-pinning) but Phases B is where resilience appears.
Do A only as the enabling step for B.

### Phase B — per-core universal services (the real isolation; session-12's mandate)
1. **Per-core timer.** The `AIOS_SIBLING_TIMER_MASK` already masks the ARM **generic-timer PPI**
   (CNTV) on secondaries (it's free — seL4 doesn't use it there). Expose that per-core PPI to a
   per-core userspace timer thread (an `IRQHandler` for the core's CNTV PPI, bound to that core's
   timer TCB). Now every thread sleeps on **its own core's** timer → no cross-core timer dep → the
   exact failure that wedged the relays in s12 is gone. (Falls back to the shared systimer / yield
   only if a core's PPI can't be claimed.) *Kernel change: route the per-core CNTV PPI to userspace.*
2. **Per-core process manager + allocator sub-pool.** Each home core runs a pipe-server-like
   instance owning the procs assigned to it, backed by its own allocman sub-pool (B1). A shell on
   core 2 forks → core-2 manager → core-2 sub-pool → runs on core 2 → exits → **core-2 manager reaps
   (teardown on core 2)**. The teardown-wedge is now confined to core 2 (principle 5). Cross-core
   process ops (rare: a global `kill`, `ps`) go through a proxy or a lock-step query.
3. **Assign interactive sessions to home cores.** netconsole/sshd/getty accept a connection and bind
   the session (its shell + children + timer) to a home core, round-robin. A wedge takes down only
   the sessions on that core; new connections land on healthy cores.

### Phase C — singleton resilience (fs, net) [optional / harder]
The block device and NIC are true singletons. Options, in increasing ambition:
- **C1 (proxy isolation):** every cross-core `Call` to fs/net goes through a sacrificial proxy
  (generalize `net_cleanup_proxy`), so a wedge of the fs/net core parks only proxies, not callers'
  main loops. Callers see a slow/failed op, not a wedge. Cheapest; bounds the damage.
- **C2 (read replication):** per-core read caches for fs (block cache shards) so reads don't cross
  cores; writes go to the singleton via C1. Most fs traffic is reads.
- **C3 (failover):** replicate fs/net ownership with health detection + idempotent re-issue so a
  sibling takes over within ms. Big (state replication + routing); likely out of scope.

### Phase D — generalize the watchdog to symmetric peer-survival
Each core runs the timer-masked pure-userspace liveness check for its peers (today only core 1
watches core 0). On a detected peer wedge: light the OOB signal, optionally migrate that core's
pending sessions to a healthy core (if state allows), and let the wedged core rejoin after ~32 s.

---

## 6. ENABLING FACTS (proven this session, don't re-verify)

- **BKL released before eret:** slowpath `restore_user_context` `NODE_UNLOCK_IF_HELD` `c_traps.c:57`
  then `eret` `:90`; fastpath `NODE_UNLOCK` `fastpath.h:181` then `eret` `:218`. Entry lock:
  `NODE_LOCK_SYS`/`NODE_LOCK_IRQ` in `c_traps.c`. ⇒ a user-side wedge holds no lock.
- **IPC endpoints core-agnostic:** global caps in `root_shared.h`, `Call`-able from any core;
  coresched already has user procs on cores 1-3 calling core-0 servers.
- **Teardown multi-core-correct:** residency-masked shootdown (`vspace.c:1310,1341`) + ASID
  generation (`vspace.c:952-1021`, gen wrap `:961-972`) + fastpath residency hook (`fastpath.h:63`);
  `AIOS_ASID_GEN` non-HYP only (`tlb.h:132-139`).
- **Survive primitive:** `AIOS_SIBLING_TIMER_MASK` default-on masks the timer PPI on all secondaries
  (`boot.c:298-318`); IPIs stay enabled; core-1 watchdog is pure-userspace MMIO (`watchdog.c`).
- **4-core on HW:** `KernelMaxNumNodes=4` (`settings-rpi4.cmake:43`, build-rpi4 cache).

---

## 7. VALIDATION

- **Per-core wedge harness:** pin a `sleep 30; echo` teardown to core N (via the home-core binding)
  and, *concurrently from a different connection bound to core M*, run a steady command stream; the
  M-stream must stay responsive while the N-core shows a `[WDOG]`/`[STAGECP]` wedge. The existing
  `[STAGECP] allidle=[...]` per-core oracle + the watchdog already report per-core liveness.
- **The s12 anti-pattern as a regression test:** confirm a thread on core M sleeping does NOT stall
  when core N wedges (proves the per-core timer removed the cross-core dep).
- **QEMU first** (smp 4): the slot-bitmap-tear repro for the allocator lock; per-core timer claim;
  session-to-home-core binding. Then HW. **Keep the board driveable** — do NOT ship blocking relays
  that depend on a cross-core timer (the s12 mistake); the yield-spin relays survive a wedge.

---

## 8. RISKS / OPEN QUESTIONS

- **Per-core timer feasibility** (Phase B1): needs a kernel change to route the per-core CNTV PPI to
  userspace. The PPI is free on secondaries (masked from seL4) — but core 0 uses CNTV for the
  preemption tick; core 0's timer service may need the systimer while secondaries use CNTV, or the
  tick + userspace-timer must coexist. Prototype on QEMU first.
- **The singletons remain a residual freeze risk** (fs/net): a wedge of the fs/net core still stalls
  fs/net callers for ~32 s unless C1 proxies are in place. Interactive responsiveness (shell + already
  -loaded binaries) survives; a *new* binary load or socket op during an fs/net-core wedge does not.
- **IPC has no timeout** (non-MCS): a `Call` to a wedged server wedges the caller. This is *the*
  reason cross-core blocking deps are dangerous and why proxies (principle 3) are mandatory for the
  singletons. Re-evaluate if MCS (timeouts) ever lands.
- **Throughput** (B4): IPC-bound distribution contends on the BKL. Keep distribution coarse
  (per-session home cores), not fine (per-syscall). This is resilience, not parallelism.
- **Does confinement actually hold on HW?** The whole premise is that a per-core wedge stays
  per-core once the cross-core blocking deps are removed. This is *unproven on HW* — Phase A's
  measurement (force a wedge, confirm a peer serves) is the first real test and should gate the rest.
- **Complexity vs payoff:** Phase B (per-core process manager + allocator) is a substantial rewrite.
  Phase A + C1 might deliver "most interactive sessions survive a wedge" for far less. Re-scope after
  Phase A's measurement.

---

## 9. NEXT STEPS / PROGRESS

- [x] **Board revert:** s12 changes (aios_root.c, netconsole.c, timer_server.c, watchdog.c, xhci.c)
  reverted to the s11 driveable base (working tree clean). Board still needs a power-cycle + a
  driveable reflash to revive (build 2881 is dead); do that before any HW step below.
- [x] **Phase A step 1 — allocator lock (commit 4fdbeee):** `src/boot/vka_lock.c` spinlock trampoline
  layer over the global vka, installed at `aios_root.c` after `allocman_make_vka`. Proven by
  `scripts/allocman_lock_host_test.c` (buggy RMW tears ~4000 handouts/run; locked = 0). QEMU gate
  unchanged at baseline (socket 8/8, shmring 25/26, smp 4/5). Inert under core-0 pinning; load-bearing
  once servers distribute.
- [x] **Phase A step 2 — un-pin + in-situ proof (QEMU) — DONE (build 2885, 2026-06-23):** two
  default-safe runtime knobs added: **`/proc/distribute[.0|.1]`** (`spawn_util.c` `aios_server_pin` +
  `distribute_cmd`) un-pins the root servers and round-robins them across cores 0..N-1, re-pinning the
  *live* TCBs on toggle; **`/proc/vkalock[.0|.1]`** (`vka_lock.c` `g_vka_lock_enabled`) A/B-bypasses the
  allocator lock. The 8 server pin-sites (`start_server_thread` ×7, root init, net_server, display,
  flush, serverstats, netd_listener) now route through `aios_server_pin`; **deliberately KEPT pinned:**
  `core0_hb` (core 0, stall detector), `core1_wd` (WD_CORE, kernel-timer-masked survive thread), and the
  **shared system timer** (a single shared timer on a wedged core would freeze every sleeper on every
  core — the s12 cross-core-blocking failure; per-core timer is Phase B). Default OFF == byte-for-byte
  today's behavior; baseline QEMU gate unchanged (SMP 4/5, socket 8/8, shmring 25/26).
  **RESULTS (`scripts/distribute_qemu_test.py`, per-config fresh boots, width-3 fork storm ×10):**
  (1) pinned 10/10 clean; (2) **distribute+lock 10/10 clean + healthy — the proof: servers spread
  across all 4 cores, concurrent cross-core forks still compute correctly, no corruption**; (3)
  distribute+bypass ALSO 10/10 clean — i.e. **the rare CSpace-slot tear does NOT reproduce at in-situ
  allocation rates** (~30 forks) even with the lock bypassed; it needs the sustained 4-thread hammering
  of `scripts/allocman_lock_host_test.c` (4000 dups → 0) to manifest. **So the HOST test stays the
  canonical lock proof; the in-situ test proves un-pinning is structurally sound + correct under the
  lock.** **KEY NEW FINDING (confirms barrier B4 empirically):** full distribution makes IPC **~2.6×
  slower** (worst round 44 s vs 17 s pinned) — the BKL serializes kernel entry, so un-pinning *contends*
  rather than parallelizes. On QEMU this is amplified because `flush`/`serverstats` use the yield-spin
  timer-fallback (no systimer MMIO) and busy-monopolize a timer-masked secondary core — a HW-vs-QEMU
  artifact, but it flags the design rule: **distribution must be SURGICAL** (don't distribute the
  console/timer/yield-spinning servers; only the ones whose isolation actually buys resilience). No
  faults/panics in any config — the shared-root-vspace race (the s12 caveat) did NOT surface in fork/
  exec (they use per-child vspaces); a root-vspace lock is still a latent Phase-B item if concurrent
  *root*-buffer allocs are ever distributed.
- [x] **Phase A step 3 — confinement-gate MECHANISM built (commits 3cb91f4 + 57c88d6; QEMU 7/7 + 5/5):**
  surgical distribution (`/proc/distribute.2`) + per-core bind (`/proc/pincore.N`) + the **wedge-survival
  worker** (`/proc/confine.N`, src/servers/watchdog.c). The worker is a thread on core N that does a
  SYSCALL (`seL4_Yield` → NODE_LOCK) every iteration; the survive-capable core-1 watchdog snapshots its
  counter across a `[WDOG]` stall and reports "worker(core N) advanced=K during the wedge". This makes the
  gate's observable OUT-OF-BAND (the console/pipe path can't be the observable — it depends on the single
  pipe_server). Default disarmed.
- [ ] **Phase A step 3 — RUN THE GATE (HW, the decisive experiment).** The board is REVIVED
  (192.168.0.8). The gate resolves the **BKL-during-wedge** question that gates Phase B: the records
  contradict — the timer-mask patch comments say the wedge HOLDS the BKL (a sibling's kernel entry blocks
  ~32 s; that's why the mask exists), the redesign doc §6 says the BKL is released before `eret`. The
  watchdog only survives because it's PURE-USERSPACE; this tests whether a secondary doing SYSCALLS
  survives. **Procedure** (flash-free, board alive):
  1. `ninja -C build-rpi4` → `python3 scripts/pi_flash.py --build` (builds kernel8 + swaps over net +
     reboots; wait for the banner-PASS). sercap on `/dev/cu.usbserial-0001` FIRST.
  2. Verify `cat /proc/version` (new build#), `cat /proc/confine` (armed=0), `cat /proc/watchdog` (enabled).
  3. Let the board SETTLE ~1 min (calm; the stall is teardown-triggered — do NOT over-probe).
  4. `cat /proc/confine.2` — arm the worker on core 2 (a secondary ≠ core 0 wedge-core, ≠ core 1 WD_CORE).
  5. Force a core-0 teardown-after-idle wedge: let it idle ~30–40 s, then ONE netconsole connect+disconnect
     (the disconnect's shell teardown-after-idle wedges core 0 — the s12 self-stall trigger).
  6. Read sercap for: `[WDOG] core0 recovered after <ms>ms; confine worker(core 2) advanced=K during the wedge`.
     **K > 0 ⇒ the core-2 syscalls completed DURING the wedge ⇒ work on a secondary SURVIVES a peer wedge
     ⇒ the symmetric-kernel premise HOLDS, Phase B viable. K == 0 ⇒ the worker froze on a kernel entry ⇒
     BKL-held wedge ⇒ the premise is broken (the wedge must be made BKL-free first).**
  7. `cat /proc/confine.r` to disarm (the worker hammers the BKL; armed only for the run).
  This gate decides whether confinement is real before investing in Phase B. **Run `sercap` BEFORE arming;
  do not over-probe netconsole. The hwdog auto-resets a total wedge (~63 s) — sercap catches the timeline up
  to any freeze.**

---

## 11. SEED PROMPT (next session -- session 14)

Paste the block below into a fresh session. Everything above is grounding. The stall is a MAJOR OPEN
CONCERN ([[feedback_stall_open_concern]]) -- session 13 found a STRONG but UNCONFIRMED cure lead; the
job now is to CONFIRM or REFUTE it rigorously, never to assume it.

>>> SEED PROMPT <<<

CONFIRM (or refute) the session-13 PREWARM stall-cure lead: a pre-NODE_LOCK cold-fabric touch at each
seL4 kernel entry gave ZERO ~32.4s core-0 wedges on HW vs ~6 without it. This may be the cure that
eluded 12 sessions -- treat it as UNCONFIRMED and demand extraordinary proof.

READ FIRST: docs/NEXT_20260623_symmetric_kernel_redesign.md (the prewarm section in S9 + the gate
result), then memory [[project_stall_session12]] (the gate + prewarm detail) + [[project_stall_hunt]]
+ [[feedback_stall_open_concern]].

SETTLED (session 13, do NOT re-derive):
- The ~32.4s wedge SPLITS BY TYPE (HW confinement gate, /proc/confine worker on core 2): the IDLE->WAKE
  wedge ([STAGECP] core=0 prev=9 this=11 = a cold load INSIDE the seL4 handler, AFTER NODE_LOCK) holds
  the BKL -> CLUSTER-FREEZE (the syscall-doing worker froze, advanced=0). The TEARDOWN-reap wedge
  (prev=13 this=9 = post-eret user fetch, pre-lock) is CONFINED (worker ran 683k syscalls through it).
- THE PREWARM (commit 0a4d045): aios_fabric_prewarm() (deps/kernel errata.c: per-core cold-scratch read
  + dsb sy) called via AIOS_PRELOCK_PREWARM() BEFORE NODE_LOCK in the 4 slowpath/IRQ entries of
  c_traps.c (NOT the IPC fastpath). It relocates the one-time fabric wake out of the BKL-held window. HW
  A/B (serial-independent /proc/laststall): build 2896 (prewarm) = 0 core-0 wedges over ~14 idle->teardown
  cycles; build 2893 (no prewarm) = ~6. Only the prewarm differs; worker-confound ruled out (armed both);
  teardowns confirmed (ASIDGEN climbed). APPEARS TO PREVENT the wedge -- coherent because it is CORE 0's
  OWN per-entry ACE/snoop-master touch (every prior keep-warm was secondary/external -> couldn't). NOT
  the refuted s10 IWARM (that held L2 lines; this relocates the wake). Board is on build 2896 (prewarm
  ON), healthy, 192.168.0.8.

DO (the confirmation, in order):
1. REPRODUCIBILITY A/B (serial-independent, robust to the flaky FTDI): in deps/kernel errata.c set
   AIOS_FABRIC_PREWARM 0, rebuild build-rpi4, pi_flash.py --build, then arm /proc/confine.2 and drive
   ~10 idle->teardown cycles (connect+disconnect, ~33s idle each); read /proc/laststall -- wedges should
   RETURN. Then set AIOS_FABRIC_PREWARM 1, re-flash, re-run -> wedges GONE again. That nails it.
2. SOAK: longer idle windows (60-120s) + more cycles on the prewarm kernel; confirm /proc/laststall stays
   "none detected". (More n on the 0-wedges side.)
3. RUNTIME TOGGLE (cleanest, for a same-boot A/B): make the prewarm gated by a kernel global settable
   from userspace -- needs a small kernel<->userspace channel (the flag is a kernel global; non-trivial).
4. PERF: measure the cost of the dsb sy per slowpath entry (the pipeline ceiling / IPC throughput); try a
   lighter barrier (dsb ish, or the cacheable touch with NO dsb) and re-confirm it still prevents the wedge.
   If a cheaper variant works, prefer it.
5. IF CONFIRMED: this changes everything -- the symmetric-kernel "survive the wedge" work (Phase B per-core
   timer etc.) becomes OPTIONAL (you cured the wedge instead of surviving it). Re-scope. The Phase A
   distribution knobs (/proc/distribute, /proc/placement, /proc/pincore) remain useful but no longer
   load-bearing for stall survival.

METHOD / STATE: Board build 2896 (prewarm ON) at 192.168.0.8. The FTDI serial adapter is BUMP-SENSITIVE
(dropped twice mid-run s13) -> use /proc/laststall over netconsole as the SERIAL-INDEPENDENT wedge
counter (it reads the userspace watchdog's g_wd_stalls; "none detected" = 0 wedges). /proc/confine.2 arms
the probe worker on core 2, .r disarms; /proc/confine ticks must be CLIMBING before a run (verify the
worker is actually scheduled). pi_flash.py --build = flash-free kernel swap (push over net + fatswap +
reboot + verify /proc/version) -- the board is alive so NO balenaEtcher needed. seL4 kernel edits live in
deps/kernel (gitignored, own git) -> regenerate the tracked patch via `git -C deps/kernel diff >
deps/patches/seL4-kernel.patch` and commit THAT. FULL QEMU gate before every flash (SMP 4/5 baseline,
socket 8/8). Commit on main; Bryan pushes. Even if confirmed, validate broadly (multiple workloads,
power cycles) before concluding -- the stall has burned every premature "cure".
