# NEXT (seed) 2026-06-19 -- Candidate 2: the AIOS-vs-Linux teardown/coherency diff (MECHANISM SOLVED)

The RPi4 ~33s idle-teardown freeze. Candidate 1 (SMPEN on cores 1-3) is REFUTED (all 4 cores SMPEN=1
-- docs/NEXT_20260619_smpen_secondary_FINDINGS.md). This seed captures the candidate-2 research
(3 agents: AIOS SoC-config map + Linux teardown/idle + BCM2711 fabric/clock registers), which **solved
the mechanism** and produced a concrete, cheap, flash-free next A/B. Memory: [[project_stall_hunt]].

## THE MECHANISM (high confidence -- A72 r0p3 TRM ARM 100095_0003_06, verbatim)

The hang is the **`dsb` after the teardown `tlbi`**, not the tlbi:
1. `tlbi <any> ; dsb` on the A72 **emits a DVM Sync** onto the ACE/CHI master interface -- TRM L2ACTLR[11]:
   *"if a TLB maintenance operation ... occurs after the previous DSB then a DVM Sync message is generated
   **regardless of the setting of this bit**."* (So broadcast `vae1is` vs local `vae1` cannot change it ->
   why the broadcast A/B was correctly REFUTED. So is `dsb nsh` -- already refuted build 2122 D2.)
2. The DVM Sync completion is **gated on the SoC fabric** -- TRM §7.7.4: *"BRESP ... must not be asserted to
   the core until all AXI masters that might have initiated the DVM synchronization request observe the
   transaction."*
3. **The SoC (not the core) idles the snoop path** -- TRM §2.4: when all outstanding snoop requests
   complete, the SoC asserts **`ACINACTM`** (an INPUT to the A72) to idle the AXI master snoop interface.
   Software on the A72 cannot deassert it. When the fabric is quiesced/clock-starved, BRESP for the
   DVM-completing transaction is withheld and the `dsb` spins until a SoC-level timeout force-completes it
   (the observed ~33s).

On BCM2711 there is **no L3 / no CCI / no system coherent interconnect**; the A72 cluster's ACE master
connects to Broadcom's proprietary **SCB / 128-bit AMBA fabric** to reach DRAM + DMA masters. That bridge
is what quiesces after idle. This is WHY every A72-ISA lever was refuted: broadcast scope (instruction
IS-bit) and A72-internal L2 clock (L2ACTLR[27]) cannot matter -- the stall lives in the **external-fabric
DVM-Complete path** triggered by the teardown DSB.

**Linux is immune because of TRAFFIC, not config:** Linux uses `dsb ish` on every teardown too (it does NOT
avoid DVM). But Linux idles with plain `WFI` (BCM2711 DT = spin-table, NO cpu-idle-states/PSCI), never
deep-idles the cluster, and its constant **timer tick + scheduler + DMA (GENET/USB/VideoCore/PCIe)** keep
the SCB fabric + snoop path warm, so the first post-idle DVM Sync always hits the warm path. AIOS's no-WFI
busy-`yield` idle keeps CPUs executing but makes **no bus transactions**, so the fabric still quiesces.

## TWO CORRECTIONS this research produced (update the mental model)
- **The "0x80000 @ ~16.2 kHz = 32.4s fabric timeout" is ARITHMETICALLY REFUTED.** The BCM2711 PM watchdog
  clock is 65536 Hz (`bcm2835_wdt.c`: SECS_TO_WDOG_TICKS = x<<16), so 0x80000 = exactly **8.0s**, and the
  20-bit max 0xFFFFF = 16.0s (matches the documented 16s watchdog max). The ÷4 to reach 16.2 kHz was
  reverse-fit. Whatever sets the ~33s, it is **not** a documented BCM2711 PM/CPRMAN/GISB timeout register.
  (Independently re-confirms [[project_stall_ubus_deadend]].)
