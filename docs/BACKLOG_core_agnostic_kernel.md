# BACKLOG: core-agnostic / symmetric-core kernel (wedge blast-radius strategy)

Status: **BACKLOGGED** (2026-06-22, session 11, Bryan's call). Pursue AFTER the
blocking-sleep cure is HW-tested ("prevent the wedge first"). This doc preserves the
session-11 research so the design can be picked up cold.

## Motivation

The ~32.4s RPi4 stall is a **per-core, idle->wake, silicon-fundamental wedge** (every
software cure refuted across 11 sessions; see [[project_stall_not_dvm_idle]] /
[[project_stall_hunt]]). So the leverage is NOT curing the wedge -- it's making a per-core
wedge **not freeze the cluster**. Today the damage isn't the wedge (one core stuck ~32s);
it's that **everything useful (all root servers, the net stack, the shell) lives on core 0,
the one core that idle->wakes and wedges.** With `AIOS_SIBLING_TIMER_MASK` the siblings are
already *alive* during a wedge -- they're just idle and useless. Core-agnosticism closes
that gap: spread the work so a wedged core is a temporarily-absent core, not a cluster kill.

## THE ENABLING FINDING (session 11 research, proven from the kernel source)

**The seL4 big kernel lock (BKL) is RELEASED before `eret`-to-user.** A core that wedges on
the first user-instruction fetch (the s10/s11 localization: refilling a blocked thread's cold
resume line) is therefore **holding no lock** -- the kernel is free and other cores can enter
it and schedule normally.

- Slowpath: `restore_user_context()` does `NODE_UNLOCK_IF_HELD` at
  `deps/kernel/src/arch/arm/64/c_traps.c:57`, THEN the asm block with `eret` at :90.
- Fastpath: `fastpath_restore()` does `NODE_UNLOCK` at
  `deps/kernel/include/arch/arm/arch/64/mode/fastpath/fastpath.h:181`, THEN `eret` at :218.
- AIOS's `aios_checkpoint(11/13)` (the breadcrumbs that localized the wedge to the resume
  fetch) sit BEFORE the unlock -- consistent with the wedge being post-unlock / user-side.
- CLH lock: `deps/kernel/include/smp/lock.h` (`big_kernel_lock`, `clh_lock_acquire/release`,
  `NODE_LOCK`/`NODE_UNLOCK`/`NODE_UNLOCK_IF_HELD`).

This contradicts the older session notes ("siblings block on the BKL") -- that was the
PRE-`AIOS_SIBLING_TIMER_MASK` behavior. With the timer mask on (default), siblings stay in
`idle.S` and don't contend. **So the cluster-freeze is an AIOS-architecture problem (all work
on core 0), not a kernel-lock problem.** That makes core-agnostic resilience *architecturally
possible*.

## THE ONE BARRIER (session 11 research)

The ONLY thing pinning the root servers to core 0 is the **single-owner, lock-free
allocman/vka**: concurrent allocations from other cores tear the CSpace-slot bitmap RMW
(`src/boot/boot_services.c:21-35` -- the v0.4.178 comment; it caused the QEMU "second SSH
connection fails" bug). Everything else is ALREADY core-agnostic:

- IPC endpoints work from any core (clients already do this).
- User processes already distribute across cores 1..N-1 (`/proc/coresched`,
  `aios_assign_core` in `src/boot/spawn_util.c:41-72`; default-off because IPC-bound work
  CONTENDS on the BKL rather than parallelizing -- pipeline 30->6 measured).
- The kernel's multi-core teardown is already correct: per-ASID **residency-masked TLB
  shootdown** + **ASID generation** (`AIOS_ASID_GEN`, `armKSASIDResidency[]` in
  `deps/kernel/src/arch/arm/64/kernel/vspace.c`; the fastpath residency hook in
  `fastpath.h`). See [[project_asid_generation]].

**=> The enabling change is small in concept: a lock around allocman/vka -> un-pin the
servers -> let them run anywhere.** (`start_server_thread` + the per-server
`seL4_TCB_SetAffinity(..., ROOT_CORE)` calls drop the pin.)

## THREE RESILIENCE TARGETS (pick before implementing)

1. **Isolation (distribute servers).** Lock the allocator, un-pin servers, spread them.
   A per-core wedge stalls only the service(s) on that core for ~32s; the rest keeps
   responding. Smallest change that delivers real resilience. **Does NOT make a wedge
   invisible** -- the service on the wedged core is still down ~32s -- and it MOVES the wedge
   (the idle->wake wedge hits whichever core idles), it doesn't avoid it.
2. **Full invisibility (replication/failover).** Replicate critical servers across cores +
   idempotent request routing + health detection, so a sibling instance answers within ~ms
   while one core is wedged and the wedged core rejoins after ~32s. Much bigger: state
   replication, routing, failure detection.
3. **Prevent the wedge first (the blocking-sleep cure).** If a fully-idle core 0 (all
   spinners blocked -> warm resume line -> cache-hit resume -> never reaches the parked
   fabric) stops wedging, core-agnosticism becomes a THROUGHPUT choice, not a survival one.
   **This is the current focus** (session 11 blocking-sleep work; Part A timer HW-validated).

## HONEST CONSTRAINTS

- The BKL still serializes kernel ENTRY -> IPC-bound distribution contends (throughput), but
  for *resilience* that's an acceptable trade.
- Distributing moves the wedge across cores; only the cure (3) or replication (2) makes a
  wedge invisible.
- A full fine-grained-locking BKL removal remains the infeasible "2+yr proof-rewrite /
  silent-corruption-risk" (MVD-2, reviewed NOT worthwhile). Core-agnosticism here does NOT
  require that -- it works *because* the BKL is released before the wedge.

## INCREMENTAL FIRST STEP (when un-backlogged)

1. Add a lock (ticket/spin) around allocman/vka access; OR give each core its own allocman
   sub-pool. Host-test the slot-bitmap-tear scenario first (the SSH-2nd-conn repro).
2. Drop the `seL4_TCB_SetAffinity(..., ROOT_CORE)` pins on the servers; distribute them
   (e.g. round-robin, or by service).
3. Measure: force a core-0 wedge (netstall) and confirm a server on core 2 keeps answering
   (isolation) -- the `[STAGECP]`/watchdog already report per-core liveness.

## RELATED
[[project_stall_hunt]] · [[project_stall_not_dvm_idle]] · [[project_asid_generation]] ·
[[feedback_stall_open_concern]] · docs/NEXT_20260622_stall_session11_blocking_sleep_seed.md
