# Evaluation: AIOS ARM SMP situation + limitations to a true SMP kernel (2026-06-23, s14)

Codebase-grounded (file:line verified). Corrects one staleness vs the redesign doc: the **confine
gate HAS been run** (s13 + s14), so Phase-B viability is no longer an open gate -- see below.

## 1. Where SMP stands today
- **Built 4-core:** `KernelMaxNumNodes=4` (settings-rpi4.cmake:43); CLH big-kernel-lock live on every
  entry (c_traps.c NODE_LOCK_SYS/IRQ), released before eret (c_traps.c:57/90, fastpath.h:181/218).
- **Multi-core-correct teardown** (residency-masked shootdown + ASID generation, vspace.c) -- live on HW.
- **Phase A groundwork all IN, but DEFAULT-OFF / pinned to core 0:** allocator lock (vka_lock.c,
  default-ON, host-proven 4000->0 slot tears); `/proc/distribute[.0|.1|.2]` un-pin/round-robin/surgical
  (spawn_util.c, default 0=pinned); `/proc/coresched` (default 0); `/proc/pincore` (default -1);
  `/proc/placement` load-aware policy (default off). Servers pinned to core 0; watchdog core 1.
- **Prewarm (s14)** default-ON, directionally confirmed (0 wedges/40 cycles ON vs 4/~36 OFF).
- **Default posture = single-core-pinned, and that posture is STABLE.** Multi-core is opt-in/tested.

## 2. The confine-gate result (the correction -- Phase B premise is ANSWERED, and it is SPLIT)
The redesign doc §9 listed the gate as the pending decision experiment. It has since RUN (s13 build
2893; s14 OFF_B2 build 2899, /proc/confine worker on core 2). Result is **split by wedge type**:
- **idle->wake cluster-freeze** (`[STAGECP] prev=9 this=11`): worker FROZE (advanced~=0), core 2
  co-wedged -> **BKL IS held during this wedge -> NOT confinable.**
- **teardown-reap** (`prev=13 this=14`): worker ran FULL-RATE (advanced ~683k through the 32s wedge)
  -> **BKL-free -> CONFINED to core 0 -> a secondary keeps serving.**
So Phase B confinement is REAL for teardown wedges, not for idle->wake. **The s14 prewarm PREVENTS the
idle->wake type** -- i.e. it covers exactly the wedge that confinement could not. Combined: prewarm
(prevent the un-confinable one) + per-core sharding (confine the rest) is a coherent, evidence-backed
plan -- not a blocked gate.

## 3. Limitations to a true SMP kernel environment, ranked
**FUNDAMENTAL (seL4-inherent, not AIOS-fixable):**
1. **The BKL serializes ALL kernel entry.** One core in the kernel at a time; every syscall/fault/IRQ
   contends. MEASURED: full distribution = ~2.6x SLOWER IPC (contends, does not parallelize). =>
   **SMP on seL4 buys RESILIENCE (blast-radius), NOT throughput.** Removing it = fine-grained locking =
   full re-verification of seL4 (upstream, multi-year). Work WITH the BKL, not against it.
2. **Cross-core blocking deps on single-core services** (timer/pipe/fs/net on core 0). A thread on core
   N that blocks on core 0's service freezes if core 0 wedges (s12's load-bearing finding). Fix = Phase B
   per-core services. Mitigated now by prewarm + per-core session binding.
3. **The ~32.4s idle->wake silicon wedge** -- MAJOR OPEN CONCERN. Prewarm appears to prevent it
   (directional, not statistically decisive; OFF rate non-stationary ~10-43%). Not a cure.

**HIGH (AIOS-fixable; block full isolation):**
4. **LATENT BUG: secondaries lose preemption under the timer mask.** `AIOS_SIBLING_TIMER_MASK` (boot.c,
   default-ON) masks the timer PPI on cores 1-3 so they never tick -- but `/proc/coresched.1` /
   `/proc/distribute.1|.2` distribute runnable USER work onto those non-preemptible cores -> a CPU-bound
   proc there is never preempted -> starvation/scheduler pathology. The mask is a COMPILE-TIME #define
   with no runtime guard. **FIX (cheap, real stabilization): reject/warn if distribution is enabled while
   the timer mask is on, OR make the mask a userspace-toggleable kernel global.** Do this before anyone
   ships distribution.
5. **Single-point services** (timer/pipe/fs/net) -- Phase B (per-core timer via CNTV PPI + per-core
   process-manager + allocator sub-pools). The real SMP-isolation work. Not implemented.
6. (was "gate unrun") -- RESOLVED, see section 2.

**MEDIUM (operational):** shared root vspace RMW (hidden today by per-child vspaces); fs/net singleton
freeze risk (Phase C proxies); console input single-core-pinned (Phase B per-session relay).

## 4. Stabilization vs enhancement
- **STABILIZATION is largely DONE and the default is STABLE:** allocator lock, multi-core teardown,
  prewarm, timer-mask survive model -- all default-on, single-core-pinned default is correct + safe.
  **The one real outstanding stabilization item is #4** (the preemption/timer-mask guard) -- latent until
  distribution is enabled, but a genuine correctness gap. Everything else multi-core is opt-in + tested.
- **ENHANCEMENT (true SMP isolation) = Phase B** (per-core universal services). 4-6 weeks: kernel change
  to expose the per-core CNTV PPI + per-core timer/process-manager/allocator sharding + HW soak. This is
  what removes the cross-core blocking deps and makes a one-core wedge a one-core event.
- **The ceiling:** even fully done, the BKL caps kernel-bound/IPC-bound throughput. Phase B delivers
  RESILIENCE, not compute scaling. If the goal is real multicore THROUGHPUT, seL4's BKL is the wrong
  base -- which is another input to the seL4-vs-Linux question (Linux has fine-grained SMP).

## 5. Recommendation
1. **Now (cheap, real):** fix #4 -- add the runtime guard coupling distribution to the timer-mask
   (reject/warn, or make the mask runtime-toggleable). This is the only outstanding *stabilization* gap.
2. **Default stays single-core-pinned + prewarm-ON.** It is stable; ship it.
3. **Phase B is the true-SMP unlock** (per-core services), and the confine result already says it is
   viable for the wedge types it targets (prewarm covers the rest). But scope it as RESILIENCE work, not
   throughput -- and weigh it against the seL4-vs-Linux pivot (BL-1/BL-2): if AIOS may move to a
   fine-grained-SMP Linux base, a multi-week Phase B on the BKL model may not be worth it.
**Not worth doing on the BKL:** per-syscall load-balancing; fine-grained kernel locks (re-verification);
more external fabric keep-warm (sessions 7-11 refuted all); MCS/timeout-IPC (needs upstream seL4).
