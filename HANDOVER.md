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

* **CURRENT STATE 2026-06-21 (session 8) -- IDLE->WAKE WEDGE LOCALIZED AT THE INSTRUCTION LEVEL ON HW
  (DECISIVE). The freeze is core 0 CLOCKED-BUT-WEDGED on a SINGLE fabric-dependent instruction,
  POSITION-INDEPENDENT (4 code sites); leading hypothesis = a COLD CACHEABLE DRAM LOAD, not a dsb. Stall
  stays a MAJOR OPEN CONCERN ([[feedback_stall_open_concern]]) -- a MEASUREMENT step toward the
  architectural fix, NOT a cure.** Board = v0.4.288 build 2805 at 192.168.0.8 (localization v0.4.286 +
  TWO cure attempts -- external DRAM-DMA keep-warm v0.4.287 + coherent cache-miss keep-warm v0.4.288 --
  BOTH HW-tested + REFUTED; both keep-warms default-OFF). Commits `06f7b37` + `c17f25e` + `5b4f05c` (+ the
  v0.4.288 mode-3 commit) on `main` (Bryan pushes). **ALL traffic-based prevention exhausted -> the cure
  pivots to KERNEL-REDESIGN CIRCUMVENTION (stop a 1-core wedge freezing the cluster).** Board still stalls;
  watchdog+hwdog default-on survive the ongoing freezes; netconsole wedges under churn (power-cycle to
  recover).
  Full detail + interpretation matrix + HW result: `docs/NEXT_20260621_stall_session8_localize.md`.
  - **HW RESULT (DECISIVE, 10+ stalls): core 0 ALWAYS `iovf=0`** (wedged on ONE instruction -- retired
    <2^32 over the whole 32.4s) with a tiny `bus` delta (`bovf=0` = clean wait on a PARKED fabric). Caught
    at 4 deterministic sites (exact inst-count each): A=`9->11` 1786 (slowpath syscall handler), B=`11->9`
    174 (exit->entry: restore_user_context ldp / eret / vector), C=`10->11` 366 (IRQ handler), D=`10->7`
    525 (IRQ + ctx-switch, wedge BEFORE setVMRoot's dsb -- `[DSBSTALL]` silent). Idle siblings always
    `iovf=1` (overflowed = spinning BILLIONS on the BKL) = DERIVATIVE. netstall 6/10 STALLED; pingmon =
    10 real whole-system ICMP GAPs (32.5/65/130s). **RESOLVES lead #2: wedged CPU on one instruction, NOT
    IRQ-not-delivered (the timer DOES fire -- siblings take it + block on the BKL). COMPLETES lead #1: the
    wedge is "the first fabric-dependent op after the SCB parks," position-independent -- NOT a removable
    fixed-PC instruction (why every prior single-instruction lever failed).**
  - **LEADING MECHANISM (Phase-2 design workflow `wf_5e364d17-60d`, high confidence): a COLD CACHEABLE DRAM
    LOAD, not a dsb.** After ~30s idle the TCB-context (restore_user_context `ldp`), scheduler/`ksCurThread`,
    and kernel-stack lines go cold; the first DRAM fill THROUGH the parked SCB hangs to the fixed ~32.4s
    timeout. Unifies the 4 sites; fits `inst=0` + small-nonzero `bus` (one line-fill) + fixed-timeout. Note:
    `aios_checkpoint(11)` is recorded just BEFORE the restore asm, so a wedge on the TCB-restore `ldp`
    presents as SITE B; SITE A is the same mechanism one stage earlier (cold `schedule()`/`activateThread()`
    read).
  - **CURE ATTEMPT -- AUTONOMOUS DRAM-DMA KEEP-WARM (v0.4.287 build 2799) = REFUTED ON HW.** The cold-load
    hypothesis implied: keep the SCB->memory path warm with REAL DRAM traffic (the one keep-warm class never
    tried). Built `src/servers/dma_warm.c` -- a BCM2711 legacy-DMA channel with a SELF-LOOPING control block
    copying a sub-1GB scratch buffer forever, autonomously in HW (no CPU kicks -> runs even while core 0 is
    wedged); `/proc/dmawarm.1` arm / `.0` disarm, default-OFF; ARM-free channel from the VC
    `GET_DMA_CHANNELS` mask (chan 6); 3-lens reviewed (both "blocker/major" findings were FALSE POSITIVES --
    WAITS/PERMAP are adjacent [20:16]/[25:21] not overlapping, and the mapping is non-cacheable [last arg=0]).
    **HW A/B: DMA armed + CONFIRMED RUNNING throughout (`active=1 err=0 dst0=0xa5a5a5a5`, self-loop loaded),
    `netstall --idle 30 --trials 10` = 6/10 STALLED = IDENTICAL to the OFF baseline (6/10); pingmon 3 real
    GAPs; 2 `[STAGECP]` (same `prev=11 this=9 iovf=0`). Continuous EXTERNAL DRAM traffic = ZERO effect.**
    PROVES the quiescence is NOT the shared SCB->memory-controller path -- it is the A72 CLUSTER's OWN
    ACE/snoop master interface (ACINACTM, TRM 2.4: idles when cluster snoop traffic stops; an A72 INPUT
    software cannot deassert; external masters cannot keep it warm). The "cold-DRAM-load" is the A72's first
    EXTERNAL transaction after ITS port parked. ELIMINATES the external-master keep-warm class. `dma_warm.c`
    KEPT default-OFF as a documented A/B knob (like refuted fabwarm/corewarm).
  - **CURE ATTEMPT 2 -- COHERENT cache-miss keep-warm (v0.4.288 build 2805) = ALSO REFUTED ON HW.** Made the
    A72 cluster ITSELF issue continuous COHERENT traffic: `fabwarm` MODE 3 (`/proc/fabwarm.3`,
    src/servers/fabric_warm.c) -- a core-1 thread cyclically reading a PRIVATE 2MB buffer (> 1MB L2) so every
    line MISSES -> coherent DRAM reads on the cluster ACE master (distinct from refuted Device-read modes 1/2
    + cache-hitting corewarm). Ran with watchdog parked (`/proc/watchdog.0` frees core 1; both are prio-1 on
    the timer-masked core 1 -> can't be time-sliced). **Verified running throughout (741,918 passes = ~1.5 TB
    of coherent reads, 5.3 GB/s), netstall = 5/10 = SAME as baseline; 7 [STAGECP], 6 GAPs. Core 1's 5.3 GB/s
    coherent reads did NOT keep core 0's path warm.** => the quiescence is CORE-0-SPECIFIC or DEEPER than any
    A72 traffic. **The ENTIRE traffic-based prevention space is now exhausted (DMA / coherent / Device /
    cacheable -- ALL refuted). Software prevention is dead -> the cure pivots to KERNEL-REDESIGN
    CIRCUMVENTION** (design workflow `wf_8791f3fb-dc7`): (1) move the wedge OUTSIDE the BKL (a fabric "wake"
    op at kernel entry before NODE_LOCK so a wedge doesn't hold the lock) + coresched; (2) try-lock+defer in
    the IRQ path; (3) a CORE-0 sleeping-timer heartbeat (the one prevention lead left, IF core-0-specific);
    (4) fine-grained locking [infeasible]. `fabwarm` mode 3 KEPT default-OFF.
  - **The PMU OVERFLOW FLAG was the key hardening** (3-lens adversarial review caught that a 32-bit delta
    masked over 32.4s wraps unpredictably -> false "wedged"): `iovf` cleanly separates the ONE wedged core
    (`iovf=0`) from BKL-spinning siblings (`iovf=1`). Without it the result would have been ambiguous.
  - **Re-read of the session-7 serial (`/tmp/sercap_idle.log`) corrected the one-line summary:** the
    wedged core (core 0) is `prev=11 this=11` (kernel-EXIT -> ...32.4s... -> kernel-EXIT, NO IRQ entry --
    it re-enters via syscall/fastpath/fault, not an interrupt). Only the IDLE cores are `11->10` (they take
    the timer IRQ and block on the BKL core 0 holds, `hb_lag~=dur` -- derivative). Core 0's `hb_lag` was
    garbage because **core 0 never runs `idle.S`** (saturated by prio-200 yield-spinning servers) so its
    idle heartbeat is always 0. The `11->11` window was UNINSTRUMENTED (no entry checkpoint; fastpath
    round-trips skip stage 11).
  - **v0.4.286 instrumentation (all behind `AIOS_TEARDOWN_CHECKPOINTS`):** stage 9 = kernel ENTRY
    (`arch_c_entry_hook`), stage 12 = fastpath EXIT (`fastpath_restore`), + per-interval PMU deltas
    (INST_RETIRED/BUS_ACCESS/CPU_CYCLES) and a real core-0 heartbeat (`kent_lag` = ms since last kernel
    entry) in `aios_checkpoint` (errata.c). New line:
    `[STAGECP] core=C prev=P this=S dur=Dms idle_lag=Ims kent_lag=Kms pmu=[inst=N iovf=0/1 bus=B bovf=0/1 cyc=Y]`
    (the `iovf`/`bovf` overflow flags were the key hardening -- see the HW-result bullets above).
  - **This bisects core 0's wedge + resolves "wedged-CPU vs IRQ-not-delivered":** `prev=9 this=11/12` (+
    kent_lag~=dur) = wedge INSIDE the kernel handler; `prev=11/12 this=9` = wedge BEFORE entry (eret asm /
    user EL0 / exception vector). `inst~=0` over 32.4s = CLOCKED but WEDGED on ONE instruction; `inst`
    large = looping (a scheduling/IRQ-delivery freeze, not a CPU wedge). Idle cores spin on the BKL so they
    should read `inst` large while core 0 reads `inst~=0` -- the PMU separates the PRIMARY wedge from the
    derivative blocks. Full design + interpretation matrix + HW procedure: `docs/NEXT_20260621_stall_session8_localize.md`.
  - **QEMU gate green-equivalent to baseline (no regression):** smp 4/5 (W=12 correct + host-load prompt
    timeout), shmring 25/26 (1 timing shed), socket 8/8, netd 10/10. build-rpi4 (non-hyp) built clean with
    stages 1-7,9,10,11,12 all present. The instrumentation touches no external bus (sysreg reads + per-core
    cacheable stores) so it cannot warm the fabric / mask the stall. Adversarially reviewed (3-lens workflow).
  - **>>> NEXT (localization DONE; external-master keep-warm DONE+REFUTED; remaining cures are hard):**
    (1) the DMA-keep-warm refutation localized the quiesce to the A72 CLUSTER's own ACE port -- so the only
    keep-warm that could work is a CORE-0-side periodic EXTERNAL transaction during idle (e.g. a core-0
    blocking-timer heartbeat that does an uncached read every ~10ms; the variant never cleanly tried --
    fabwarm ran on core 1). CAVEAT: core-1 fabwarm (A72 Device reads) was ALSO refuted, so the quiesce may
    be deeper than the cluster ACE port (VideoCore/SCB-side) and even a core-0 keep-warm may fail -- test it
    but temper expectations. (2) The ARCHITECTURAL cure: drop the BKL coupling so a wedged core 0 does not
    freeze the whole cluster (the only path independent of defeating the fabric quiesce; 2+yr proof-rewrite
    or a silent-corruption-risk lock-drop -- see the MVD-2 verdict). (3) Phase-2 cold-load-vs-dsb confirm
    (register-safe asm checkpoint around the restore `ldp`; spec in workflow `wf_5e364d17-60d`) -- now mostly
    moot (the cure is refuted regardless of which instruction). To re-run any HW A/B: power-cycle, `sercap
    /tmp/x.log`, board on v0.4.287 (or `pi_flash.py --build`), `pingmon` + `netstall --idle 30 --trials 10`,
    grep [STAGECP]. STILL A MAJOR OPEN CONCERN. <<<**
  - Kernel diff in `deps/patches/seL4-kernel.patch` (1532 lines). KEPT: ASID-gen + coresched S1/S2 +
    watchdog default-on + [TLBISTALL]/[DSBSTALL]/[STAGECP] (now with PMU overflow flags + kent_lag)
    profilers + MVD-1.

* **CURRENT STATE 2026-06-21 (session 7) -- THE ASID-GENERATION CURE IS BUILT + HW-TESTED, AND THE STALL IS
  LOCALIZED: it is the IDLE->WAKE transition, NOT process teardown -- the multi-session "tlbi/teardown is
  the cause" premise is OVERTURNED. Board = v0.4.284 build 2768 at 192.168.0.8 (ASID-gen ON + coresched S1
  + session-7 diagnostics + MVD-1 watchdog DEFAULT-ON, HW-verified: enabled=1 hwdog=1, no false-trip).
  Commits on `main` (Bryan pushes): `d40d8e6` v0.4.277 (ASID-gen), `147ea34` v0.4.282 (stall localized),
  `3e6d33a` v0.4.283 (coresched S1), `811481d` (handover docs), `5d4c56d` v0.4.284 (watchdog default-on).
  The stall STAYS a MAJOR OPEN CONCERN -- never "solved"
  ([[feedback_stall_open_concern]]). Detail: [[project_asid_generation]] + `docs/NEXT_20260621_asid_generation_IMPL.md`.
  **>>> NEXT SESSION: `docs/NEXT_20260621_stall_session8_seed.md` has the full stall state + a
  paste-ready SEED PROMPT for continuing the answers/mitigation search. POWER-CYCLE the Pi first
  (netconsole-wedged from the coresched test). <<<**
  - **ASID-generation TLB recycling SHIPPED (v0.4.277, gated `AIOS_ASID_GEN`, non-hyp/Pi only).** Decouples
    the seL4 logical asid from the hw asid (TTBR0[63:48]); recycles by a 32-bit generation so teardown +
    self-munmap issue NO per-asid `tlbi`. Host-validated (`scripts/asidgen_host_test.c` 4/4), adversarially
    reviewed (5-agent, invariant SOUND, `docs/asidgen_review_session7.json`), QEMU-regression-clean. **KEY:
    the QEMU build is HYP, the Pi build is non-HYP -> ASID-gen runs ONLY on the Pi; the QEMU gate is
    regression-only; AIOS_HWASID() is identity on hyp.** TCR_EL1.AS=1 confirmed on silicon (`[ASIDGEN]` probe).
  - **HW A/B: ASID-gen is NEUTRAL on the stall.** It removed the teardown `tlbi` (proven: `[TLBISTALL]` fires
    with it OFF, vanishes ON) but the ~32.4s freeze persisted at the SAME rate (5-7/10 both ways). Then
    skipping the teardown cache-clean (`dc cvau`) ALSO did nothing. Three cure hypotheses refuted by
    elimination (tlbi, cache-clean, context-switch dsb).
  - **STALL LOCALIZED via a checkpoint breadcrumb (`[STAGECP]`, errata.c `aios_checkpoint`).** EVERY freeze =
    `prev=11 this=10 dur=~32400ms` = the 32.4s sits BETWEEN kernel-exit (`restore_user_context`) and the next
    IRQ entry (`c_handle_interrupt`) -- the idle->wake window -- with NO teardown stage around it.
    CLUSTER-WIDE (cores 2,3 idle heartbeat stale the full duration; they block on the BKL while core 0 wedges
    on the first fabric op after idle). idle.S has no wedging instruction => the wedge is in
    eret/IRQ-vector/c_entry_hook, BKL-held. **VERDICT: fabric-quiescence-FUNDAMENTAL at idle->wake (BCM2711
    SCB quiesces during idle; first wake fabric op hangs ~32.4s, halts the cluster). NOT a removable seL4
    instruction. ACCEPT + MITIGATE (MVD-1), now PROVEN by localization.** This explains why every prior lever
    failed (all attacked the teardown, which is not the wedge).
  - **Coresched safety S1 SHIPPED (v0.4.283): reserved-asid at generation wrap** (Linux PCID; host-proven) --
    the prerequisite for unpinning user threads from core 0. S2 (peer-visibility on clear) DOCUMENTED +
    DEFERRED at the `aios_hwasid_clear` site (needs a masked IPI, belongs with coresched enablement; moot
    under core-0 pinning).
  - **MVD-1 watchdog DEFAULT-ON SHIPPED + HW-VERIFIED (v0.4.284, `5d4c56d`):** the core-1 timer-masked
    watchdog + core-0 heartbeat + PM HW-watchdog auto-reset now arm at boot (unattended-production posture --
    justified now the stall is fabric-fundamental). HW: `/proc/watchdog enabled=1 hwdog=1 stalls=0`, no
    false-trip; QEMU gate unchanged (device pokes null-guarded -> no-op on QEMU). Disable: `/proc/watchdog.0`,
    `/proc/watchdog.hwdog.0`.
  - **vDSO fast-path (task #10) DEPRIORITIZED:** investigation found the AIOS time path is ALREADY near-optimal
    (`clock_gettime` reads `CNTPCT_EL0` directly in EL0; the wall offset is cached per-process with
    re-query-while-0; SNTP runs once at boot) -- so the vDSO win is marginal and its impl touches the
    high-blast-radius exec_server spawning path. Revisit only with a concrete shared-sysinfo-page consumer.
  - **NEXT (Bryan's agreed direction): coresched S2 + unpin user threads** when ready (BKL-ceiling-bound; gives
    compute parallelism; S2 = masked-IPI peer-visibility on `aios_hwasid_clear`, documented at the clear site).
    **Stall (optional, gentle -- board hammered s7):** instrument c_entry_hook/IRQ-vector for the exact wedged
    instruction; zero-flash pingmon-only-idle test to confirm idle-alone freezes without teardown.
  - **Coresched safety S2 SHIPPED (v0.4.285, `ab20ce5`):** `aios_asid_gen_invalidate` now does the
    residency-masked `tlbi` (the original, HW-proven residency-shootdown path) for a PEER-resident vspace
    instead of abandoning its hw asid -- so coresched (`/proc/coresched.1`, the existing `aios_assign_core`
    round-robin) is ASID-gen-correct. Dormant under core-0 pinning. Verified: host 4/4, build-rpi4 clean,
    QEMU smp 4/5 (W=6..18 distributed-correct). **HW RESULT -- CORESCHED (unpin) WEDGES THE Pi,
    REPRODUCIBLE: enabling `/proc/coresched.1` reliably drops the netconsole + wedges the board (twice;
    the 2nd on a clean freshly-booted v0.4.285 board, no deploy running -- so NOT the churn I first
    blamed). Board stays ALIVE on ICMP (net stack/core 0 up) but the shell is dead -> wants a
    power-cycle.** No serial trace of the moment (sercap was not running), so [ASIDGEN-BUG] vs a stall-
    storm is unconfirmed. **LIKELY CAUSE -- coresched is BLOCKED BY THE UNSOLVED FABRIC STALL:**
    distributing user work to cores 1-3 exposes those cores to the same idle->wake freeze (cluster-wide)
    AND widens the residency mask so teardowns IPI peers -> coresched amplifies the stall and wedges the
    box. So unpin and the stall are COUPLED: cannot safely unpin until the (fabric-fundamental) stall is
    addressed. S2 is correct groundwork (QEMU+host+review validated, reuses the proven residency-shootdown
    path) but DO NOT enable /proc/coresched.1 on the Pi until this is investigated (with serial capture
    running -- `/proc/coresched.1` then watch serial for [ASIDGEN-BUG]/fault/[TLBISTALL] storm). DEFAULT
    is SAFE (coresched off, resets on reboot). LESSON: run sercap BEFORE any coresched/stall HW test;
    wait for pi_flash banner-PASS before testing; netconsole wedges under churn.
  - Board left on v0.4.285 build 2774 (ASID-gen + S1 + S2 + `[STAGECP]`/`[DSBSTALL]` diagnostics + watchdog
    default-on; teardown clean ON; `AIOS_TEARDOWN_NO_CLEAN` knob default-off), ALIVE but netconsole-wedged
    -> power-cycle for a clean board. seL4 changes in `deps/patches/seL4-kernel.patch` (1429 lines);
    watchdog default-on in `src/servers/watchdog.c`.

* **CURRENT STATE 2026-06-21 (session 6) -- THE STALL CURE IS SCOPED: the seL4 ASID-GENERATION redesign
  (`docs/NEXT_20260621_asid_generation.md`) is the one architectural cure left, and the stall hit LIVE this
  session (3 freezes, repeatedly killing the keyboard) -- vindicating "never conclude it". Board = v0.4.276
  build 2738 at 192.168.0.8 (netd-OFF). This session's code is UNCOMMITTED WIP (see below); Bryan pushes.**
  - **STALL = the headline, and it bit LIVE.** Typing on the HDMI keyboard, an `ls`/`cat` teardown triggered
    `[TLBISTALL] asid=1 va=0x117ce000 dur=32395ms` -- a ROOT SELF-MUNMAP per-page `tlbi vae1` (NOT the deleteASID
    `aside1`), which errored the keyboard int-IN EP (`cc=36` -> keyboard dead -> replug to recover). A keyboard
    attached makes the stalls FREQUENT (every few commands; matches the old "stall per quantum with a keyboard"
    note). **NEW INSIGHT: the dominant stall is the self-munmap per-page `tlbi vae1` (current asid, eager flush),
    not just the teardown `aside1` -- so the cure must kill BOTH.** Reframed as a MAJOR OPEN CONCERN
    ([[feedback_stall_open_concern]]): MVD-1 only SURVIVES it; the system still freezes 32.4s.
  - **THE CURE, SCOPED: seL4 ASID-GENERATION recycling (`docs/NEXT_20260621_asid_generation.md`).** Every ISA
    lever is exhausted -- local vs broadcast `tlbi` (both froze 2026-06-19), `dsb sy` vs `dsb nsh` (both froze
    32399ms, build 2122), every keep-warm/clock/voltage. The A72 emits the DVM-Sync from ANY `tlbi;dsb`
    regardless. So the cure is to NOT ISSUE the teardown/self-munmap `tlbi` at all: decouple the seL4 asid from
    the HARDWARE asid, recycle hw asids via a generation counter (`deleteASID` = no flush; self-munmap = assign a
    FRESH hw asid + reload TTBR0, no `tlbi`; full flush ONLY at generation wrap -- rare + warm-fabric). Removes the
    after-idle `tlbi` that hangs. Correctness-critical (a bug = silent cross-process TLB corruption) + DIVERGES
    from seL4's Isabelle proof (ships TESTED-not-PROVEN -- Bryan's call). Injection points grounded in the doc:
    `setVMRoot`/`armv_contextSwitch` ([vspace.c:912]), `deleteASID` ([vspace.c:1237]), `invalidateTLBByASID`
    ([vspace.c:1089]). Multi-session. SEED PROMPT at the end of the doc + handed to Bryan this session.
  - **HW-VERIFIED this session (USB):** Stage 5 STALL recovery -- the NATURAL transparent recovery finally
    reproduced (re-plug -> `inject=0 recoveries=4`, drive healthy = a GENUINE bulk STALL recovered transparently
    on the VL805, closing last session's loose end). Stage 6 multi-sector read HW-VERIFIED (`msc-multi:
    selftest=PASS n=8` on the 4TB Buffalo). Both observable in `/proc/xhci`.
  - **Also: O_CREAT silent-fail FIXED** (v0.4.275 `bc528ef` -- `open(O_CREAT)` on a denied create returns -EACCES;
    idtest extended, identity 6/6). **sshd CONFIRMED WORKING on :2222** (the "down" was a wrong-port test -- AIOS
    sshd is 2222 not 22; multi-session was already fixed v0.4.178). **THREE "open tracks" were STALE-INDEX
    (already done): SHM-ring HW-coherency, identity-privesc, SSH-multi-session** -- MEMORY.md index lines corrected.
    Serial capture restored: `scripts/sercap.py` now opens **O_RDWR** (macOS FTDI rejects `tcsetattr` on a
    read-only fd -> EINVAL).
  - **UNCOMMITTED WIP (the board RUNS it; source not committed): v0.4.276 runtime keyboard-LED** -- `xhci.c`
    `set_leds_runtime` re-enabled (Stop-EP -> SET_REPORT -> resume the int ring from the `dev_ctx` HW dequeue ->
    doorbell; `[led-rt]` serial diagnostics; keypress-wiring still OFF, only the `/proc/xhci.led` poke fires it).
    FLASHED (build 2738) but UNTESTED -- the LED poke test got derailed by the stall storm (the stall kills the
    same int-IN EP the LED manipulates). NEXT for the LED: poke `/proc/xhci.led.2` + read the serial `[led-rt]`
    line + confirm the Caps LED lit + that typing survives. **RELATED HIGH-VALUE IDEA: an int-IN EP RECOVERY** at
    [xhci.c:888](src/usb/xhci.c) (the driver detects `int-IN err cc=36` but only re-arms; add Reset-EP + re-arm,
    the bulk `bot_ep_recover` pattern) so the keyboard SELF-HEALS after every stall -- no replug. Same int-IN EP
    mechanism as the LED.
  - **COMMIT the WIP** (LED `xhci.c` + `version.h` 276 + the `sercap.py` O_RDWR fix + the scope doc), Bryan pushes.
    Board: v0.4.276 build 2738, Buffalo + keyboard attached (stall-prone), FTDI serial on `/dev/cu.usbserial-0001`.

* **CURRENT STATE 2026-06-21 (session 5) -- USB MSC Stage 5 bulk-STALL recovery HW-VERIFIED; the last
  HW-pending item from the stall-hunt era is closed. The stall hunt is NOT concluded -- it stays a MAJOR OPEN CONCERN, BACKLOGGED (the freeze is mitigated via MVD-1, NOT cured; Bryan: never frame it as done -- [[feedback_stall_open_concern]]).** Board =
  **v0.4.273 build 2729** at 192.168.0.8 (netd-OFF build-rpi4). 1 code commit on `main` (`191552f`; Bryan
  pushes). Pivoted off the (backlogged-but-still-open) stall hunt to clear a HW-pending verification while the board was clean.
  - **What was pending:** Stage 5 USB bulk-STALL recovery (`bot_ep_recover`, v0.4.257) was QEMU-verified 9/9
    but had never run on real hardware. It could not be observed over netconsole: the runtime USB driver logs
    recovery to SERIAL only (confirmed -- `/proc/log` holds only boot lines; runtime re-enumerations leave no
    ring trace) AND `/proc/xhci.stalltest` is a write-only inject knob with no recovery counter. (SHM-ring, the
    OTHER "HW-pending" index line, was already HW-validated 2026-06-17 -- a STALE index entry, not real work.)
  - **The fix (`191552f`):** added a persistent `g_msc_stall_recoveries` counter (incremented only after a
    successful EP reset in `bot_ep_recover`) surfaced as `msc-stall: inject=N recoveries=M` in `/proc/xhci` --
    netconsole-observable; recovery logic byte-identical. Extended `usb_msc_stall_recovery_qemu_test.py` to
    assert it (now 10/10).
  - **HW result (build 2729, 4TB Buffalo on the VL805 hub, SuperSpeed port 3):** armed `inject=3`, replug ->
    **`recoveries=3 inject=0`** = `bot_ep_recover` EXECUTED 3x on the real VL805 (the counter only climbs after
    the controller accepts Reset-Endpoint). The HW-pending core -- does the recovery path run on silicon? -- is
    ANSWERED: yes. The injected STALL storm transiently aborted enumeration (`msc ok=0`) but the drive
    re-enumerated clean to healthy 4TB (`ok=1`) via the Path-B hub reconcile = a robustness win, no wedge.
  - **Finding -- the fault injection is QEMU-faithful but NOT HW-faithful:** the fake STALL leaves the EP
    RUNNING, so `bot_ep_recover` Reset-EPs a NON-Halted EP (the VL805 treats that as disruptive; QEMU tolerates
    it). So injected tests CANNOT demonstrate TRANSPARENT in-session recovery on HW -- that stays QEMU-proven
    (10/10) and on HW needs a GENUINE natural STALL (a SuperSpeed first-replug; none reproduced this session --
    boot-enum `recoveries=0`, the `inject=3` replug showed exactly 3). Recorded in an `xhci.c` HW-NOTE comment.
  - **Net:** Stage 5 recovery path HW-proven to execute + the system robust to a STALL storm; transparent
    recovery QEMU-proven. The pragmatic verification ceiling for an opportunistic error path is reached. Board
    left clean (`inject=0`, `msc ok=1`, drive healthy). QEMU gate green pre-flash (`usb_msc_stall_recovery`
    10/10; smp 4/5 + shmring 25/26 host-load sheds; socket 8/8; netd 10/10). NEXT (open tracks): USB Stage 6
    multi-sector read; V3D Phase 4b; security privesc fixes; net SSH-one-session-per-boot. [[project_usb_msc]].

* **CURRENT STATE 2026-06-20 (session 4) -- THE STALL HUNT IS BACKLOGGED -- A MAJOR OPEN CONCERN (mitigated via MVD-1, NOT cured; do NOT call it concluded). The ~32.4s
  idle-teardown freeze is INCURABLE (cure space closed) but now fully SURVIVABLE + auto-recovering, and the
  ambitious "keep serving through a freeze" (MVD-2) was reviewed and judged NOT WORTHWHILE.** Board =
  **v0.4.272 build 2726** at 192.168.0.8. 9 commits on `main` (Bryan pushes). seL4 changes (residency mask +
  lazy-TLB + diagnostics + core-1 timer-mask) in `deps/patches/seL4-kernel.patch` (940 lines). The arc, top to
  bottom:
  - **Residency/IPI bug FIXED + HW-verified** (`42e5497` v0.4.269): every freeze was `asid=1` (root vspace)
    with `residency[1]={0,1,2,3}` from the keep-warm threads' boot-time marks -> a shootdown-mask makes
    teardowns core-LOCAL (`ipi` climbing 73264->101120 -> flat 0). Detail (A)/(B) below.
  - **NEW finding: the remaining sibling-freeze is the BKL + timer tick, NOT the residency** -- removing the
    IPI is necessary but not sufficient (the kernel idle thread takes the timer IRQ during the stall and
    blocks on the BKL core 0 holds ~32s). Detail (C) below. This set up MVD-1.
  - **Cure space DEFINITIVELY CLOSED** (`909d01c` v0.4.270): over_voltage=6 + the GENET MDIO poll (Linux's
    EXACT PHY_POLL lever) both REFUTED -- every keep-warm / clock / register / voltage lever is exhausted.
    Detail (D) below.
  - **MVD-1 watchdog COMPLETE + HW-PROVEN** (`95c822d` v0.4.271): a timer-masked, pure-userspace core-1
    watchdog stays alive through each 32s freeze and reports it OUT-OF-BAND on the mini-UART -- every `[WDOG]`
    detection correlates perfectly with the kernel's `[TLBISTALL]`. Detail (E) below.
  - **Watchdog ACTIONS + MVD-2 verdict** (`630d337` v0.4.272): the watchdog now ACTS (ACT-LED + PM HW-watchdog
    auto-recovery: a TOTAL wedge auto-reboots in ~63s), plus /proc/laststall + /proc/axiquiet; MVD-2 reviewed =
    NOT WORTHWHILE (the BKL is the wall -- removing it is a 2+yr proof rewrite, the shortcut risks silent
    corruption on a verified kernel, for a niche benefit). Detail (F) below.
  **Net: the freeze is accepted; the box no longer goes silently dark (observe + report + auto-reboot a true
  hang); the cure AND survivability spaces are fully mapped.** /proc/watchdog + /proc/watchdog.hwdog are
  DEFAULT-OFF (enable for production). Diagnostics (PMU/heartbeat/profiler/res=/rmask=) KEPT. Board left clean:
  watchdog disabled, residency fix active, ~66C.

  **(A) ROOT CAUSE PINNED (session-3 PMU was right; session-4 found the exact source).** A baseline capture
  (build 2699) caught 8 stalls, EVERY ONE `asid=1` (the immortal root-task vspace) with `ipi` climbing
  73264->101120 and `hb_ms[2,3]==dur`. The `residency[1]={0,1,2,3}` bits come from the KEEP-WARM DIAGNOSTIC
  threads: `fabric_warm.c` (core 1, DEFAULT-ON) + `core_warm.c` (cores 1/2/3, parked) are spawned in the
  ROOT vspace and run their entry code once on cores 1-3 before parking -> they mark `residency[1]` for those
  cores, and the bits NEVER clear (asid 1 is never `deleteASID`'d). So every root self-munmap + the dying-proc
  `aside1` IPI-stormed the idle siblings, and core 0's stalled `dsb` dragged them into THEIR own `tlbi;dsb`.

  **(B) THE FIX (HW-verified, 3/3 post-fix stalls).** Two parts: (1) **residency shootdown-mask** (seL4
  `vspace.c`): `aios_asid_residency()` now returns `residency[asid] & aios_resident_cores_mask`, default
  `BIT(0)`; the mask latches WIDE (sticky) only when a NON-root vspace (`asid != IT_ASID`) actually RUNS on a
  secondary core = genuine coresched `/proc/coresched.1` (user procs have asid>=2). The keep-warms run the
  ROOT asid on secondaries so they do NOT widen it -- which is what keeps the immortal-root teardown
  core-local. (A first cut latched on a migrateTCB hook; the `rmask=` diagnostic IMMEDIATELY caught it
  mis-firing -- the keep-warms `SetAffinity` to cores 1/2/3 at boot, so the migrate latch widened the mask
  during boot. Moved the latch into `aios_mark_asid_residency`, keyed on the non-root asid. The diagnostic
  earned its keep.) (2) **fabwarm DEFAULT-OFF** (refuted GPLEV0 keep-warm; reverts the v0.4.267 default-on) so
  core 1 idles and the fix is provably safe + the heartbeat readable. Result: `res=0xf rmask=0x1 ipi=0` across
  3 consecutive stalls (baseline was `ipi` climbing) -- the IPI storm is ELIMINATED and asid-1 teardowns are
  core-LOCAL. QEMU gate green (smp 4/5 + shmring 25/26 host-load sheds; socket 8/8, netd 10/10).

  **(C) NEW FINDING -- the residency fix is NECESSARY but NOT SUFFICIENT for "siblings survive"; the
  REMAINING coupling is the BKL + the per-core timer tick, NOT the residency.** Even with `ipi=0`,
  `hb_ms[1,2,3]` STILL read `==dur` (cores 1,2,3 froze the whole stall). Mechanism: the kernel idle thread
  (idle.S, where the heartbeat stamps) runs with IRQs ON; during core 0's 32s stall core 0 holds the BIG
  KERNEL LOCK, so each sibling's periodic TIMER IRQ enters the kernel and blocks in `clh_lock_acquire` ~32s.
  The idle heartbeat therefore CANNOT demonstrate survival (it is kernel-side, BKL-coupled). The handover's
  session-3 expectation "hb_ms[2,3]~=0 after the residency fix" was OPTIMISTIC -- it overlooked the
  timer-tick-BKL coupling. **=> MVD-1 must do THREE things to actually survive a core-0 stall: (1) the
  residency fix [done -- no TLB-IPI to the watchdog core], (2) MASK the timer IRQ on the watchdog core (so it
  never enters the kernel for a tick -> never blocks on the BKL), (3) be PURE USERSPACE (no syscalls in the
  hot loop, like fabric_warm.c).** Only then does a watchdog core keep running while core 0 is wedged.

  **(D) TWO MORE CURE LEVERS REFUTED -> the cure space is now DEFINITIVELY CLOSED.** (1) **over_voltage A/B**
  (build 2707, flash-free `set_overvolt.py 6`): stall PERSISTS (dur=32399ms); active-signature confirmed by a
  +5C idle-temp delta (79.3C->84.2C->79.3C on revert -- firmware genuinely applied it). AVS/DVFS idle-voltage
  droop is NOT the cause. (2) **GENET MDIO-poll keep-warm** (build 2711, v0.4.270 -- Linux's EXACT PHY_POLL
  lever): added `/proc/fabwarm.2` = a core-1 ~1kHz MDIO read of the PHY BMSR via the root's `dev_genet_vaddr`
  (netd does not auto-poll MDIO -> no contention). ARMED + ACTIVE (iters 5524->209483 at ~1.1kHz; the stall
  capture's own hb_ms[1]=233186 = core 1 busy with the keep-warm AT the stall = active-signature embedded in
  the negative result). Stall STILL persists (4+ stalls dur~32.4s, ipi=0, rmask=0x1). REFUTED. Non-coherent
  GENET-block MDIO traffic does NOT warm the SCB DVM-completion/snoop path. Kept default-OFF (fabric_warm.c
  mode 2) as an A/B knob. **=> the cure space is EMPTY (every keep-warm incl. Linux's actual mechanism, every
  clock/register/voltage lever refuted). The ONLY remaining path for the freeze is MVD-1 (SURVIVE, not cure).**

  **(E) MVD-1 WATCHDOG: COMPLETE + HW-PROVEN (95c822d, v0.4.271 build 2723) -- a timer-masked core-1 watchdog
  detects + reports a freeze IN REAL TIME, with PERFECT correlation to the kernel's own stall record.** All
  three parts work: (1) **kernel timer-mask** (seL4 boot.c: skip `setIRQState(IRQTimer)` for core 1 -> CNTV PPI
  masked -> core 1 takes no tick -> never blocks on the BKL; IPIs stay enabled) -- VALIDATED (SMP gate W=20/20).
  (2) **pure-userspace core-1 watchdog** (src/servers/watchdog.c, busy-loop + direct mini-UART poke via
  dev_uart_vaddr). (3) **core-0 heartbeat** at the SERVER prio (200) + seL4_Yield (a prio-1 thread STARVED --
  core 0 is saturated by prio-200 yield-spinning servers: serverstats/pipe/xhci/root/flush ~= 98%; this also
  explains the kernel profiler's "core 0 never idles"). HW result (serial): `22:31:23 [WDOG] core0 STALLED
  (core1 alive, no tick/BKL) stalls=1` / `22:31:46 [TLBISTALL] core=0 dur=32395ms` / `22:31:46 [WDOG] core0
  recovered after 32461ms` -- EVERY [WDOG] detection lines up with a real [TLBISTALL], ~32.4s durations match,
  hb_iters climbs, age=0. So the core-1 watchdog stays alive through each 32s freeze + reports OUT-OF-BAND on
  the mini-UART while core 0/kernel/netconsole are all dead = the "1-2 cores stuck beats whole-box wedged" win,
  FULLY REALIZED. Default-OFF (/proc/watchdog .1 enable / .0 disable / status). **CAUTION: heavy stall-testing
  WEDGED the board once this session (needed a physical power-cycle) -- be GENTLE (short netstall runs).**

  **(F) MVD-1 POLISH SHIPPED (v0.4.272, 630d337) + MVD-2 REVIEWED = NOT WORTHWHILE.** Four follow-ups, all
  HW-tested: (#1) the watchdog now ACTS -- ACT-LED (GPIO 42) during a stall + PM HW-watchdog auto-recovery
  (/proc/watchdog.hwdog.1, default-OFF: core-1 pets the BCM2711 PM watchdog every loop; a normal 32s freeze
  keeps petting [no reset], a TOTAL wedge stops petting -> SoC auto-resets in ~63s -- HW-PROVEN the board stays
  up via petting); (#2) /proc/laststall (serial-independent); (#3) WD_STALL_MS tunable #define (9s,
  false-trip-free -- prio-200 heartbeat); (#4) /proc/axiquiet -- **ARM_LOCAL 0xFF800000 is NOT a userspace
  device untyped** (`[devmap] armlocal -> 0`), so AXI_QUIET_TIME is unmappable without a kernel change (not
  worth it). **MVD-2 VERDICT (3-agent feasibility review): the freeze is coherency-path-specific NOT a total
  fabric freeze (the watchdog wrote to the mini-UART DURING it; Pi4 GENET DMA is non-coherent), so the I/O
  HARDWARE could move packets through a freeze, AND the I/O-server split is tractable (~800-1200 LOC + a small
  seL4 GIC setIRQTarget patch). BUT the BIG KERNEL LOCK is the wall: a serving thread must syscall, and core 0
  holds the BKL ~32s. Removing it = full fine-grained locking = 2+ YEARS + an Isabelle proof rewrite
  (infeasible); the only shortcut (drop the BKL around the teardown dsb with per-ASID locking) is ~2-3 weeks
  but with UNPROVEN TLB-coherency safety = silent-corruption risk on a VERIFIED microkernel. Breaking seL4's
  verified BKL serialization to dodge a HW freeze is self-defeating (it trades away the verification that is
  the reason to use seL4), for a niche benefit (keep a shell alive through a ~2.5%/teardown 32s freeze).
  => MVD-2 is NOT WORTHWHILE. The pragmatic ceiling = MVD-1 + the HW-watchdog auto-recovery (DONE): observe,
  report out-of-band, auto-reboot a true hang in ~63s. The box no longer goes silently dark.**

  **NEXT-SESSION PRIORITIES:** the stall work is BACKLOGGED as a MAJOR OPEN CONCERN (Bryan 2026-06-21: never conclude it; the real fix is the seL4 ASID-gen/lazy-TLB redesign) (freeze accepted; observe + report
  + auto-recover all shipped; cure space + MVD-2 both closed). Optional: consider making the watchdog +
  hwdog DEFAULT-ON for production unattended operation. seL4 tree changes (residency mask + lazy-TLB +
  diagnostics + timer-mask) captured in deps/patches/seL4-kernel.patch (940 lines). [[project_stall_hunt]].

* **CURRENT STATE 2026-06-20 (session 3) -- RESEARCH + DIAGNOSTICS. Two deep research workflows re-confirmed
  the cure space is closed BUT surfaced one promising untried cure (GENET MDIO poll) + proved multi-core
  "survive the stall" is feasible as a watchdog; the PMU+heartbeat diagnostics are IMPLEMENTED + FLASHED
  (build 2699) but the capture is BLOCKED on a wedged USB-serial adapter.** Board = **v0.4.268 build 2699**
  = the committed clean baseline (lazy-TLB + profiler) PLUS UNCOMMITTED PMU/heartbeat diagnostics (seL4 tree:
  machine.h/errata.c/idle.S). RAM 3877 (full DRAM, no trim). Committed baseline is still `8797f64` (v0.4.268);
  the diagnostics sit uncommitted on top.

  **(A) The Linux-vs-AIOS gap (primary-source) -> the most promising UNTRIED cure.** Linux on RPi4 never
  freezes -- not via any register/barrier/TLB-structure delta (its boot/coherency setup is byte-identical; it
  DOES WFI + DOES go bus-quiet), but because it emits a **~1 Hz floor of REAL peripheral MMIO/DMA** that keeps
  the BCM2711 SCB DVM-completion logic clocked: chiefly the **GENET PHY link-poll over MDIO every 1 second**
  (BCM2711 GENET has NO link-change IRQ, so Linux phylib runs PHY_POLL = HZ -- an MDIO read traversing the
  GENET block on the SCB), plus opportunistic GENET RX DMA + USB. The TIMER TICK does NOT warm the fabric
  (CNTPCT is a system-register read, zero bus traffic -- exactly why every AIOS CPU keep-warm failed). **=>
  UNTRIED CURE LEAD: a periodic (~1 kHz, above the quiesce onset) GENET-block MDIO register read from a core-1
  thread (Linux's exact mechanism), DISTINCT from the refuted fabwarm GPLEV0/GPIO read (different SCB
  sub-block).** Could still fail (GENET DMA is non-coherent), but it is the only lever that maps to Linux's
  actual immunity. (No Linux analog of this freeze exists; the only A72 DSB-stall errata 838569/848970 are the
  MIRROR -- they stall when the bus is too BUSY.)

  **(B) Multi-core "survive the stall": FEASIBLE as a watchdog -- the hang is CORE-LOCAL.** Spec-proven (A72
  TRM 7.5/7.7 + ARM ARM): the hung tlbi;dsb wedges ONLY the issuing core (DSB completion is per-PE; the A72
  SCU services intra-cluster coherency WITHOUT touching the external ACE/SCB core 0 hangs on; the DVM-Sync is
  awaited by the issuing core's DSB). AIOS data corroborates (masked-shootdown stalled with
  aios_tlbi_ipi_sent=0). **BUT the Big Kernel Lock is the software coupling:** core 0 holds the BKL during the
  stall, so any sibling that ENTERS THE KERNEL (syscall/fault/IRQ) spins in clh_lock_acquire ~32s. Un-pinning
  helps IFF the survivor stays in USERSPACE. **MVD-1 (RECOMMENDED, low-risk):** a core-1 out-of-band watchdog
  -- userspace-only (private status page + direct mini-UART/GPIO poke), never enters the kernel, same safety
  as fabric_warm.c; reads core 0's heartbeat, emits "[WDOG] core0 stalled" out-of-band + optional HW-watchdog
  reset. The attainable "1-2 cores stuck beats whole-box wedged" win + real-time visibility. **MVD-2 (HARD,
  multi-session):** split the reaper off the I/O servers -- blocked by (1) the shared UNLOCKED global allocator
  (core-0 pinning IS the lock, boot_services.c:28), (2) net IRQ can't be retargeted without a kernel patch
  (GIC-400 ITARGETSR; no seL4 invocation sets SPI target CPU), (3) the >=2048-ASID residency fallback becomes a
  stale-TLB CORRUPTION landmine if a proc runs on core 1, (4) cross-core shootdowns RE-IMPORT the stall. AND
  even done perfectly a live net thread needs the SCB warm = the bet fabwarm LOST -> no live network shell. SMP
  re-arch backlog.

  **(C) AXI_QUIET_TIME corrects a dead-end doc.** docs/NEXT_20260619_ubus_register_deadend.md WRONGLY says
  ARM_LOCAL (0xFF800000) has "no fabric/quiesce block." BCM2711 datasheet 6.5.2 documents **AXI_QUIET_TIME @
  offset 0x30** -- IRQ "if no AXI bus traffic for a programmable time ... software can confirm bus traffic from
  the ARM cluster to VideoCore has ceased." It instruments the CAUSE + measures the keep-warm period. Only
  raises an IRQ (can't bound the hang -- the doc's broader no-writable-timeout conclusion stands). #1 new
  diagnostic; ACTION = correct the doc + map the page.

  **(D) PMU + heartbeat diagnostics: FLASHED (build 2699) + CAPTURED (serial restored by replug). 5/5
  stalls, two results.** Impl: errata.c (aios_pmu_init programs PMEVCNTR0-4 = INST_RETIRED/BUS_ACCESS/
  MEM_ACCESS/L2D_REFILL/CPU_CYCLES + aios_core_heartbeat[] + extended aios_tlbi_profile), machine.h
  (AIOS_PMU_READ5), idle.S (per-core cntpct stamp). The [TLBISTALL] line now also prints pmu=[...] + hb_ms=[...].
   - **PMU (DECISIVE, 5/5 identical): `pmu=[inst=21 bus=7..67 mem~12 l2~8 cyc=<wraps>]`.** inst~=21 (~=0) =>
     core 0 is CLOCKED but WEDGED ON ESSENTIALLY ONE INSTRUCTION (the dsb); bus < 100 across the WHOLE 32.4s
     => a CLEAN WAIT on a parked fabric, NOT a retry storm. **RETIRES the "live arbiter" branch** -- the core
     is parked on a DVM-Sync completion that never returns, issuing no bus traffic. (cyc wraps ~11x in 32s
     = a 32-bit counter, useless for duration; negatives print as huge uint64 = a cosmetic seL4 printf %d quirk
     to fix; CNTPCT remains the wall-clock source.)
   - **Heartbeat (ROUGH but a CLEAR signal -- it CHANGES the multi-core picture): the stall is NOT cleanly
     core-local in practice.** hb_ms[2,3] ~= the STALL DURATION every capture (32.4s @ 1-quantum, 64.8s @
     2-quantum) => CORES 2,3 FREEZE for the full stall. The `ipi` counter climbing (43743 -> 64593) is the
     cause: the teardown of a boot-era-residency asid (residency {0,1,2,3}) IPIs cores 2,3 (the masked-shootdown
     "separate perf bug"), and they hang in THEIR OWN tlbi;dsb on the same parked fabric. (cores 0,1 show
     hb=never/growing -- they never run idle_thread; core 0 is always busy, core 1's state needs a look; the
     probe needs polish for a clean per-core read.) **=> THE RESIDENCY/IPI BUG IS NOW A PREREQUISITE FOR
     MULTI-CORE SURVIVAL, not merely a perf bug:** clearing boot-era residency (on pinning / not marking during
     boot) makes teardowns NOT IPI siblings -> the stall becomes core-LOCAL (matching the spec's per-PE-dsb
     proof) -> a watchdog core (MVD-1) survives. Without it the remote shootdown spreads the stall cluster-wide.
     Build 2699 diagnostics are UNCOMMITTED.

  **(E) Test-validity audit (Bryan's catch).** Re-verified all 3 negative HW tests ran on a kernel WITH the
  change: every flash passed pull-back byte-verify + on-card sha + post-reboot /proc/version build#, AND the
  RESULT carried a live-only signature -- DTS trim (RAM 3813 + relocated stalls) and lazy-TLB (the asid=8 vae1
  stalls VANISHED, replaced by asid=1 + [reap] destroy=32401ms aside1) are AIRTIGHT. GAP: the V3D keep-warm was
  armed + RFC-climbing PRE-soak but NOT verified active THROUGHOUT (could have self-disarmed); the mechanism
  (non-coherent DMA) still refutes it but the empirical test is not clean. LESSON: deployed != active; passive
  mitigations need a liveness assert DURING the test.

  **NEXT-SESSION PRIORITIES (ranked):** (1) **Fix the residency/IPI bug** (clear boot-era residency {0,1,2,3}
  on pinning, or do not mark during boot; vspace.c armKSASIDResidency) -- now the GATE for multi-core survival
  (the heartbeat proved the remote shootdown spreads the stall to cores 2,3) AND the ipi-storm perf fix; then
  re-capture the heartbeat (expect cores 2,3 to survive a core-0 stall = core-local). (2) Try the **GENET
  MDIO-poll keep-warm** (the untried cure mapping to Linux's real immunity; core-1 thread, ~1 kHz GENET MDIO
  read, distinct from the refuted GPLEV0 read). (3) The cheap **over_voltage idle-voltage A/B**
  (`scripts/set_overvolt.py 6`, flash-free; expect FAIL -> closes the avenue). (4) Build **MVD-1** (core-1
  watchdog, AFTER the residency fix). (5) Polish the heartbeat probe (cores 0/1 never stamp; fix the %d-negative
  printf) + add **/proc/laststall** (serial-independent capture -- the FTDI adapter is flaky/goes stale). (6)
  Map **AXI_QUIET_TIME** + correct the dead-end doc. Diagnostics build 2699 UNCOMMITTED (keep-as-monitor vs
  strip TBD). [[project_stall_hunt]].

* **CURRENT STATE 2026-06-20 (session 2) -- THE BAND HYPOTHESIS (block below) IS REFUTED, and so are two
  more cure attempts. The ~32.4s teardown freeze is the BCM2711 SCB-fabric DVM-Sync hanging when the fabric
  has quiesced after idle, and it is UNREACHABLE from every CPU/GPU/TLB/register/clock lever. DECISION
  (Bryan): accept + mitigate; keep the lazy-TLB for its IPI-perf win. Board runs `v0.4.268 build 2691` at
  192.168.0.8 (lazy-TLB + the TLBI profiler/monitor; NO DTS trim, NO keep-warm; RAM available 3877 = full
  DRAM).** Four refutations this session, all the same root mechanism:
  1. **DTS trim (the band hypothesis) -- REFUTED.** Trimmed usable DRAM to end at 0xf8000000 (exclude the
     top 64MB). HW: the stall RELOCATED, did not vanish -- `/proc/freezes`=3, profiler `pa=0xf40c7000 /
     0xf6da9000 / 0xf4443000` (all band=0, BELOW the trim; hbT=0 = the old band is now empty yet it still
     froze). The "fatal band" was a CORRELATION ARTIFACT: the LIFO allocator drops the heavy PIPE_MMAP_ANON
     pool at the TOP of DRAM, so stalls correlated with the top band; trimming just moved the pool (and the
     stall) down. The `gap=0` "mid-burst" clue was likewise a red herring.
  2. **Lazy-TLB teardown (88 per-page `tlbi vae1` -> 1 `tlbi aside1`) -- REFUTED as a cure, but KEPT for
     perf.** Implemented in the seL4 tree (`AIOS_LAZY_TLB_TEARDOWN`, tlb.h/vspace.c/fastpath.h): unmapPage
     of a NON-current ASID defers the per-page flush (pending-flush bitmap), consumed by one aside1 at the
     next setVMRoot / fastpath switch-to / deleteASID; self-munmap (asid==current) still flushes eagerly.
     QEMU gate GREEN, HW correct. But `[reap] SLOW destroy=32401ms`: the ONE unavoidable deleteASID aside1
     (the dying-ASID flush) STILL stalls 32.4s. The stall is FABRIC-STATE-driven, not tlbi-COUNT-driven --
     the first teardown DVM-Sync after idle hangs whether there are 88 or 1. You cannot have zero
     DVM-Syncs per teardown (the ASID must be flushed before reuse). The "88x fewer" reasoning was WRONG.
     KEPT anyway: it cuts the per-teardown remote-IPI storm ~88x (the `ipi`-climbing perf bug -- boot-era
     asids resident {0,1,2,3}), a real teardown-latency/IPI win independent of the freeze.
  3. **V3D DMA bus-master keep-warm -- REFUTED.** `/proc/v3d.warm.1` -> display_server renders cube frames
     back-to-back (RFC verified climbing = continuous GPU DMA; board responsive via yield). Armed soak = 3
     freezes (asid=1 root self-munmaps), same rate as disarmed. FAILS because (a) Pi4 V3D DMA is
     NON-COHERENT -- it never joins the ARM DVM/snoop domain the teardown DVM-Sync waits on; (b) during a
     CPU-bound teardown the root monopolizes core 0, so display_server cannot kick frames -> the GPU idles
     mid-teardown anyway. Reverted (code removed; kept lazy-TLB only).
  WHY no lever works: Linux's immunity is its constant timer-tick + scheduler COHERENT activity, not DMA
  per se; AIOS's no-WFI busy-yield idle makes NO bus transactions, and no keep-warm thread can synthesize
  the coherent traffic. Combined with the handover's prior exhaustion (every CPU keep-warm, all A72
  registers, core_freq, broadcast-vs-local TLBI, SMPEN), the cure space reachable in software is EMPTY.
  Remaining theoretical avenue = a coherent external bus-master or a VideoCore-firmware fabric-retention
  knob (uncharted; the handover found no such register). Diagnostics/monitor KEPT: the TLBI profiler
  (`[TLBISTALL]`, machine.h+errata.c+vspace.c+tlb.h) + `/proc/freezes`. seL4 changes captured in
  `deps/patches/seL4-kernel.patch` (35.9KB, now includes the lazy-TLB). [[project_stall_hunt]].

* **CURRENT STATE 2026-06-20 -- ROOT CAUSE FOUND. The ~32s teardown freeze is PHYSICAL-REGION-SPECIFIC, NOT
  idle-quiescence: the unmap `tlbi+dsb` hangs ONLY for pages whose PHYSICAL address is in the top 64MB of
  usable DRAM [0xf8000000, 0xfc000000) (the band just below the 0xfc000000 peripheral window). Every
  "idle-quiescence HW watchdog" / keep-warm framing in the entries BELOW is SUPERSEDED/WRONG.** Pi runs
  **v0.4.267 build 2669** at 192.168.0.8 (the ENHANCED TLBI-STALL PROFILER; core_freq=250).
  HOW WE GOT HERE (this session, after refuting EVERY keep-warm + core_freq + A72 register -- see
  docs/NEXT_20260620_dvm_heartbeat.md): built an enhanced profiler that, per stall, reports
  core/dur/gap/bpos/bms/asid/va/**PA**/PTE/ipi/**band/hbS/lbS**/qgap. Impl: `machine.h` hook (times the
  tlbi+dsb) -> `errata.c` `aios_tlbi_profile`; `vspace.c` `unmapPage` stashes the page's paddr+pte; `tlb.h`
  counts real remote-IPI tlbis. RESULTS: (1) **gap=0 ALWAYS** -> the stall is MID-BURST; the FIRST
  post-idle tlbi is FAST -> the idle-quiescence theory (basis of fabwarm/beacon/heartbeat) is WRONG.
  (2) the stalling VA is invariant (0x10006000-0x10011000 = the process mmap region = PIPE_MMAP_ANON musl
  malloc/TLS pages) but bpos varies -> it follows the PAGE not the position. (3) the **PA is ALWAYS
  0xf8xxxxxx-0xfbxxxxxx** (10+ samples); PTE = normal cacheable inner-shareable (AttrIndx=4) -> it is the
  PADDR, not the page type. WHY ALL PRIOR FIXES FAILED: keep-warms made CPU/cache traffic, never traffic on
  THAT band's path; core_freq/registers never touched it. ROOT-CAUSE MECHANISM IN OUR CODE: the kernel
  exposes the top band as usable DRAM (`deps/kernel/src/plat/bcm2711/overlay-rpi4-4gb.dts` memory@0 range 2
  = base 0x40000000 size 0xbc000000 -> end 0xfc000000), and the LIFO root-task allocator hands out the
  HIGHEST-paddr untyped FIRST, so the heavy PIPE_MMAP_ANON pool lands in the band -> its teardown tlbis
  stall. WHY the band itself is fatal (hypothesis, not yet pinned): it routes through a path that quiesces
  after idle -- VideoCore firmware lives at top-of-RAM, or it is the memory-controller/peripheral boundary.
  (It is NOT gpu_mem: the DTS puts the GPU/fb reservation LOW at 0x3a000000-0x40000000.)
  **STAGED FIX (NOT yet implemented/tested): trim that DTS range-2 size 0xbc000000 -> 0xb8000000 (end DRAM at
  0xf8000000, excluding the top 64MB). Anon-mmap then falls to low DRAM -> no stall. V3D's 8MB pool (also
  from the VKA) moves below 0xf8000000 but the GPU MMU maps any DRAM so it is fine. Costs 64MB; also closes a
  latent kernel/VideoCore memory overlap.**
  **DO FIRST NEXT SESSION -- CONFIRM then FIX: (a) re-run `netstall.py --idle 30 --trials 10` with ONE
  serial monitor (this session a DOUBLE monitor on /dev/cu.usbserial-0001 GARBLED the band lines -- killed
  the dup; ALWAYS one reader), grep the serial for `hbS=.. lbS=..`; lbS (low-band stalls) must stay 0 while
  hbS climbs + lbT large -> root cause LOCKED. (b) implement the DTS trim, flash, netstall -> expect the
  stall GONE.** SECONDARY (separate perf bug, NOT the stall cause): ipi climbs ~thousands -- residency
  {0,1,2,3} for boot-era asids that ran on cores 1-3 before pinning; the masked shootdown silently
  broadcasts. Fix by clearing residency on pinning / not marking during boot. All diagnostics are UNCOMMITTED
  (sibling seL4 tree: machine.h, errata.c, vspace.c, tlb.h, idle.S reverted to busy-yield, crt0.S band-6/8
  experiments gated-OFF). [[project_stall_hunt]].

* **CURRENT STATE 2026-06-19 (candidate 2 DEPLOYED -- the keep-warm is now DEFAULT-ON; the board USES the
  Linux fix). [SUPERSEDED 2026-06-20 -- keep-warm is REFUTED; the stall is the top-DRAM-band physical region,
  not idle-quiescence; fabwarm is known-ineffective.]** Pi runs **v0.4.267 build 2635** at 192.168.0.8 (fabwarm armed at boot on core 1; committed
  `b3f7615`). Bryan: "we are still getting freezes so we should move to implement the linux solution" (the
  disarmed baseline froze 4x during boot alone). Made fabwarm DEFAULT-ON (`fabric_warm_start` arms +
  Signals the thread at boot; `/proc/fabwarm.0` disables -> mitigations-only baseline). **INITIAL SIGNAL
  POSITIVE: boot freezes 4 (disarmed build 2632) -> 1 (default-on build 2635)** -- the survivor is an
  early-boot teardown before fabwarm spawns mid-boot. HW: boots ARMED (armed=1, iters ~1kHz) + responsive
  (mild netconsole slowdown when armed). **VALIDATION IS EMPIRICAL/ONGOING (freeze too rare for a quick
  A/B): watch /proc/freezes over normal use vs ~2.5%/teardown; if it does NOT drop -> revert
  /proc/fabwarm.0 and try (a) spawning fabwarm earlier in boot, (b) a core-0 BLOCKING timer heartbeat,
  (c) the backlogged core_freq 250->500.** QEMU correctness gate green; SMP/shmring shed 1 timing-only
  check each under default-on = QEMU host-load artifact (always-busy core-1 vCPU), not a real-board
  regression. Mechanism + the default-INERT precursor (a06ac18) detailed below. [[project_stall_hunt]].

* **CURRENT STATE 2026-06-19 (candidate 2 -- MECHANISM SOLVED + the "Linux approach" keep-warm SHIPPED;
  the resolution A/B is the open item).** Pi runs **v0.4.266 build 2632** at 192.168.0.8 (fabwarm on core 1,
  DEFAULT-INERT). **The ~33s freeze mechanism is solved** (3-agent research + A72 r0p3 TRM): the teardown
  `tlbi ; dsb` emits a DVM Sync the `dsb` cannot retire until the BCM2711 **SCB / 128-bit AMBA fabric**
  returns DVM-Complete; after idle the SoC idles the AXI snoop interface (ACINACTM) / clock-starves that
  fabric, so the FIRST post-idle DVM Sync hangs to the ~33s SoC timeout. Linux never freezes because its
  constant timer/DMA bus traffic keeps the fabric warm; AIOS's no-WFI `yield` idle spins the CPU but makes
  NO bus transactions. This EXPLAINS every prior refutation (broadcast scope, L2ACTLR[27] -- the stall is
  the external-fabric DVM-Complete, not the A72 ISA) and CORRECTS two myths (the 0x80000=32.4s math -- it is
  8.0s @ 65536Hz; and "no BCM2711 fabric-clock register exists"). Full writeup
  **docs/NEXT_20260619_candidate2_fabric_dvm.md**; config levers (core_freq 250->500, AXI_QUIET_TIME) parked
  in BACKLOG.md. **THE FIX (Bryan's call -- the principled traffic approach, not a config knob):
  `src/servers/fabric_warm.c` + `/proc/fabwarm`** -- a DEFAULT-INERT thread that when armed busy-loops light
  UNCACHED GPLEV0 reads (~1kHz, paced via cntpct) to keep the cluster SCB AXI/snoop link warm so core 0's
  post-idle teardown DVM-Sync completes (distinct from the refuted cacheable `corewarm`: Device-memory reads
  = bus warmth with NO DVM contention). **HARD-WON LESSON (cost ~3 flash/test cycles): the keep-warm thread
  MUST run on CORE 1, not core 0.** A busy-loop on core 0 monopolizes the shell/netconsole core and hangs a
  BLOCKED shell (the board went unresponsive twice when armed -- both RECOVERED via `/proc/fabwarm.0`; no
  power cycle). Core 1 is idle-only (all AIOS threads pin to core 0) so monopolizing it starves nothing --
  the proven `core_warm` pattern. **HW-verified armed-responsive (build 2632): arm -> `sleep` + `echo`
  return, iters runs at ~1kHz (reads are FAST, not stalling on the cold fabric), disarms clean; mild
  netconsole slowdown when armed (minor fabric contention), fully inert when disarmed.** Full QEMU gate
  green (SMP 7/7, shmring 26/26, socket 8/8, netd 10/10). **Committed `a06ac18` (v0.4.266; local on main,
  Bryan pushes).** **OPEN ITEM = the resolution A/B (RUNNING / re-run if needed): does keeping the fabric
  warm actually prevent the freeze? Metric = `/proc/freezes` (NOT netstall raw timing -- the mild
  armed-slowdown perturbs it) with fabwarm DISARMED vs ARMED over a `netstall --idle 30` soak. If armed
  freezes << disarmed (ideally 0) -> THE CURE (then make fabwarm default-ON in a follow-up). If no
  difference -> core-1 warmth does not reach core 0's link; try core-0 keep-warm via a BLOCKING timer
  heartbeat (NOT a busy-loop -- that wedges core 0), or the backlogged core_freq=500 fabric-clock A/B.**
  [[project_stall_hunt]].

* **CURRENT STATE 2026-06-19 (candidate 1, SMPEN-on-secondaries) -- the LAST A72 per-core domain lever is
  RULED OUT: all four cores boot SMPEN=1. The stall hunt's A72/ISA avenue is now completely exhausted.**
  Extended the A72 boot probe to the secondary cores (the old `aios_a72_probe` only read core 0). New
  `errata.c aios_a72_probe_secondary(word_t core)` reads `CPUECTLR_EL1` under the fault-survivable
  temporary-EL1-vtable path; called from `boot.c try_init_kernel_secondary_core` right after `NODE_LOCK_SYS`
  so it runs SERIALIZED under the big kernel lock (the shared trap flag + UART are race-free, and the
  primary is spinning silently in `release_secondary_cpus()` for that window). HW serial
  (`/tmp/aios_serial.log` after a pi_flash reboot, build 2622): **core 0/1/2/3 ALL print
  `CPUECTLR_EL1=0x40 SMPEN=1 RET=0`** -- identical (L2ACTLR=0xc000010 = candidate-B clock-force, DVM
  broadcast ENABLED). So EVERY secondary is fully in the inner-shareable/DVM domain; the hypothesis (a
  secondary with SMPEN=0 hangs core-0's `tlbi` completion to the fabric timeout) is **REFUTED**. With this,
  EVERY A72/TLBI-ISA + per-core lever is ruled out (register-config gap, L2-logic clock B, full-cluster
  clock B+ [harmful], broadcast-vs-local C, and now SMPEN on the secondaries). The ~33s freeze is
  conclusively below the A72 ISA -- a BCM2711 SoC-fabric/SCU/UBUS DVM-completion quiescence.
  **DISPOSITION:** the probe is a KEEPER (permanent all-core diagnostic, parallel to the core-0 probe),
  captured into `deps/patches/seL4-kernel.patch`; **NO version bump** (still v0.4.264; running build 2622 =
  the committed local-`vae1` baseline + the all-core probe -- the broadcast build 2617 is gone). Full QEMU
  gate GREEN pre-flash (smp 7/7, shmring 26/26, socket 8/8, netd 10/10; the secondary probe fires harmlessly
  on QEMU a53 as three `(not A72)` lines). NO stall A/B was needed -- a direct register refutation, not a
  fix to soak (also spares the fragile box). The old `disk/kernel8_v264.img` revert is now MOOT (it predates
  + lacks the probe; the board is already on a committed-equivalent kernel). **NEXT = the ONLY remaining
  stall candidate: the deep AIOS-vs-Linux teardown/coherency-setup diff** (Linux on this exact Pi4 never
  freezes -- the principled multi-session fix). Mitigations kept (contain it to ~2.5%): nodes=4, masked TLB
  shootdown, clock floor 600. Full writeup `docs/NEXT_20260619_smpen_secondary_FINDINGS.md`.
  [[project_stall_hunt]].**

* **CURRENT STATE 2026-06-19 -- stability/robustness session; the BROADCAST-TLBI stall experiment is
  DONE: REFUTED. Pi runs `v0.4.264 build 2617` = the (HARMLESS) BROADCAST-TLBI kernel (`tlbi vae1is`) at
  192.168.0.8 (4-core, 1000MHz, MAC dc:a6:32:1c:2e:e1; /bin/netconsole = the v264 accept-pace binary).
  Source is REVERTED to local `vae1`; flash `disk/kernel8_v264.img` to put the board on the committed
  local baseline when convenient (the broadcast kernel has identical stall behaviour, so it is cosmetic).**
  SHIPPED + HW-verified this session (committed, local on main, Bryan pushes): **v0.4.262**
  (`3acb910`+`caa2df8`) = kernel BUILD TIME in uname/version (bump-build.sh stamps build_time.h from host
  `date`; /proc/version + `uname -v` show "Fri Jun 19 ..."), ARM clock 600->1000 (cpu_gov.c GOV_MAX +
  mksdcard arm_freq=1000) + floor 300->600 (GOV_MIN + arm_freq_min=600; caps the idle-teardown freeze at
  ONE ~33s quantum), and **the BCM2711 SCB/UBUS fabric-timeout-register cure is a CONFIRMED DEAD END**
  (two primary-source passes: no such reg exists/wired; 0x80000@16.2kHz matches no real clock =
  coincidence -- [[project_stall_ubus_deadend]]). **v0.4.263** (`840ad4d`) = graceful spawn ceiling
  (vka_audit_check_headroom pre-check in fork.c do_fork + pipe_server PIPE_EXEC, before the destructive
  teardown; free the minted new_fault_ep + reply on PIPE_EXEC configure/elf_load failure; rate-limited
  reject log -- no more sel4utils lib cascade under spawn storms; smp gate ceiling still 30). **v0.4.264**
  (`b391eca`) = netconsole 200ms accept-pace (anti-wedge; HW-validated 5/5 + 10/10 rapid reconnects, no
  box wedge). **TOOLS** (`2b7a362`, `819ba10`): `scripts/aios_nc.py` = robust HELD-connection netconsole
  driver -- **USE THIS to drive the Pi, not reconnect-per-command** (it rides the ~33s stall via a 50s
  timeout + avoids the back-to-back-conn wedge); `scripts/pi_deploy.py` = ATOMIC disk-binary deploy
  (non-forking __put to `<target>.tmp` + __get byte-verify + atomic rename) -- **USE THIS for /bin
  deploys, not cp** (a forked cp gets stall-killed mid-write -> partial binary -> corrupt service).
  **THE STALL = the session's root pain** (it killed deploys, crash-looped netconsole, killed the USB
  keyboard). **Candidate C (broadcast `tlbi vae1is`) was the last A72/TLBI-ISA lever -- A/B DONE 2026-06-19:
  REFUTED.** Ping-monitored soak (pingmon + netstall --idle 30) froze in the FIRST 2 trials: pingmon
  GAP #1 = 33.3s (real whole-system freeze) + netstall trial-0 105s severe stall. A cure needed 0 freezes,
  so the ~33s stall is INDEPENDENT of the TLBI scope (local vs broadcast). Source reverted (machine.h
  `AIOS_TLBI_BROADCAST` re-commented + a REFUTED note; the committed deps/patches/seL4-kernel.patch already
  carries it commented, so it = the local baseline). **EVERY A72/TLBI-ISA lever is now ruled out:**
  register-config gap, L2-logic clock (B), full-cluster clock (B+, harmful), broadcast-vs-local (C). The
  freeze is the BCM2711 SoC FABRIC/UBUS DVM-completion quiescence, below the A72 ISA. **NEXT stall
  candidates (deeper, multi-session RE): (1) verify CPUECTLR.SMPEN on cores 1-3** (the boot probe only read
  core 0 -- if a secondary is not fully in the inner-shareable/DVM domain, core 0's tlbi completion hangs
  on it; extend errata.c aios_a72_probe to the secondaries); **(2) deep AIOS-vs-Linux teardown/coherency
  diff** (Linux on this exact Pi4 never freezes -- the principled fix). Mitigations already shipped + kept:
  nodes=4, masked TLB shootdown, clock floor 600 (these contain it for normal use, ~2.5% residual). **INCIDENT LESSON (cost ~1h):** writing directly to a live service binary (`/bin/netconsole`)
  via `cp` OR raw `__put` gets stall-killed mid-write -> 4095-byte partial -> getty crash-loops it ->
  ~33s-stall storm + USB-keyboard death + fork-pool exhaustion. Recovery: SERIAL `cp /tmp/netconsole_new
  /bin/netconsole` (serial rides stalls -- no netconsole 30s SIGKILL; **/tmp PERSISTS, it is ext2 not
  ramfs**, so pushed binaries survive a reboot). `pi_deploy.py` exists precisely to prevent this. Memory
  consolidated 28KB->11KB. Rollback kernels in disk/: `kernel8_v264.img` (local-TLBI baseline),
  `kernel8_v264_bcast.img` (broadcast, ON PI), `kernel8_v261_candidateB_l2clk.img`. version.h = **264**.
  Full seed: `docs/NEXT_20260619_broadcast_tlbi_stall.md`. [[project_stall_hunt]] [[project_netconsole]].**

* **CURRENT STATE 2026-06-17d -- A72 stall hunt: the WHOLE A72-register avenue is EXHAUSTED -- THREE
  hypotheses disproven on real HW (register-config gap, L2-logic clock, full-cluster clock -- the last
  even HARMFUL). The stall is a BCM2711 SoC-fabric/UBUS phenomenon, NOT controllable via any A72 reg. The
  session's durable wins are a DEFINITIVE ping-monitor freeze detector + a reproducing condition
  (--idle 30). NEXT = the BCM2711 SCB/UBUS fabric-timeout register (BACKLOG #2). Pi reverted to baseline
  B (build 2604).**
  Pi now runs **candidate B, v0.4.261 build 2604** at 192.168.0.8. **The breakthrough is a redirection:**
  a fault-survivable boot probe (kernel `errata.c aios_a72_probe`) read the A72 IMP-DEF regs on the real
  Pi4 -- **all NOMINAL** (CPUECTLR=0x40 SMPEN=1 retention=0, CPUACTLR=0, L2ACTLR=0x10 DVM-broadcast-ON).
  So the seed's prime suspect (an A72 IMP-DEF config Linux sets + AIOS doesn't) is WRONG: Linux's
  __cpu_setup writes nothing imp-def, both inherit the same armstub SMPEN. The gap is RUNTIME. **Research
  (primary sources) pinned the mechanism: A72 TRM §2.4 -- the L2 control logic clock-gates after 256 idle
  cycles; on BCM2711 its wake-up sometimes sticks to the 32.4s UBUS timeout. Control = L2ACTLR_EL1[27]
  "Force L2 logic clock enable active" -- the register form of the proven nodes=4 cure.** Candidate B
  writes `L2ACTLR |= (1<<27)|(1<<26)` at elfloader `crt0.S _start` (pre-MMU; MIDR-guarded to A72;
  `AIOS_L2_CLOCK_FORCE`). build 2604 boots clean, boot probe confirms **L2ACTLR=0xC000010** (write took),
  QEMU gate smp 7/7 + shmring 26/26. **BUT B IS REFUTED ON HW -- it does NOT cure the stall.** The
  breakthrough METHOD: run netstall WITH a concurrent ICMP ping monitor (`/tmp/pingmon.py`, logs GAP>4s) --
  a ping GAP = a real whole-system freeze (kernel IRQs off stops ICMP too), a netconsole death stays
  pingable. This finally distinguishes them. Result: netstall --idle 30 trial 57 = 33.5s stall COINCIDING
  with a ping GAP=33.2s system-unreachable (SAME event) -> a real ~33s freeze with L2ACTLR[27]+[26] active
  (1/60 ~1.7%, no better than stock ~2.8%). So the L2-256-cycle-gate is NOT the (sole) cause; the
  mechanism is DEEPER (SCU/fabric/BCM2711-UBUS). Earlier B "0/30"/"0-of-120" were the stall being RARE,
  not a cure. **NEXT (now cleanly A/B-able with the ping monitor): force CORE clocks pre-MMU
  (CPUACTLR[63] mem-sys RCG / [30] main clk / L2ACTLR[28] tag-bank clk), or bound the BCM2711 SCB/ARM
  UBUS-timeout reg (default 0x80000, NOT PCIe 0x40a8 -- BACKLOG #2). Reproduce at --idle 30 (~1.7-2.8%);
  --idle 8 too weak; --idle 45+ corrupts netstall (conn-deaths). **DECISION (Bryan): B is ADOPTED as the
  new working baseline (kept -- costs only power, MAY partially help; "no better than stock" was an
  overstatement, samples too small to tell). Improvements build ON B; old-stock (b2596) kept for a final
  comparison. CANDIDATE B+ (= B + L2ACTLR[28] + CPUACTLR_EL1[63]+[30], force ALL cluster clocks) was
  TESTED + REFUTED + HARMFUL: build 2606 boot probe confirmed the writes (CPUACTLR=0x8000000040000000,
  L2ACTLR=0x1c000010) but the ping-monitored soak caught 2 freezes/72 incl a CATASTROPHIC 321.7s one
  (vs ~33s usual). So forcing the A72 clocks is NOT the lever; B+ DISABLED (AIOS_CORE_CLOCK_FORCE
  gated-off) + Pi reverted to B. Freeze tally (ping-confirmed): stock 1/36 33.5s, B 1/60 33.2s,
  B+ 2/72 321.7s. Kernels add disk/kernel8_v261_candidateBplus_coreclk.img (e7de53ef).** Candidate C
  (`tlbi vae1->vae1is`, kernel machine.h `AIOS_TLBI_BROADCAST`) built+gated but DISABLED (research:
  L2 gate affects all tlbi -> C unlikely to help; kept as fallback). Full writeup
  **`docs/NEXT_20260617_a72_cpuectlr_FINDINGS.md`**. Kernels: `disk/kernel8_v261_b2596_good.img`
  (rollback), `_candidateB_l2clk.img` (b47eb898, on Pi), `_candidateC_vae1is.img`. Side-finding: a
  pre-existing netconsole invoke-after-free cap bug ("null cap" spam under soak reconnect churn,
  non-fatal). [[project_stall_hunt]]. version.h = **261**; local-only, Bryan pushes.

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
