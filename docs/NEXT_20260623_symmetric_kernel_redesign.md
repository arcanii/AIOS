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

## 9. IMMEDIATE NEXT STEPS

1. **Recover the board:** revert the s12 changes (aios_root.c, netconsole.c, timer_server.c,
   watchdog.c, xhci.c) to the s11 driveable state and reflash — the symmetric design will NOT use the
   blocking relays / timersleep-on. (Build 2881 is un-driveable; the board is currently dead.)
2. **Phase A step 1 on QEMU:** build the allocator lock + the slot-bitmap-tear host repro; prove it.
3. Then Phase A steps 2-3 (un-pin + distribute + the per-core-wedge measurement) — the gate that
   tells us whether confinement is real before investing in Phase B.
