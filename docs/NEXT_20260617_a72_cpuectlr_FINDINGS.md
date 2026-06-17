# A72 CPUECTLR stall hunt -- SESSION FINDINGS (2026-06-17)

Working log for the idle-teardown TLBI/DVM stall session. Seed: `docs/NEXT_20260617_a72_cpuectlr_stall.md`. Memory: `[[project_stall_hunt]]`.

## 1. PROBE RESULT -- the register-config hypothesis is DISPROVEN (HW ground truth)

Added a fault-survivable A72 IMP-DEF register probe to the kernel boot path
(`deps/kernel/src/arch/arm/machine/errata.c` `aios_a72_probe()`, called from
`boot.c` after "Bootstrapping kernel"; reads under a temporary EL1 vector table
so a trapped MRS reports TRAPPED instead of halting). Flashed to the real Pi4
(v0.4.261, build 2600, sha 2cb2a021). Serial (`/tmp/aios_serial.log`) printed:

```
[A72PROBE] MIDR=410fd083                              -> Cortex-A72 r0p3 (part 0xD08)
[A72PROBE] CPUECTLR_EL1=40 SMPEN=1 RET=0              -> SMPEN ON, dynamic-retention[2:0] OFF
[A72PROBE] CPUACTLR_EL1=0 FORCECLK30=0 DSBSY53=0      -> reset; no force-clock, no dsb-as-sy, no errata bits
[A72PROBE] L2CTLR_EL1=3002422                         -> 4 cores + armstub latency (nominal)
[A72PROBE] L2ACTLR_EL1=10 DVMDIS8=0 DSBNODVM11=0      -> reset; DVM/CMO broadcast ENABLED, DSB-DVM-sync ON
```

**Every A72 control register is nominal.** SMPEN on, retention OFF (safe), DVM
broadcast ENABLED, no anomalous CPUACTLR/L2ACTLR bits. The armstub writes
exactly `CPUECTLR=0x40` (SMPEN only) + `ACTLR_EL3=0x73` (un-traps EL1 IMP-DEF
access) + `L2CTLR` latency; CPUACTLR/L2ACTLR are left at reset.

