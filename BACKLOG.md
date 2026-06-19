# AIOS BACKLOG

Items deferred out of HANDOVER's "What is pending" so the active table
stays focused. Each entry lists what it would ship, the rough size, and
the most relevant reference. Promote items back into HANDOVER when
they're up next.

---

## RPi4 idle-teardown TLBI/DVM stall -- ~8% residual; NOT a HW limit, an AIOS/seL4 config gap (queued 2026-06-17)

**Symptom**: ~8% (2/24 on `netstall.py --idle 8`) of idle-then-process-teardown sequences freeze the
WHOLE system for 33-66s (= N x 10.8s; 32.4s == 0x80000 ticks @ ~16.2kHz = the BCM2711 UBUS-timeout
class). IRQs are off inside the kernel teardown unmap, so the tick, all threads, net, echo -- everything
stops. Bites only the idle-then-teardown edge; light/normal use is mostly fine. The masked TLB shootdown
(32dbc39) already cut it 6/16 -> ~2/24. ACCEPTED for now (2026-06-17) -- backlogged, not closed.

**UPDATE 2026-06-19 -- MECHANISM SOLVED + active approach chosen; the config levers are backlogged here.**
Candidate-2 research (3 agents + the A72 r0p3 TRM) solved it: the hang is the teardown `dsb` waiting on
**DVM-Complete from the BCM2711 SCB / 128-bit AMBA fabric**, which quiesces after idle (the SoC asserts
ACINACTM to idle the AXI master snoop interface -- an A72 INPUT software cannot deassert). Every A72-ISA
lever is refuted (incl **SMPEN on cores 1-3 = all 1**, 2026-06-19 -> direction #1 below is DONE/refuted) and
**no writable BCM2711 fabric register exists**; the "0x80000 = 32.4s @ 16.2kHz" premise is also arithmetically
wrong (the PM clock is 65536Hz -> 0x80000 = 8.0s). Linux is immune via TRAFFIC (timer/DMA keep the fabric
warm), not config. **ACTIVE APPROACH (NOT backlogged -- being HW-A/B'd): the "Linux approach" fabric
keep-warm** -- a core-0, light, periodic UNCACHED-MMIO read during idle keeps the SCB AXI link warm so the
post-idle teardown DVM-Sync completes (src/servers/fabric_warm.c, `/proc/fabwarm`, v0.4.266; distinct from the
refuted heavy-cacheable corewarm). Full mechanism + ranked levers: **docs/NEXT_20260619_candidate2_fabric_dvm.md**.
**BACKLOGGED config investigations (revisit if keep-warm falls short):**
- **(a) core_freq 250 -> 500** -- AIOS HARD-PINS the FABRIC clock low (mksdcard.py core_freq=250 +
  core_freq_min=250, for mini-UART baud stability) vs the Pi4 default 500. Raising it tests the
  fabric-clock-SPEED axis. Flash-free via `fatswap config.txt`; the mini-UART serial garbles at 500 (drive
  the A/B over netconsole + `/proc/freezes`). Cheap + decisive.
- **(b) AXI_QUIET_TIME instrument** (ARM_LOCAL+0x30, Core-0 IRQ; BCM2711 ARM-peripherals datasheet Tbl 110)
  -- arm it to timestamp bus-quiesce-onset vs stall-onset; confirms the premise + finds the minimal
  keep-warm interval for the fabric thread.
- **(c) L2ACTLR[8] DISABLE_DVM_CMO_BROADCAST** -- one-MSR A/B (LOW confidence; the [11] "regardless" clause
  means the DVM Sync likely still fires).
- Tools: **`/proc/freezes`** (v0.4.265) = passive in-use freeze-rate counter; pingmon + `netstall --idle 30`
  = the gold whole-system-freeze A/B.

**KEY: this is NOT an inherent BCM2711 fabric limitation -- Linux on the SAME Pi4 does TLBIs, idles
cores, and tears down processes with ZERO 32.4s freezes.** So AIOS/seL4 (or the firmware/armstub it
relies on) is MISSING some fabric/coherency/DVM setup that Linux does. The fix is to find that gap by
COMPARING AIOS-vs-Linux on the BCM2711, not to mine a timeout register blind.

**Narrowed cause (do NOT re-derive -- see [[project_stall_hunt]] "RESIDUAL NARROWED 2026-06-17")**: the
FIRST `tlbi vae1`+dsb DVM completion AFTER idle hangs to the UBUS timeout ("teardown-after-idle stalls,
back-to-back clean" -> the first DVM txn warms the cold fabric). RULED OUT: tlbi_probe keepalive
(redundant, A/B-proven), idle-core DVM quiescence (corewarm A/B made it WORSE not better), the Stage-S
fastpath residency hook (code-proven inert under core-0 pinning), dsb-scope (nsh == sy), and reducing
the teardown TLBI count (first-after-idle, not count-dependent -> the seL4 unmap-path change won't help).

**Linux-informed fix directions (the real work)**:
1. **Compare A72 coherency/SMP config AIOS-vs-Linux.** Suspect: CPUECTLR_EL1.SMPEN (bit 6, the A72's
   hw-coherency / inner-shareable + DVM participation enable) on cores 1-3, and the cluster power
   config. If a secondary core isn't fully in the coherency domain, core 0's tlbi DVM completion hangs
   on it. seL4/elfloader secondary bring-up may not set what Linux/the armstub sets. (CPUECTLR may be
   EL3-locked -> armstub/config.txt.) Documented in the A72 TRM -- NOT a blind hunt.
2. ~~**The BCM2711 SCB/ARM-fabric UBUS timeout register.**~~ **DEAD END (2026-06-19, two
   primary-source research passes -- see [[project_stall_ubus_deadend]] + docs/NEXT_20260619_ubus_register_deadend.md).**
   No writable non-PCIe fabric-timeout register exists or is wired on BCM2711: the GISB arbiter
   (ARB_TIMER) is STB-only (no DT node, scb=plain simple-bus); ARM-local 0xFF800000 is only the
   L1-intc + GIC-400; the ONLY UBUS_TIMEOUT is the PCIe one @0x40a8. AND the math disproves the
   premise: every documented BCM2711/2712 fabric timeout ticks at 216MHz or 750MHz, where 0x80000 =
   ms not 32.4s; the ~16.2kHz needed for 32.4s matches no real clock -> 0x80000<->32.4s is a
   coincidence, not a register. The freeze is a quiesced-fabric DVM-completion hang with NO
   software-writable bound. Do NOT blind-scan MMIO for it. SHIPPED instead (v0.4.262): clock
   severity-mitigation -- DVFS floor 300->600 + ceiling 600->1000 (cpu_gov.c, mksdcard.py); the freeze
   scales inversely with clock (600->~33s vs 300->~164s) so this caps worst-case severity (NOT a cure).
3. **Confirm-first diagnostic** (cheap, decisive): time `tlbi vae1`+dsb in invalidateLocalTLB_VAASID +
   record the per-cpu GAP before a slow (>1s) tlbi (long gap = first-after-idle; short = mid-burst),
   expose via /proc. Pins down which sub-mechanism before any fix.

**Size**: deep, multi-session HW reverse-engineering (AIOS-vs-Linux fabric/coherency compare). Probes:
`scripts/netstall.py` (idle-teardown), the `/proc/corewarm` A/B knob. **Ref**: [[project_stall_hunt]].

---

## SMP re-architecture: per-core allocators + per-core spawn/IPC servers (multikernel-grade) (queued 2026-06-16)

**The big one for real multi-core scaling of IPC-bound work.** Size: HUGE (multi-session
research effort). Refs: docs/NEXT_20260616c_smp_tlb_stall_fix.md, the v0.4.257 IPC-redesign
analysis (this session), [[project_stall_hunt]].

WHY: AIOS pipelines are IPC-bound and the pipeline CEILING is SPAWN-bound -- fork/exec/teardown
all run through the SINGLE core-0 servers (exec/pipe) + the lock-free, single-owner vka/cspace
allocator (pinning everything to core 0 is what AVOIDS the "second SSH" cap-bitmap corruption).
seL4's big kernel lock is a CLH lock taken on EVERY kernel entry -- including the fastpath
(NODE_LOCK_SYS in c_handle_fastpath_call) -- so total kernel throughput is ~1 core's worth of
syscalls/sec regardless of core count, and it is IMMUTABLE (load-bearing for verification).
Result: distributing user processes to cores 1-3 (Stage S, v0.4.257, /proc/coresched) REGRESSED
the parallel-pipeline ceiling 30 -> 6 (BKL contention + cross-core IPI + core-0 server/allocator
serialization). Naive distribution HELPS CPU-bound work (HW-proven 3.77x) but CANNOT help
spawn/IPC-bound pipelines. seL4's INTENDED SMP model is partitioned subsystems per core, NOT
shared servers -- AIOS chose shared servers (simpler, single-image), the anti-pattern for SMP.

THE IDEA: go multikernel-ish. Give EACH core its own allocator arena (partition the untyped pool
per core / a per-core sub-CNode) + its own fs/pipe/exec server REPLICAS, so a process on core N
spawns + IPCs its LOCAL servers (same-core fastpath, no cross-core IPI) from a LOCAL allocator
(no cross-core bitmap race). Then spawn + pipe I/O parallelize across cores. Shared state (the
FS, the global process table) gets explicit cross-core sync (message-passing or a coarse lock),
kept off the hot path.

HARD PARTS: (1) the BKL STILL serializes the IPC syscall itself -- per-core servers remove
cross-core IPI but each core's fastpath still takes the global BKL, so the throughput win must
come from FEWER syscalls-per-work (coalescing / SHM rings) AND local spawn-allocation, not from
the BKL. (2) per-core untyped partitioning risks fragmentation (one core starves). (3) seL4
gives the root task ONE CNode -- true per-core cspaces need careful sub-CNode partitioning +
cross-core cap transfer (seL4_CNode_Copy is cross-core-safe). (4) shared FS/process-table
consistency. (5) hardest to verify; QEMU TCG cannot show the speedup (no guest-core parallelism)
-- needs the real 4x A72.

PAYOFF: the ONLY path to scaling spawn/IPC-bound parallel pipelines past 1 core -- what Stage-S
distribution alone cannot deliver. Exactly what a research kernel exists to try. Complementary,
already-scoped first steps: pipe data-path coalescing (4KB SHM writes, prototyped v0.4.257) and
the SHM-ring pipe (producer<->consumer lock-free ring, pipe_server out of the data path).

---

## Process requirement -- deeper pre-flash smoke (queued 2026-06-14)

**Before flashing ANY kernel to the real Pi, run the FULL QEMU gate suite**, not a
targeted smoke: `netd_qemu_test.py` 10/10, `net_socket_qemu_test.py` 8/8 (flag-ON
AND flag-OFF), `ssh_qemu_test.py` 6/6 (+ `smp_qemu_test.py` for the >=30-pipeline
ceiling when capacity could move). The DVFS Phase-0 flash (v0.4.241 build 2217,
2026-06-14) shipped with only a `cat /proc/cpufreq` smoke -- consciously, a research
kernel under heat pressure -- but root-task changes (the main-loop counter + procfs)
can regress boot/net/fork in ways a targeted smoke misses. Make the full suite the
default gate; skip it only with an explicit "research kernel, proceeding" call and
say so in the deploy notes.

---

## DONE: General FAT32 file read+write (config.txt over the network) -- 2026-06-14 (ca595ce)

DONE + QEMU-verified + HW-verified on the real Pi. `find_kernel_dirent` is now
`find_dirent(name83)`; `fat32_swap_kernel` -> `fat32_write_file(target_name, ...)`;
new `fat32_read_file`. `FS_FATREAD` + `fatswap --read <target>` + `fatswap <src>
[target]` (default kernel8.img). Edit flow is plain shell: `fatswap --read
config.txt > /tmp/c; <edit>; fatswap /tmp/c config.txt`. Crash-ordered + sha-verified
like the kernel swap; edits an EXISTING root-dir file only (no create/move/reformat).
`mksdcard` now bakes `arm_freq_min=300` into config.txt. Tests: `fatconfig_qemu_test.py`
(round-trip on a partitioned MBR+FAT+ext2 disk) + `fatconfig_hw.py` (read/write/revert
on the live boot partition, marker is a harmless comment so a mid-fail still boots).
DEPLOY NOTE: the engine is in the ROOT TASK -> a kernel flash, AND the `fatswap`
command is a DISK app -> a `pi_filexfer push /bin/aios/fatswap` (the first HW attempt
failed because only the kernel was flashed, leaving the old fatswap CLI -- harmless,
the old CLI rejected the new args with a usage error, no FAT write). NOT a real mount
at `/boot` (still single-file, root-dir only); a full mount stays a future item.

---

## Queued epics (2026-06-15)

- **TCP graceful close + tail retransmit** -- fixes the SSH last-command drain race
  (AIOS TCP had no sender-side retransmission). IMPLEMENTED + COMMITTED `3e3e26a`
  (net_server.c): snd_una + 4KB retransmit ring + RTO resend + deferred graceful close +
  FIN retransmit. Two read-only adversarial reviews (lifecycle hardened). QEMU
  no-regression GREEN (socket 8/8 ON+OFF, ssh 6/6, reconnect 6/6, netd 10/10). **STILL
  PENDING: the HW flash + loss-path verify** (QEMU/SLIRP is lossless; net_server is in
  the kernel/netd so a bad flash could drop the Pi's net -> needs Bryan's go-ahead + a
  short-session loss-rate repro). No version bump until HW-verified. Spec/review trail:
  `docs/NEXT_20260615c_tcp_graceful_close.md`.
- **V3D textured console epic** -- (a) a textured (`art/aries_screen.png`) spinning +
  bouncing cube boot splash -> text; (b) a fast V3D-accelerated text console; (c) emoji.
  ALL gated on one new V3D capability: TEXTURE MAPPING (the TMU). Staged like Phases 2-4.
  Seed: `docs/NEXT_20260615d_v3d_textured_console.md`.
- **HCI keyboard HOTSWAP** -- ROOT-PORT hotplug IMPLEMENTED + QEMU-VERIFIED + COMMITTED
  `07fa756` (xhci.c): evt_dispatch flags port-status changes, the driver thread reconciles
  root ports (enumerate new / teardown vanished), device_teardown reclaims the slot + DMA
  (new dma_free + freelist). `scripts/xhci_hotswap_qemu_test.py` PASS (unplug->teardown->
  replug->re-enum, slot reused). PENDING: VL805 DOWNSTREAM-hub hotplug (RPi4 kbd is behind
  the hub -> needs the hub interrupt-IN status pipe, HW-only), the 2 enum scratch-page
  leaks, a real-keyboard verify on the Pi (kernel flash, holding for Bryan), and the
  ROBUSTNESS sweep (xHCI error/recovery -- the scoping lens that failed). Details:
  `docs/NEXT_20260615e_hci_robustness_hotswap.md`.

## Next up -- recommended order (queued 2026-06-03)

Execution order set after the v0.4.143 pipe-EOF fix shipped: reliability
first, then a clean feature, then efficiency, then the high-risk/hardware
item. Intended to be `/schedule`-d as one-per-day sessions; recorded here so
the order survives regardless of the scheduler.

1. **Harden pipes under load -- INVESTIGATED 2026-06-03, DEFERRED (resource
   ceiling).** Root cause is NOT the pipe path: it is a **resource ceiling** --
   `MAX_ACTIVE_PROCS = 16` (root_shared.h, BSS-shift hazard to change), VKA pool
   8000 pages, morecore 6 MB/proc -> ~16 concurrent procs max. Under heavy
   concurrency the failures CASCADE: VKA/slot pressure -> PIPE_EXEC/do_fork fail
   (EPERM / "Cannot fork") -> a reader that fails to exec leaves the writer with
   no reader -> the writer's bytes are dropped (PIPE_WRITE `written<wlen`). 3+
   concurrent QEMU boots also overwhelm the host. These are largely artifacts of
   ARTIFICIAL multi-QEMU host-CPU contention; single-instance + real RPi4 work
   reliably. A secondary, genuine pipe-write **data-loss** bug exists (client
   advances `sent += chunk`, ignoring server `written`; 4096 ring drops overflow
   when the reader lags). A client busy-yield fix was tried (v0.4.144) and
   REVERTED -- it busy-spins on a full ring, adding pressure and deadlocking late
   readers. The only safe data-loss fix is server-side NON-spinning writer
   blocking (mirror pipe_read_blocked -> pipe_write_blocked, stash + resume on
   drain, EPIPE on read_closed) -- but it does NOT fix the load ceiling. A real
   "harden" needs capacity/admission work (swap, footprint reduction, careful
   limit raising) -- large, low payoff for real use. See docs/NEXT_20260603b.md.
   Repro: 2-3 concurrent QEMU `--smp 4` on separate disk copies (`--no-mirror`);
   an unclean QEMU kill corrupts `disk_ext2.img` -- regenerate via mkdisk.py.
   - **UPDATE 2026-06-07 (ceiling RAISED):** bumped `MAX_ACTIVE_PROCS` 16->48,
     `MAX_PIPES`/`MAX_ZOMBIES` 16->48, `PROC_MAX` 32->80, `MAX_WAIT_PENDING` 8->16.
     The feared "BSS-shift hazard" did NOT materialize -- QEMU -smp 4 boots clean
     and the parallel-pipeline ceiling went 6 -> 22 (verified by
     `scripts/smp_qemu_test.py`: fork-width probe clean to W=22, races exact). The
     binding limit at 48 is still the `active_procs` table (W=24 = 48 procs +
     system overflows it). See the ceiling investigation in HANDOVER for how high
     it can go and the next wall (VKA pool / per-proc footprint).
   - **UPDATE v0.4.181 (footprint -- ELF demand-text DONE, Phase 1):** the wall
     above the table is per-proc resident footprint (NOT eager morecore -- that
     is already demand-paged: `BSS lazy pages=1580`). `pipe_server.c`
     `setup_demand_text` now demand-pages the read-only ELF text from the
     executable file (reusing the v0.4.146 file-fault engine), so a proc keeps
     resident only the code it executes (`text lazy pages=75` per storm proc),
     not the whole statically-linked binary. Boots clean, executes correctly
     (executability via Default_VMAttributes). Table raised 48->64 (ceiling ~30;
     W=24 now fully clean). **Phase 2 (bigger win, not yet done): SHARE one
     read-only `.text` copy across same-binary procs** (root keeps a {binary ->
     text frames} cache) -- one 75-page copy for N procs instead of N. Also still
     open: the netconsole relay stall caps how WIDE you can drive (separate item
     above), and a per-proc resource leak under sustained storms (the Race-B
     cascade in `scripts/smp_qemu_test.py`).

**Netconsole relay stalls under heavy concurrent output** (debug transport, LOW
priority; found 2026-06-07 building the SMP test). Driving a `>~8`-wide
`seq | wc -l &` storm THROUGH netconsole: every wc output arrives CORRECTLY, but
the trailing `aios# ` prompt never comes (the relay does not deliver the prompt /
detect the `dash -c` exit under the burst). NOT a fork/SMP bug -- the fork-width
probe runs clean to W=22 and the outputs are correct; only netconsole's prompt
framing stalls. Likely the relay's EOF/drain handling under a burst of concurrent
writers (same family as the netconsole receive bottleneck, [[feedback_netconsole_push_speed]]).
Test workaround: drive repeated rounds at width<=8 with a settle. Real fix:
harden the netconsole relay (bulk drain + reliable end-of-command framing) --
`src/apps/netconsole*.c` + the pipe relay.

2. **file-backed mmap** -- new POSIX VM feature; see the Medium-risk entry
   below. Self-contained, QEMU-testable, no hardware. ~300 LOC.

3. **COW Step 3 -- wc/shutdown post-promotion EPERM** -- efficiency win; see
   the Medium-risk entry below. One focused session, ~30 LOC + tracing.

4. **RPi4 SMP bring-up -- DONE v0.4.179 (2026-06-07, HW-VERIFIED).** SMP=4
   (`settings-rpi4.cmake KernelMaxNumNodes=4`) boots all 4 A72 cores via the
   elfloader spin-table (`Boot cpu id = 0x0` -> `Core 1/2/3 is up`); the kernel
   bootstraps 4-core SMP, AIOS boots, DHCP 192.168.0.8, ping 0% loss; `/proc/hw`
   cores=4, `/proc/version` says "4-core SMP".
   - The long-blamed `smp_boot.c:119` `while (!is_core_up(num_cpus))` hang was a
     **GHOST -- never an SMP bug.** The bring-up was INVISIBLE: the elfloader
     aux-uart driver (`bcm-uart.c` `bcm2835_uart_init`) skipped
     `uart_set_out(dev)`, so `plat_console_putchar` stayed NULL and every
     elfloader printf no-op'd. The v0.4.178 `dtoverlay=disable-bt` "make it
     visible on PL011" attempt made it WORSE: the elfloader console is
     serial1=the mini-UART (build-time DTB stdout-path), so disable-bt only
     disconnected the trace from the cable AND broke the kernel boot (root task
     drives the mini-UART) -> Pi unreachable, masquerading as a SMP hang.
   - **Fix:** register the elfloader mini-UART console (gitignored deps
     `bcm-uart.c` `uart_set_out`; its putchar is already bounded) + revert
     disable-bt -> known-good mini-UART @115200, `core_freq=250` (`mksdcard.py`
     + `hw/rpi4/config.txt`). Then elfloader + kernel + login all land on the
     same cable. See the `project_rpi4_smp` memory + `hw/rpi4/BOOT_NOTES.md`.

---

## Medium-risk

### RPi4 power/thermal -- DVFS (lower ARM clock at idle, not WFI)
- **Symptom**: the Pi runs very hot. The cause is AIOS's own idle policy, not the
  firmware: to keep the v0.4.228 TLBI/DVM stall cured, all 4 A72 cores idle-SPIN
  (no WFE/WFI) so the SCU stays clocked ([settings-rpi4.cmake:33-41](settings-rpi4.cmake);
  the root idle is `while(1){ seL4_Yield(); }` at [aios_root.c:582](src/aios_root.c:582)).
  So the cores never enter low-power idle -> ~full draw -> heat. The obvious fix
  (WFI at idle) is exactly what RE-OPENS the stall, so it is off the table until
  the stall is re-cracked (hard, separate; residual spawn-storm stall already open).
- **What ships**: cut dynamic power WITHOUT deep idle by lowering the A72 CLOCK --
  cores keep spinning at a lower freq, so the SCU stays clocked (stall-safe), and
  the firmware drops voltage with frequency (power ~ f*V^2 falls well). Two tiers:
  (a) a STATIC boot-time ARM-clock cap (immediate heat cut, throughput trade);
  (b) a LOAD-DRIVEN governor -- drop to min when the root idle loop is hot, raise
  to max under load. Reconciling rule: idle == LOW CLOCK, never WFI.
- **Already half-built**: the VC-mailbox clock path exists --
  [src/gpu/v3d.c:284-348](src/gpu/v3d.c:347) has VC_TAG_SET_CLOCK_RATE /
  GET_CLOCK_RATE + a working `v3d_vc_tag()` helper. Reuse it with the ARM clock id
  (CLK_ARM = 3). A static cap is a few LOC; the governor needs a load signal + a
  small control loop. Temp read is just as easy (GET_TEMPERATURE tag 0x00030006).
- **Size**: static cap ~1 session; load-driven governor ~1-2 sessions + real-Pi
  thermal tuning.
- **Risk/verify**: HW-only. Confirm a lower clock does NOT re-trigger the stall
  (cores still spin, so it should hold) and does not disturb the mini-UART baud
  (tied to the SEPARATE core_freq=250). Firmware already throttles ~80-85C, so
  this is about running cool, not safety.

### COW Step 3 -- wc/shutdown post-promotion EPERM
- **What ships**: enables `COW_STRIP_PARENT 1`. With strip on, dash forks
  for `wc`/`shutdown` post-promotion fail with EPERM. Mechanism is
  proven (parent_promotions counts, no kernel errors); a downstream
  state divergence kills subsequent fork+exec.
- **Repro**: flip the gate in [src/process/cow.c:44](src/process/cow.c).
- **Plan**: instrument `do_fork`'s 12 `return -1` paths to find which
  fires post-promotion. Most likely culprits: cap allocation interacting
  with the orphaned parent_cap, or a child cspace cap copy that ends
  up wrong.
- **Size**: ~30 LOC of fix on top of the diagnostic; one focused session.
- **See**: [docs/NEXT_20260503a.md](docs/NEXT_20260503a.md).

### Block cache write-back
- **What ships**: switch from write-through to write-back, with periodic
  flush. AIOS fs traffic is currently too low to make this measurable.
- **Size**: ~150 LOC.

### file-backed mmap
- **What ships**: `MAP_SHARED` on a regular file. Extends `PIPE_MMAP_ANON`
  with a file path + offset, fs_server reads the page into a fresh frame,
  caller maps it. `msync` for write-back is the hard bit.
- **Size**: ~300 LOC.

### COW Step 4 -- stack COW
- **What ships**: probe parent's stack tightly, share via `cow_setup_segment`.
  Previous attempt (NEXT_20260502b) collided with child's IPC buffer;
  bound the probe to the actual stack range.
- **Size**: ~200 LOC. Depends on Step 3 working in production.

### COW Step 5 -- parent-dies safety
- **What ships**: today, child holds R/O dups of parent's frames; if parent
  dies and `vspace_tear_down` frees the underlying frames, child caps
  dangle. Needs cookie-ownership transfer at fork time (or refcount-driven
  free in `cow_frame_release`).
- **Size**: uncertain, touches sel4utils internals.

---

## High-risk

### Server health probes -- full (with auto-restart)
- **What ships**: extends v0.4.121 ping probe with restart on stale
  age. Detecting death is easy; restoring server state across restart
  is the hard part (BSS-resident state, in-flight reply caps,
  registered clients).
- **Size**: ~400 LOC.

---

## Long-term research

### Swap / paging out
- **What ships**: anonymous-page eviction to disk + page-in on fault.
  Needs a swap area, an LRU policy across active_procs vspaces, and
  fault-handler integration.

---

## Known bugs & limitations (low-priority)

### netconsole "null cap" console spam under reconnect churn (invoke-after-free, NON-FATAL)
- **Symptom**: under a netconsole soak with many connect/teardown cycles (e.g.
  `python3 scripts/netstall.py --host 192.168.0.8 --trials 30 --idle 30`), the
  kernel debug console spams
  `<<seL4(CPU 0) [decodeInvocation/643 T0x... "child of: 'rootserver'"]: Attempted
  to invoke a null cap #19718.>>`, alternating between two specific cap slots
  (e.g. #19718/#19719). Appears ONLY during reconnect churn and STOPS the instant
  the soak ends. NON-FATAL: the kernel rejects the invalid invocation and
  continues; the system stays fully responsive (a gentle one-shot netconsole cmd
  returns ~1.3s, ping clean). Found 2026-06-17 during the A72 stall hunt.
- **Diagnosis**: a latent invoke-after-free / invoke-stale-cap in the netconsole
  connection-teardown path. A "child of: 'rootserver'" thread (a spawned server,
  likely netconsole/net_server) invokes a per-connection cap AFTER it has been
  freed; the two repeated slots are two stale connection references.
- **Where to look**: `src/servers/net_server.c`, `src/boot/spawn_netd.c`,
  `src/net/net_tcp.c` -- connection accept/close + the per-connection cap. Fix =
  stop invoking the connection's cap after free (guard the invocation, or fix the
  close/free ordering so the stale reference is cleared).
- **Repro**: netstall on the real Pi (192.168.0.8); watch `/tmp/aios_serial.log`
  (a serial monitor mirrors there). Distinguishes itself from a real freeze: the
  Pi stays pingable throughout (see the ping-monitor method, [[project_stall_hunt]]).
- **Severity**: LOW (console noise only; no functional impact). Worth cleaning for
  connection-lifecycle correctness + to de-noise the console during net soaks.

### GENET real-MAC read fails -> Pi takes .127 not .8 (HARMLESS)
- **Symptom**: on the real RPi4 the board takes DHCP lease `.127` (the fake
  fallback MAC dc:a6:32:01:02:03) instead of `.8` (the real MAC). HARMLESS -- the
  Pi works at either IP; check both. Appears consistent since HDMI+v3d were
  enabled (v0.4.168+); the older note in [[genet-umac-swinit]] called it
  "intermittent".
- **Root cause (HW-narrowed, build 2171)**: net_genet's mailbox MAC read
  (`genet_mbox_call` / `read_mac_from_mailbox`) returns `ret=-1` EVERY time --
  confirmed by a fully-settled post-boot `cat /proc/genet.mac` (so it is NOT a
  boot-timing race). Yet `display_vc`'s `mbox_call` to the SAME VC property
  mailbox (channel 8) SUCCEEDS at boot ("Display server ready 1024x768"). The
  mailbox HW works; net_genet's CALL is broken.
- **Ruled out** (all checked on HW): high DMA address (genet_dma is low ~4MB);
  display contention (net inits at aios_root.c:384, before display at :387);
  VC-not-ready / read-too-early (it fails fully post-boot too).
- **Prime suspect**: tag-buffer region or cache coherency. `display_vc` PINS its
  tag buffer low at `MBOX_TAG_PADDR=0x3A000000` (the v0.4.168 HDMI fix);
  net_genet uses `genet_dma+0x10000`. Compare the two `mbox_call` paths: tag
  placement, the `|0xC0000000` bus alias, cached-vs-cleaned tag write. Likely
  fix: give net_genet a VC-reachable (pinned-low, non-cached, cleaned) tag buffer
  like display_vc's.
- **First step**: instrument `genet_mbox_call` (log WHICH poll fails + `buf[1]`),
  or just point `read_mac_from_mailbox` at display_vc's proven tag region, then
  one reflash.
- **CLEANUP owed**: the v0.4.234 retry (genet_init, 3x) + v0.4.235 deferred
  re-read (net_server, 5x) each spin the ~2s mailbox timeout for nothing (~14s of
  wasted boot polling, all failing). REVERT both to a single attempt as part of
  the real fix.
- **Size**: ~1 instrumented reflash + ~20 LOC. **See**: [[genet-umac-swinit]].

### PTY/SSH: last command before `exit` can lose its output (queued 2026-06-06)
- **Symptom**: in an interactive PTY session (observed over SSH), the LAST
  command's stdout immediately before `exit` can be dropped -- the client
  never receives it. Identical pipelines earlier in the SAME session work.
- **Repro (automated)**: SSH in and feed one input blob
  `echo abc | wc -c\nls /bin | wc -l\necho hello | rev\nexit\n`. The first two
  print (`4`, `114`); the third (`echo hello | rev`, last before `exit`) prints
  nothing client-side. Reordering it earlier makes it print -- so it is
  position-before-exit, not `rev`-specific.
- **Hypothesis**: a relay-teardown drain race. When dash runs the last command
  then `exit`, it writes output to its stdout pipe and exits ~immediately; the
  registered-writer EXIT latches the pipe `write_closed`, and the relay's
  non-blocking `read()==0` (EOF) path in `ssh_channel.c:channel_relay` may fire
  and tear the channel down before the final buffered bytes are drained +
  framed to the socket. (Pipe semantics SHOULD return buffered data before EOF,
  so this needs confirming -- it may instead be a dash exit-flush issue, or a
  test-capture timing artifact.) Same family as the rc=255 cosmetic (sshd never
  sends `exit-status` before CHANNEL_CLOSE per RFC 4254 6.10).
- **Where to look**: `src/ssh/ssh_channel.c` `channel_relay` -- on `read()==0`,
  do a final drain of any remaining pipe bytes before send_chan_eof/close; and
  dash's stdout flush on `exit`. Also check the A72 pipe-SHM coherency window
  (the relay may observe the writer EOF before the last written bytes are
  coherently visible -- QEMU cannot model it).
- **ROOT CAUSE FOUND 2026-06-15 (code-traced) -- it is the TCP STACK, not the pipe.**
  The pipe read path is serial-correct (`PIPE_READ`/`PIPE_READ_SHM` serve all `count`
  bytes before EOF; the SHM xfer is coherent across the IPC reply), and the socket
  send NEVER returns EAGAIN (`net_server.c` NET_SENDTO for TCP always returns `len`
  when ESTAB). The real bug: **AIOS TCP has NO sender-side retransmission.** The
  `net_socket` struct has `snd_nxt` but NO `snd_una` (highest-ACKed), so it cannot
  track unACKed data; `net_tcp_send` is fire-and-forget (no retransmit queue, no RTO
  timer), and `NET_CLOSE_SOCK` sends FIN + frees the socket (`in_use=0`) WITHOUT
  draining unACKed data. So the last pre-close segment, if lost on the wire or dropped
  by a momentarily-full client window, is gone -- client sees FIN, no data. A72-only
  because SLIRP loopback is lossless + instant. (All AIOS "retransmit" code is the
  RECEIVE side, relying on the PEER to retransmit to us.) **Fix is TCP-layer, not
  sshd-local**: add `snd_una` + a retransmit queue + RTO timer (retransmit unACKed on
  timeout / 3 dup-ACKs) + a GRACEFUL close (hold the socket in FIN_WAIT until
  `snd_una == snd_nxt`, retransmitting the tail). Fixes ALL outbound TCP reliability
  (sshd, netconsole push, fatswap), not just this symptom. Sizeable (~hundreds of LOC,
  careful state machine); QEMU cannot exercise the loss path -- needs the real-Pi
  deploy-over-net verify loop. A narrower interim: graceful-close-with-tail-retransmit
  only (still needs `snd_una` + a small unACKed buffer).
- **CONFIRMED ON HW (v0.4.178 deploy).** Once reconnect was fixed and many
  sequential SSH sessions ran on the real RPi4, this race became visible:
  ~35% of short sessions (`echo X; exit`) intermittently disconnect with
  "session ended" and NO output. QEMU got 6/6 (no cache lag); the Pi got 5/8,
  failing on conn 3/4/7 but PASSING 5/6 after -- so sshd RECOVERS, it is not a
  hard limit. This is now the main reconnect-reliability gap on hardware (the
  two reconnect leaks themselves are fixed). Interactive humans type commands
  then `exit` separately, so they still see output; the loss only bites the
  last-command-immediately-before-exit / one-shot pattern. NOT yet investigated.
- **Also makes `scp` return rc=1 on HW (v0.4.178 scp/sftp).** The transfer
  itself is correct (byte-verified both ways), but the channel `exit-status 0`
  packet is among the last-bytes-before-close that the A72 drops, so scp (which
  treats a missing exit-status as failure) exits non-zero. On QEMU it arrives ->
  scp rc=0. `sftp` is lenient and is rc=0 even on HW. So fixing this race also
  cleans up scp's exit code on hardware.

### zsh hangs over SSH -- ZLE raw-mode / termios not honored by the relay (queued 2026-06-06)
- **Symptom**: launching `zsh` inside an SSH session wedges -- typed commands do
  not run, `Ctrl-C` and `exit` do nothing. dash over SSH is completely
  unaffected. (Also: zsh emits OSC color / DA terminal queries whose replies the
  SSH relay echoes back as visible `]11;rgb:.../[?1;2c` noise, and `zsh/compctl`
  fails to load -- AIOS zsh is static, no loadable modules.)
- **sshd-wedge SELF-HEAL: RESOLVED (v0.4.178) + EMPIRICALLY CONFIRMED 2026-06-15.**
  The old claim ("a hung shell takes sshd DOWN for all future connections") is
  STALE. `channel_relay` cleanup already does `kill(child, SIGKILL)` (when the loop
  ended without a clean shell EOF) before `waitpid`, and `kill(SIGKILL)` destroys
  the target DIRECTLY inside pipe_server (`handle_child_fault` -- it does not need
  the blocked `dash` to run code), creating the zombie that `waitpid` then reaps at
  once. Reproduced on QEMU (`/tmp/ssh_wedge_repro.py`: PTY ssh -> `sleep 120` so
  dash parks in `waitpid` -> abrupt client SIGKILL -> reconnect): **3/3 cycles the
  next connection succeeded** (the session `dash` was reaped each time, 2->1). So
  the "robustness sub-fix" is already in place. (Aside: the BACKLOG's proposed
  `waitpid(WNOHANG)` form is moot anyway -- AIOS's `waitpid` shim ignores `options`,
  [posix_proc.c:62](src/lib/posix_proc.c); the AIOS idiom is a `/proc/status` poll.)
- **STILL OPEN -- orphaned-grandchild LEAK (low severity).** When the relay kills
  `dash`, a grandchild `dash` itself spawned (the hung `zsh`, or `sleep` in the
  repro) is ORPHANED, not killed -- the repro saw `sleep` climb 1->2 across cycles.
  It accumulates only on abrupt disconnect WHILE a foreground child runs (normal
  `exit` leaks nothing); slow path to the proc-table cap. Cannot be fixed
  sshd-locally: `/proc/status` exposes no PPID (pipe_server `active_procs` has it,
  procfs does not). Proper fix is a CORE change -- have `handle_child_fault` kill/
  reparent a destroyed process's children -- or expose PPID so sshd can walk+kill.
- **STILL OPEN -- zsh raw-mode usability (separate, Medium):** zsh itself does not
  work over SSH (the cooked relay vs ZLE raw-mode fight, below). That is a
  usability gap, NOT a service-wedge anymore. Fix = make the SSH channel
  termios-aware (mirror the local tty_server path).
- **Cause**: `ssh_channel.c` (`channel_relay` + `process_input`) implements a
  FIXED cooked-mode server-side line discipline -- it echoes chars, line-buffers,
  and sends a whole line to the shell on Enter. dash expects exactly that. zsh
  drives ZLE, which sets the PTY to RAW mode via termios and wants
  character-at-a-time input + cursor control; the SSH relay IGNORES the shell's
  termios request and keeps line-buffering/echoing, so ZLE and the relay fight ->
  hang (input never reaches zsh as it expects; Ctrl-C/exit included). zsh works
  on the LOCAL console because that path honors termios via IPC (tty_server,
  [[is_tty_routing]] / v0.4.99 ZLE) -- only the SSH relay lacks it.
- **Fix direction**: make the SSH channel termios-aware. When the shell puts the
  PTY in raw mode, drop the server-side echo + line-buffering and relay raw bytes
  both directions, letting the shell own echo/line-editing (mirror the local
  tty_server termios path; sshd would need a termios channel to the shell, or to
  honor the pty-req modes + a SET_TERMIOS hook). Medium effort; also unlocks any
  raw-mode / full-screen app over SSH (vi-like editors, pagers, top-like UIs).
- **Workaround**: use dash (`#`) over SSH; do not launch zsh. Found v0.4.178.

## Tooling polish (small but deferred)

### Smoke-driver flakiness
- The python smoke driver occasionally fails to reach the dash prompt
  on first run (timing race with QEMU + getty + login). Workaround:
  retry once. Worth investigating with explicit prompt polling rather
  than fixed sleeps.

### SIGSEGV / fault-observation harness
- Would let us actually verify `mprotect(PROT_NONE)` faults reads, and
  `mprotect(R/X)` clears XN. Today the IPC return is real but the user
  has no way to observe the page-fault outcome.
