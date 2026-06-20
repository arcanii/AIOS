# NEXT (seed) 2026-06-20 -- exec-teardown TLBI/DVM stall: profiler + DVM-heartbeat

Session continues the BCM2711 idle-teardown ~32.4s whole-system freeze hunt. The
keep-warm "Linux approach" class is now exhausted; the profiler pinned the
mechanism precisely; the DVM-heartbeat is the current candidate.

## 1. The profiler (SHIPPED, build 2638+, diagnostic -- uncommitted)

- `deps/kernel/include/arch/arm/arch/64/mode/machine.h`: `invalidateLocalTLB_VAASID`
  times each `tlbi+dsb` (cntvct around it) and calls `aios_tlbi_profile`.
- `deps/kernel/src/arch/arm/machine/errata.c`: `aios_tlbi_profile` prints
  `[TLBISTALL] dur=.. idle_gap=.. axi_out=.. axi_q=.. n=..` for any tlbi >= 1000ms.
  AXI fields are weak n/a (0xffffffff) -- bcm2711 does not map ARM_LOCAL (V2 TODO).

### RESULT (HW-captured, the headline)
- The freeze is **ONE `tlbi vae1; dsb` = 32399ms**, identical to the millisecond
  across EVERY capture (n=1..16). => a **deterministic SoC fabric watchdog**, not a
  slow op: the DVM-Sync cannot complete while the snoop path is quiesced, and a
  fixed timeout force-completes it at ~32.4s (= 3 x the 10.8s quantum).
- Multi-quanta freezes (65485ms = 2x, 97892ms = 3x) are ONE teardown doing
  several tlbis, 2-3 of which each independently hit the 32.4s timeout.
- `idle_gap` is contaminated (shared global across cores, SMP) -- per-core is a
  V2 refinement; not needed so far.

## 2. Keep-warm class = REFUTED (the user-chosen "Linux approach" is exhausted)

netstall `--idle 30` forces the teardown-after-idle; ~100% baseline stall rate.

| kernel | keep-warm | mechanism | netstall result |
|--------|-----------|-----------|-----------------|
| 2638 baseline | fabwarm (device GPLEV0 read, core 1) | AXI READ channel | 7/7 stalled |
| (prior) corewarm | heavy 256KB cacheable RMW x3 cores | intra-cluster snoop -> DVM CONTENTION | refuted (worse) |
| 2641 beacon | light cacheable RMW, idle loop, all cores | intra-cluster snoop (SCU) | 3/3 stalled |

**Why they all fail:** BCM2711 is a single 4xA72 cluster. Cache-coherency snoops
are handled INTRA-cluster by the SCU and never reach the external SCB fabric where
the DVM-Sync hangs. Device reads ride the AXI READ channel, not the coherency/DVM
channel. Neither puts traffic on the **SCB DVM path** that `ACINACTM` gates.
(The beacon was a lighter rediscovery of the already-refuted corewarm.)

Also note: idle.S has been busy-yield (no WFI) since v0.4.217 with a comment
claiming it "eliminated" the stall (0/12) -- but 0/12 at a ~2.5% rate has P~0.74 of
showing zero by luck; the profiler proves busy-idle does NOT fix it (yield emits no
fabric traffic at all).

## 3. DVM heartbeat (build 2645, CURRENT candidate -- Phase C A/B running)

`deps/kernel/src/arch/arm/64/idle.S`: idle cores fire a paced (1ms, cntvct),
**dsb-less** `tlbi vae1is` over cycling unmapped low VAs (ASID 0 -> no-op invalidate,
varied so ops don't coalesce). Theory: per A72 TRM 7.7.4 the tlbi OPERATION is
broadcast eagerly; only the following `dsb` emits the DVM SYNC that stalls. So a
dsb-less tlbi = DVM-channel traffic that warms the snoop path with no Sync to hang
on. The one untried traffic class (DVM, not cacheable/device).

- 10us pacing thrashed QEMU softmmu TLB (no boot in 150s) -> slowed to 1ms; onset
  is seconds-scale (passive ~2.5%, idle-30 forces it) so 1ms has huge margin.
- QEMU gate green (correctness): socket 8/8, netd 10/10, smp 4/5 + shmring 25/26
  (one host-load timing shed each, same as prior default-on configs).
- Early-boot freezes still occur (busy phase, no core idle => heartbeat inactive);
  not a steady-state signal.

### Phase C verdict (build 2645, dsb-less): REFUTED -- 8/10 stalled, several ~900s.
### Phase D verdict (build 2649, tlbi vae1is + dsb ish, Linux-exact): REFUTED -- 2/2 stalled.

## 3b. CONCLUSION: the CPU-traffic keep-warm class is DEAD (5 variants)

| variant | traffic | result |
|---------|---------|--------|
| fabwarm | device read (AXI read ch) | refuted |
| corewarm | heavy cacheable RMW x3 | refuted (worse) |
| beacon | light cacheable RMW idle | 3/3 stalled |
| dsb-less tlbi | DVM op, no Sync | 8/10 stalled |
| tlbi + dsb ish | DVM op + Sync (Linux-exact) | 2/2 stalled |

Even firing tlbi+dsb ~1kHz on every idle core <1ms before the teardown does NOT
keep the teardown's DVM-Sync warm. So **no CPU-generated traffic** (device,
cacheable-coherency, or DVM/tlbi) prevents the BCM2711 SCB ACINACTM quiescence.
This matches Linux's actual profile: Linux stays warm via **DMA bus masters**
(GENET/USB/VideoCore/PCIe continuously moving data across the SCB), NOT via CPU
ops. AIOS idle has no such background DMA -> the fabric quiesces. => a CPU-side
keep-warm cannot work; warming requires a continuous BUS-MASTER (DMA) on the SCB.