**Conclusion: there is NO misconfigured register and NO Linux-vs-AIOS register
delta.** Research (2 agents, primary sources) independently confirmed: Linux's
arm64 `__cpu_setup` writes NOTHING IMP-DEF and relies on the SAME armstub SMPEN;
the RPi armstub already sets SMPEN per-core + un-traps EL1 access. So the seed's
prime suspect (an "A72 IMP-DEF config gap" Linux fills and AIOS doesn't) is
**refuted** -- the gap is RUNTIME behavior, not boot config.

Ruled out by the probe specifically: SMPEN-off (it's on), dynamic retention
armed (it's off), L2ACTLR DVM-broadcast-disabled (it's enabled), CPUACTLR
treat-DSB-as-SY (off). These were the candidate "left at reset" anomalies.

## 2. PIVOT -- the runtime difference: TLBI variant (local vae1 vs broadcast vae1is)

The one concrete Linux-vs-AIOS RUNTIME difference in the teardown path: Linux
SMP uses the inner-shareable BROADCAST `tlbi vae1is`; AIOS uses local `tlbi vae1`
(it pins all threads to core 0, so local suffices for correctness). The residual
stall is the FIRST local `tlbi vae1`+dsb on core 0 after idle hanging to the
32.4s BCM2711 UBUS timeout. Hypothesis: the local-only completion path cold-gates
when the cluster idles, while the DVM/inner-shareable path (constantly exercised
by 4-core coherency) stays warm -- which is why Linux (broadcasting) never hangs.

### Candidate C (testing now): `tlbi vae1` -> `tlbi vae1is` in the teardown
`deps/kernel/include/arch/arm/arch/64/mode/machine.h` `invalidateLocalTLB_VAASID`,
gated behind `#define AIOS_TLBI_BROADCAST`. Minimal isolation: only the tlbi
variant changes; the trailing `dsb sy` (>= ish) still waits for the DVM
completion. Correctness-safe (vae1is invalidates a superset of vae1; the
masked-shootdown software IPI is skipped under core-0 pinning anyway).
Probe confirmed the DVM path is enabled (L2ACTLR[8]=0) + SMPEN=1, so the
broadcast has a working transport.

- Staged kernel: `disk/kernel8_v261_candidateC_vae1is.img` (sha a4682457).
- Good rollback: `disk/kernel8_v261_b2596_good.img` (the v261 b2596 baseline).
- Test: QEMU gate (smp/shmring/socket/netd) -> flash -> `netstall.py --trials 30 --idle 8`. Goal 0/30.
- RISK: could WORSEN it -- if the DVM-to-idle-cores path is itself the slow one
  (the corewarm A/B hinted DVM contention worsens the stall), every tlbi now
  broadcasts to cores 1-3. The A/B decides.

### Candidate B (research-FAVORED; the documented mechanism + its exact control bit)
Research (TRM 100095 r0p3 §2.4 + §4.3.65, primary-sourced) PINNED the mechanism:
the A72 **L2 control logic clock-gates after 256 consecutive idle cycles** and the
shared-cluster clock gating is controlled by **L2ACTLR_EL1[27] "Force L2 logic
clock enable active"** (=1 -> "Prevents the clock generator from stopping the L2
logic clock"). This is the REGISTER ENCODING of the proven nodes=4 busy-spin cure
(traffic keeps the L2 inside the gate window; [27] just holds the clock on).
Companion bits: L2ACTLR[28] (tag-bank clock), L2ACTLR[26] (L2/GIC/Timer RCG, no
retention precondition). Retention preconditions are SATISFIED on the Pi
(CPUECTLR[2:0]=0, no Q-channel), so setting [27]/[28] is legal.

**CRITICAL CONSTRAINT: L2ACTLR_EL1 must be written BEFORE the MMU is enabled**
("set statically ... only when L2 idle ... before MMU enable, before any
ACE/CHI/ACP traffic"). The kernel's `arm_errata` runs POST-MMU (head.S enables
MMU first) -> WRONG place. It must go in the **elfloader pre-MMU** (crt0.S _start
or the top of arm_enable_mmu, on the primary core; L2ACTLR is cluster-wide = one
write suffices). EL2 access is un-trapped (armstub ACTLR_EL3 bit6=L2ACTLR set;
probe read L2ACTLR=0x10 OK). Brick risk LOW if pre-MMU (clock-enable hold, no
data corruption); writing it LIVE post-MMU risks a transient mis-clock -> avoid.
Neither Linux nor ATF set it (define the macros, never write them) -> this is a
NEW mitigation, but it's the documented mechanism's knob and the seed explicitly
counts "a cluster field that cures it" as the breakthrough.

NOTE the research caveat: the TRM says the 256-cycle L2 wake is only a 4-cycle
penalty normally -- so the 32.4s hang is the BCM2711 fabric integration making
that wake-up handshake stick, not the gating per se. L2ACTLR[27] cures by never
gating (no wake-up needed). This also predicts candidate C (tlbi variant) is LESS
likely to help: the L2-logic gate affects ALL L2 ops, local or broadcast.

## BLOCKER discovered: the stall is NOT reproducing this session
First baseline soak on the probe kernel (= vae1, the stalling behavior):
**0/30 STALLED, all clean ~1.0s.** At the memory's ~10% rate, 0/30 is only ~4%
likely -> either a lucky-clean run OR the current board state (21h uptime, warm,
recently active) is keeping the fabric warm enough to suppress it. Running a
bigger baseline (40 trials) + escalating idle to confirm the stall reproduces AT
ALL before any A/B -- without a reproducing baseline, neither B nor C can be
empirically validated (a fix showing 0/30 proves nothing if baseline is also 0).

## 3. REPRODUCTION cracked: the stall is IDLE-DURATION-dependent
The stall did NOT reproduce at `--idle 8` (0/70 this session) but DOES at longer
idle -- the cold-fabric trigger needs more than 8s of cooling in the current
board state (warm, recently active). Confirmed on the probe kernel (= stock vae1):
- `--idle 8`  : 0/70  (two runs, 0/30 + 0/40)
- `--idle 30` : **1/12 STALLED** -- trial 3 = 63.5s total / **33.5s residual**
  (= exactly 3 x 10.8s, the TLBI/DVM quantum). REAL stall reproduced.
- `--idle 45` : establishing baseline rate now.
So the A/B must use `--idle 30`+ (longer = higher rate, more A/B power). `--idle 8`
is too weak a trigger in the current board state.

## 4. A/B in progress: baseline (vae1) vs candidate B (L2ACTLR[27])
Candidate B gate: **smp 7/7** (boot-safe; B is MIDR-inert on a53, so QEMU only
validates boot integrity -- the real write runs on A72/HW, where the boot probe
re-reads L2ACTLR and should show 0x10|0x0C000000 = **0xC000010** confirming it
took effect). Staged: `disk/kernel8_v261_candidateB_l2clk.img` (b47eb898).
Plan: solid baseline at `--idle 45` -> flash B -> same soak -> compare. Goal:
baseline stalls, B clean. Then a large confirm soak.

## Kernels staged (disk/)
- `kernel8_v261_b2596_good.img` (4b7fd48e) -- ROLLBACK (stock v261 b2596)
- `kernel8_v261_candidateB_l2clk.img` (b47eb898) -- candidate B (elfloader L2ACTLR[27]) + probe
- `kernel8_v261_candidateC_vae1is.img` (a4682457) -- candidate C (tlbi vae1is) + probe [deprioritized]
- currently on Pi: the PROBE kernel (2cb2a021 = stock vae1 + probe) = the baseline

## 5. Candidate B HW-VALIDATED: boots + the write took effect (build 2604)
Flashed `disk/kernel8_v261_candidateB_l2clk.img` (b47eb898). pi_flash banner OK
(v0.4.261 build 2604, cortex-a72 4-core). **The boot probe confirms the L2ACTLR
write landed on real A72:**
```
[A72PROBE] L2ACTLR_EL1=c000010 DVMDIS8=0 DSBNODVM11=0   <- was 10; now [27]+[26] set
```
(0x10 | 0x0C000000 = 0xC000010). Every other reg unchanged. NO brick; the elfloader
pre-MMU L2ACTLR write is safe on the real silicon. Soaking now.

## 6. The A/B-power problem (honest caveat)
The stall is BARELY reproducing this session: baseline = **0/70 at idle8, 1/36
(~3%) at idle30** (the 1/12 was a lucky hit; a clean 24-trial idle30 run gave
0/24). The memory's ~8-12% was a different board state. At ~3%, a definitive
baseline-vs-B A/B needs ~100+ trials/arm (50+ min/arm at 30s) -- impractical, and
even then the baseline rate is unstable. So this session CANNOT empirically PROVE
B cures the stall. What it CAN deliver: (1) the register hypothesis is disproven
(probe), (2) the mechanism is the documented L2 256-idle-cycle gate (research),
(3) B is the exact control bit for it (the register form of the proven nodes=4
cure), HW-validated to boot + apply + not regress + soak clean. Empirical cure
confirmation awaits a session where the baseline reproduces reliably (~8-10%),
then re-run B. netstall is netconsole-bound: reliable only at idle <= ~30s
(idle45 = conn-deaths + corrupt measurement).

## Status
- [x] Probe (register hypothesis disproven; all A72 regs nominal)
- [x] Research (mechanism pinned: L2 256-idle-cycle gate; control = L2ACTLR[27])
- [x] Reproduction cracked (idle-duration dependent; --idle 30 reproduces, rare ~3%)
- [x] Candidate B implemented + gated (smp 7/7) + HW-validated (boots, L2ACTLR=c000010)
- [x] Candidate B short soaks looked clean (0/30, then 0-measured-stalls/120) -- but the stall was RARE
- [x] **CANDIDATE B REFUTED (ping-monitored soak):** a longer soak WITH a concurrent ICMP ping monitor
      (/tmp/pingmon.py -- the new GOLD freeze detector) caught a REAL freeze on B: netstall --idle 30
      trial 57 = 33.5s stall COINCIDING with ping GAP = 33.2s system-unreachable (SAME event). 1 freeze/60
      (~1.7%, no better than stock ~2.8%). **=> L2ACTLR[27] does NOT prevent the freeze; the L2-256-cycle
      clock-gate is NOT the (sole) cause. Mechanism is DEEPER (SCU/fabric/BCM2711-UBUS).**
- [x] Captured: deps/patches/seL4-kernel.patch (probe+C-gate) + seL4_tools.patch (B); memory; HANDOVER

## 7. The real breakthrough this session = the METHOD + the negative results
Two hypotheses DEFINITIVELY ruled out on real HW: (a) the A72 register-config gap (probe: all regs
nominal), (b) the L2-logic clock-gate (B: L2ACTLR[27] active, freeze still happened, ping-confirmed). The
durable WIN is the **ping-monitor freeze detector**: run `python3 /tmp/pingmon.py` (continuous ICMP ping,
prints GAP>4s) concurrently with `netstall.py --idle 30` -- a ping GAP coinciding with a netstall stall =
a REAL whole-system freeze; a netstall death with NO ping gap = netconsole-only. This ends the
freeze-vs-netconsole-death ambiguity that muddied every prior measurement. EVERY future A/B must use it.

## NEXT SESSION (B is refuted; the path is now clear + cleanly A/B-able)
The METHOD is solved: `python3 /tmp/pingmon.py` + `netstall.py --host 192.168.0.8 --idle 30 --trials N`,
look for a ping GAP coinciding with a netstall stall = real freeze. Baseline stock reproduces ~1.7-2.8%
at --idle 30 (rare -- use trials>=60 and the ping monitor). Candidates, in order:
1. **Force the CORE clocks too** (B only forced the L2 logic clock; the core/SCU side may be what
   quiesces): add CPUACTLR_EL1[63] (mem-system RCG) + [30] (main clock) + L2ACTLR_EL1[28] (tag-bank clk)
   to the SAME pre-MMU elfloader crt0.S block as B (CPUACTLR is per-core so also add to secondary_startup;
   pre-MMU avoids the post-MMU "off-label" CPUACTLR-write rule). A/B with the ping monitor.
2. **Bound the BCM2711 SCB/ARM-cluster UBUS-timeout register** (default 0x80000 = 32.4s; DIFFERENT from
   the PCIe RC's 0x40a8 already bounded in pcie_brcmstb.c) -- turns the freeze into a blip regardless of
   cause. Reg is undocumented; mine Linux brcmstb / U-Boot / the BCM2711 TRM (BACKLOG #2).
3. **Re-examine whether nodes=4 truly helps** with the ping monitor (it may also be only partial).
4. DECISION (Bryan 2026-06-17): B is ADOPTED as the new working baseline (kept, NOT reverted -- it costs
   only power and MAY partially help; the stock-vs-B difference is unproven, samples too small + measured
   in different board-state windows, so "B no better than stock" was an overstatement). Improvements now
   build ON B and A/B vs B; disk/kernel8_v261_b2596_good.img kept for a final full-stack-vs-old-stock
   confirmation. Remove/gate the boot probe + C-gate once a real fix lands.

## CANDIDATE B+ : REFUTED + HARMFUL (the A72-clock hypothesis is DEAD)
B+ = B's L2ACTLR[27]+[26] PLUS L2ACTLR[28] (tag-bank) + CPUACTLR_EL1[63] (mem-system RCG) + [30] (main
clock) on core 0 (gated `AIOS_CORE_CLOCK_FORCE`, now DISABLED). HW build 2606: boots, boot probe confirmed
the writes took (CPUACTLR=0x8000000040000000, L2ACTLR=0x1c000010). **Ping-monitored soak (--idle 30): 2
ping-confirmed freezes / 72 -- one was a CATASTROPHIC 321.7s freeze** (GAP #2, coinciding netstall trial
71 = 338s; GAP #1 = 32.7s @ trial 27). So forcing ALL the cluster clocks active did NOT cure the stall
AND made a freeze ~10x WORSE.

**FREEZE COMPARISON (all ping-confirmed):**
- stock (b2596): 1/36, worst 33.5s
- B (L2 clocks):  1/60, worst 33.2s
- B+ (all clocks): 2/72, worst **321.7s** (worse)

**CONCLUSION: the A72 clock-gating hypothesis is RULED OUT.** Three A72-register hypotheses now dead on HW:
(1) register-config gap (probe), (2) L2-logic clock (B), (3) full-cluster clock (B+, even harmful). The
stall is NOT controllable via any A72 register -- it is a BCM2711 SoC-fabric/UBUS phenomenon (the SoC
interconnect outside the A72 cluster). nodes=4 busy-spin "helps" by generating SoC TRAFFIC (not by clock
state -- B+ forced the clocks and still froze), so the mechanism is fabric QUIESCENCE/traffic, not clocks.
Pi REVERTED to baseline B (B+ harmful); AIOS_CORE_CLOCK_FORCE disabled in crt0.S (gated-off for record).

## THERMAL / DVFS investigated (no-flash, via /proc/cpufreq + /proc/temp)
- **Thermal throttling RULED OUT**: /proc/temp = 54-63C, throttle = 85C. The firmware is not throttling.
- **AIOS has a DVFS governor (cpu_gov.c)** that scales the ARM clock 300<->600 by load (firmware VC
  mailbox SET_CLOCK_RATE -- NOT an A72 reg, which is the layer B/B+ couldn't touch). It downclocks to 300
  only when the WORK-SERVERS are idle (busy<80permille for 3 ticks). **But it held 600 ALL session**
  (gov_dbg sets=1, bmin~155-163 -- a held netconsole connection reads as "work"), so every soak I ran
  (stock/B/B+) was at 600 MHz; the idle-downclock was NOT a factor in the measured freezes.
- **Clock frequency affects freeze SEVERITY, not occurrence** (no-flash A/B via /proc/cpufreq.set):
  pinned 300 MHz -> 1 ping-confirmed freeze of **164.7s** (~5 x the 32.4s UBUS quantum) vs ~33s (1 quantum)
  at 600 MHz. So a freeze that strikes during a real *disconnected*-idle period (when the governor IS at
  300) would be ~5x LONGER. (Pinned-300-under-load also breaks netconsole -- an artifact of sustained low
  clock the governor would never allow; the ping monitor stayed the truth.)
- **IMPLICATION**: raising the DVFS floor / not downclocking to 300 at idle (a cpu_gov / config knob, no
  kernel hack) is a freeze-SEVERITY mitigation (33s vs 164s in real idle) -- NOT a cure (freezes still
  happen at 600). The freeze being N x 32.4s quanta (1 @ 600, ~5 @ 300) reinforces that the core issue is
  the UBUS-timeout recovery (below), with clock modulating how many quanta elapse.

## THE ONLY REMAINING LEVER (for a CURE): the BCM2711 SCB/ARM-cluster UBUS-timeout register (BACKLOG #2)
The 32.4s == 0x80000 ticks @ 16.2kHz = a UBUS-timeout. Bound it (like v0.4.213 bounded the PCIe RC's copy
@0x40a8) and the freeze becomes a blip regardless of the (uncontrollable) fabric quiescence. The reg is
UNDOCUMENTED + DIFFERENT from the PCIe one -- in ARM-local/SCB space (~0xff8xxxxx or 0xfdxxxxxx). NEXT
SESSION: mine Linux's brcmstb fabric/bus driver + U-Boot + the BCM2711 TRM for a reg defaulting to 0x80000
in the ARM-cluster/SCB MMIO range (NOT the PCIe block). An MMIO scan is risky (undocumented reads can
hang). This is a deeper RE task -- the A72-register avenue is exhausted. METHOD for any test: the ping
monitor (/tmp/pingmon.py) + netstall --idle 30 (gold freeze detector).
RULED OUT (do not re-try): A72 register-config gap (probe), L2-logic clock force (B), candidate C is
unlikely (same L2-path reasoning), tlbi_probe, idle-core quiescence, dsb-scope.