- **No "fabric clock-enable / disable-clock-gating / fabric-timeout" register exists on BCM2711.** The VPU
  clock that drives the bus has NO enable bit by design (`clk-bcm2835.c`: *"VPU clock ... doesn't have an
  enable bit, since it drives the bus for everything else"*). No PM power-domain is the A72 cluster/fabric.
  No GISB arbiter on 2711 (`# CONFIG_BRCMSTB_GISB_ARB is not set`; the 2711 `scb` node is a bare
  simple-bus). The "poke a Broadcom fabric register" avenue is **CLOSED**.

## RANKED next experiments (do in order; all reuse pingmon + netstall + /proc/freezes)

### #1 -- RAISE THE FABRIC CLOCK: `core_freq` 250 -> 500 (CHEAP, FLASH-FREE, DECISIVE -- DO FIRST)
The VideoCore "core" clock **IS** the 128-bit AMBA system fabric the A72<->SoC DVM traffic crosses
(SkatterBencher; "core_freq drives the L2 cache and memory bus"). **AIOS currently hard-pins it LOW:**
`scripts/mksdcard.py` writes `core_freq=250` + `core_freq_min=250` + `enable_uart=1` (pinned for mini-UART
baud stability). So the fabric runs at a constant 250 MHz vs the Pi4 default 500. Hypothesis: a cold 250 MHz
fabric is too slow to complete the first post-idle DVM Sync before the SoC timeout; 500 MHz (stock) warms it.
- **TEST (flash-free):** `fatswap --read config.txt` -> set `core_freq=500` + `core_freq_min=500` (leave
  arm_freq/arm_freq_min) -> `fatswap /tmp/new config.txt` -> reboot -> soak (pingmon + `netstall --idle 30
  --trials 60` + read `/proc/freezes` before/after). Revert is a flash-free fatswap back.
- **CAVEAT (mini-UART):** core_freq drives the mini-UART baud; at 500 the SERIAL console garbles (AIOS's
  divisor assumes 250). Drive the test over NETCONSOLE (clock-independent) + /proc/freezes; serial garble is
  cosmetic. The firmware early boot is fine (500 is stock). Do NOT need force_turbo (avoid over-volt).
- **HONEST NUANCE:** AIOS already PINS the fabric clock (just at 250, no idle-downclock), yet still freezes.
  So this tests the fabric-clock SPEED axis (250 too slow vs 500 ok), NOT pin-vs-downclock. If 500 cures/
  reduces -> clock speed is the lever (and a real fix: pin core_freq=500, solve the mini-UART via
  disable-bt/PL011 or accept it). If 500 does NOT help -> the snoop interface quiesces from lack of TRAFFIC
  independent of the clock pin -> go to #2.
- Confidence: MED. Cheap + decisive enough to run first.

### #2 -- KEEP THE FABRIC WARM WITH LIGHT UNCACHED TRAFFIC (the traffic axis; kernel change)
If #1 fails, the snoop/fabric quiesces from no bus traffic (ACINACTM), clock-independent. Linux stays warm
via timer/DMA traffic. The refuted `corewarm` used HEAVY CACHEABLE 256KB RMW on 3 cores -> DVM CONTENTION ->
WORSE. The UNTESTED variant: a **light periodic UNCACHED / Device-memory READ** (an MMIO register read
traverses the SCB fabric to keep its clock/bridge active but generates NO cacheable coherency/DVM traffic,
so no contention). Inject one such read every ~10-20 ms from a low-priority background context (or the idle
path). Minimal sufficient interval < the fabric quiesce-onset time (instrument with #4 to find it).
- Also test: keep the per-core arch-timer tick live on idle cores (if AIOS idle cores are currently
  tickless) -- the GIC/timer path touches the fabric, the cheapest Linux-parity traffic.
- Confidence: MED-HIGH on the mechanism; the trick is "right kind of traffic" (uncached, light) vs the
  refuted heavy-cacheable corewarm.

### #4 (do alongside #1/#2 as the INSTRUMENT) -- arm AXI_QUIET_TIME to see bus-quiesce-onset vs stall-onset
**ARM_LOCAL + 0x30** (base 0x4c0000000 / 0xff800000): bit[20]=AXI_QUIET_IRQ_ENB, bits[19:0]=load (x16
AXI/APB cycles); a 24-bit timer reloaded to 16*N+15 while AXI txns are outstanding, counting down when the
**ARM->VideoCore AXI link is idle**; IRQ status = IRQ_SOURCE0 bit10 (Core-0 only). Arm its Core-0 IRQ to
timestamp exactly WHEN the bus goes quiet vs when the stall begins -- directly confirms/refutes the
fabric-quiesce premise and tells you the minimal keep-warm interval for #2. (BCM2711 ARM Peripherals
datasheet Table 110 p.95.) Pairs with ARM_CONTROL.AXIERRIRQ_EN (ARM_LOCAL+0x00 bit[6], Table 105 p.92) to
catch any AXI error during the stall. These are DETECTORS, not cures.

### Ruled out / deprioritized
- Any "fabric clock-enable/timeout" register: does NOT exist on BCM2711 (see corrections above).
- `dsb nsh` teardown barrier: already REFUTED (build 2122 D2, same 32399ms as dsb sy -- the TRM "regardless"
  clause bites).
- L2ACTLR[8] DISABLE_DVM_CMO_BROADCAST: LOW (debug-only; the [11] "regardless" clause means the DVM Sync
  still fires; likely breaks coherency, not skips the sync). One-MSR A/B only if bored.
- SMPEN cores 1-3: DONE + REFUTED (all =1).

## TOOLS / measurement
- `/proc/freezes` (NEW v0.4.265): passive counter -- `freezes / worst_ms / total_ms / last_ms / msgs /
  threshold_ms(8000)`. Read before/after each soak; this is the real-rate denominator.
- pingmon (`python3 -u /tmp/pingmon.py`, GAP>4s) + `netstall.py --idle 30 --trials 60` = the gold A/B.
- config.txt flash-free: `fatswap --read config.txt` / `fatswap /tmp/new config.txt` (crash-safe,
  sha-verified -- [[project_fatswap]]); `scripts/fatconfig_hw.py` automates it.

## Source anchors
A72 TRM 100095_0003_06: L2ACTLR[11]/[8]/[27], §2.4 ACINACTM/L2-WFI, §7.7.4 DVM/BRESP, Table 4-79 SMPEN.
Linux `arch/arm64/include/asm/tlbflush.h` (aside1is/vae1is + dsb ish; MAX_DVM_OPS=PTRS_PER_PTE batch->
vmalle1is). `clk-bcm2835.c` (VPU bus clock, no enable bit). `bcm2835_wdt.c` (65536 Hz). BCM2711 ARM
Peripherals datasheet ARM_LOCAL 0x00/0x30. config.txt defaults core_freq=500/core_freq_min=200
(AIOS overrides both to 250). Fabric-clock-floor precedent: linux commit c9107dd0b851 (sdhci-iproc, BCM2711
fabric hangs on pathological core/bus clock ratio -> software clock floor needed).