## 3c. A72 IMP-DEF register levers -- ALSO EXHAUSTED (2026-06-20)

Authoritative bit map from ARM Trusted Firmware `cortex_a72.h`. L2ACTLR_EL1
(S3_1_C15_C0_0): [28]=FORCE_TAG_BANK_CLK, [27]=FORCE_L2_LOGIC_CLK,
[26]=FORCE_L2_GIC_TIMER_RCG, [11]=DISABLE_DSB_WITH_NO_DVM_SYNC, [8]=
DISABLE_DVM_CMO_BROADCAST, [6]=DISABLE_ACE_SH_OR_CHI. Set pre-MMU in elfloader
crt0.S (MIDR-guarded to A72, so inert on QEMU a53).

| candidate | bits | probe | result |
|-----------|------|-------|--------|
| B clock-force | L2ACTLR[27:26] | c000010 | refuted (build 2617) |
| B+ core clock | CPUACTLR[63]+[30] | - | HARMFUL (10x worse, 321s) |
| D no-DVM-broadcast | L2ACTLR[8] | c000110 DVMDIS8=1 | refuted (build 2652, still 97s) |
| E no-ACE-shareable | L2ACTLR[6]+[8] | c000150 | refuted (build 2653, still stalls) |

Even DISABLING the ACE shareable interface entirely (E) does not stop the DVM
Sync reaching the fabric. CPUECTLR SMPEN=1/RET=0 nominal; the armstub is the
STOCK RPi one (== Linux); Linux's `__cpu_setup` writes nothing IMP-DEF. So there
is no boot-register cure and no Linux-delta register. The DVM-Sync-to-SCB path is
robust against every A72-side control.

## 4. Remaining levers (after keep-warm + clock + register dead ends)

The cure space reachable by config/registers/keep-warm is EXHAUSTED. What's left
is architectural, not a quick lever:
- **Avoid the teardown `tlbi` entirely** (the only thing that definitively kills
  the DVM Sync): an ASID-generation / lazy-TLB scheme so process teardown does
  NOT issue a per-unmap `tlbi vae1` (invalidate whole ASID on recycle instead).
  Big seL4 kernel change, correctness-critical. The one avenue that would work.
- **Accept + mitigate**: nodes=4 + masked shootdown + clock floor contain it to
  ~2.5%/teardown. Ship mitigations-only; keep the profiler as the monitor.
- DMA keep-warm (coherent external bus-master): Pi4 peripherals are non-coherent
  -> almost certainly dead; large effort to even set up.

1. **tlbi+dsb heartbeat** (Sync version): each heartbeat completes its own Sync so
   the teardown dsb only Syncs its own op; works iff onset >> pace (no self-stall).
   Risk: self-stalls if a >onset busy-no-tlbi gap precedes idle.
2. **core_freq 250->500** (VideoCore = AMBA fabric clock): flash-free (config.txt
   via fatswap). BUT the profiler's fixed-32.4s-TIMEOUT (not slow completion) argues
   it is quiescence, clock-independent -> likely won't help. mini-UART tradeoff
   (pin fixed; PL011/disable-bt or accept) -- the backlogged lever.
3. **Accept + mitigate**: nodes=4 + masked shootdown + clock floor contain it to
   ~2.5%/teardown. Commit the profiler, revert the heartbeat, document, move on.
4. **Root-cause (deep)**: disable the firmware RAM/fabric retention states that let
   the cluster ACE master quiesce (idle.S "retention follow-up"); under-specified --
   needs research into which config/firmware knob.

## Tools / state
- Board: 192.168.0.8 (DHCP bounces; ARP cache on the mac goes stale -- .197 is a
  DIFFERENT LAN device, NOT the board; trust serial / `arp -a | grep dc:a6:32`).
- `scripts/pi_flash.py [--build]` flashes; `scripts/netstall.py --idle 30` forces;
  `scripts/aios_nc.py` drives netconsole; serial monitor mirrors `/tmp/aios_serial.log`.
- RECOVERY: killing netstall mid-run can wedge netconsole (back-to-back-conn limit);
  TaskStop is cleaner than pkill. If wedged: serial reboot via
  `aios_console.py serial <dev> --login --cmd reboot` (stop the monitor first).
- `/proc/freezes` (pipe >=8000ms) + serial `[TLBISTALL]` (tlbi >=1000ms) = the meters.

## 5. BREAKTHROUGH (2026-06-20 LATE) -- ROOT CAUSE: a PHYSICAL DRAM region, not idle

After everything above was refuted, Bryan: "look for a cause in OUR code; more
diagnostics around the stall". Built an ENHANCED profiler -- per stall it now prints
core/dur/**gap**/**bpos**/bms/asid/va/**PA**/PTE/ipi/**band/hbS/lbS**/qgap. Impl:
machine.h hook -> errata.c `aios_tlbi_profile`; vspace.c `unmapPage` stashes the page
paddr+pte; tlb.h counts real remote-IPI tlbis.

RESULTS (Pi build 2669, netstall --idle 30):
- **gap=0 on EVERY stall** => MID-BURST; the first-post-idle tlbi is FAST. The
  idle-quiescence theory (which drove fabwarm/beacon/heartbeat) is WRONG.
- the stalling VA is INVARIANT 0x10006000-0x10011000 (PIPE_MMAP_ANON musl pages)
  while bpos varies 65..8600 => follows the PAGE, not the burst position. (Bryan's
  "stalls around /proc" clue fit: /proc-reading `cat`s are the frequent teardowns.)
- the **PHYSICAL addr is ALWAYS 0xf8xxxxxx-0xfbxxxxxx** (10+ samples) = the TOP 64MB
  of usable DRAM [0xf8000000, 0xfc000000), just below the 0xfc000000 peripheral
  window. PTE = normal cacheable inner-shareable (AttrIndx=4) => it is the PADDR,
  not the page type. Low-DRAM pages mostly do NOT stall.

MECHANISM (our code): `overlay-rpi4-4gb.dts` memory@0 range 2 (base 0x40000000 size
0xbc000000 -> end 0xfc000000) exposes the band as usable DRAM; the LIFO root-task
allocator (libsel4allocman split.c head-insert; bootinfo low->high) allocates the
HIGHEST untyped FIRST, so the heavy PIPE_MMAP_ANON pool lands in the band. WHY the
band is fatal (hypothesis, NOT yet pinned): it routes through a path that quiesces
after idle -- VideoCore firmware at top-of-RAM, or the memory-controller/peripheral
boundary. NOT gpu_mem (the DTS puts GPU/fb LOW at 0x3a000000-0x40000000). EXPLAINS
why every prior fix failed (none touched that band's path).

STAGED FIX (NOT yet implemented/tested): trim DTS range-2 size 0xbc000000 ->
0xb8000000 (end DRAM at 0xf8000000). Anon-mmap falls to low DRAM -> no stall. V3D's
8MB pool moves below 0xf8000000 but the GPU MMU maps any DRAM. Costs 64MB.

DO FIRST NEXT SESSION:
1. CONFIRM with ONE serial monitor (a DOUBLE monitor garbled this session's band
   lines -- `pgrep -fl "aios_console.py monitor"`, kill dups): netstall --idle 30
   --trials 10; grep serial `hbS=.. lbS=..`; lbS (low-band stalls) must stay 0 while
   hbS climbs + lbT large -> root cause LOCKED.
2. FIX: trim the DTS, full QEMU gate, pi_flash --build, netstall -> expect GONE.
3. If NOT cured -> instrument WHY the band stalls (GPU-activity A/B: does a V3D
   render loop prevent it? or read the BCM2711 memory-controller / VideoCore
   top-of-RAM layout).

SEPARATE perf bug (NOT the stall cause): ipi climbs ~thousands = the masked shootdown
broadcasting to boot-era residency {0,1,2,3} asids; fix by clearing residency on
pinning. Diagnostics all UNCOMMITTED (sibling seL4 machine.h/errata.c/vspace.c/tlb.h;
idle.S busy-yield; crt0.S bit-6/8 gated-off). Pi: build 2669 + core_freq=250.
