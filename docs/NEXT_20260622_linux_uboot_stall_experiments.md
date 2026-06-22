# Stall experiments via bare-metal / U-Boot / Linux (2026-06-22)

The RPi4 ~32.4s idle->wake fabric freeze is a MAJOR OPEN CONCERN (never "solved" --
[[feedback_stall_open_concern]]). The cure is exhausted from *inside* AIOS (every A72-side
keep-warm refuted; no software-writable fabric-timeout register; coresched wedges; BKL removal
infeasible). These experiments step OUTSIDE AIOS to use U-Boot and Linux as reference oracles.

## The one question these resolve

We have a real contradiction. Linux on this exact Pi4 NEVER freezes, yet RPi4 Linux idles *deeply*
(tickless NO_HZ_IDLE + WFI) -- so "Linux just stays busy" is NOT the explanation. And every keep-warm
we ran was on core 1 or an external DMA master -- NEVER core 0, which the instruction-level localization
says is the wedged path. So we genuinely do not know whether Linux's immunity is:

- **(A) the right traffic on the right core** -- Linux's normal operation keeps *core 0's own* ACE/snoop
  master port warm, and all our keep-warms missed it (wrong core / external master); or
- **(B) a fabric/power/clock configuration** Linux or the VPU firmware sets that AIOS does not, so the
  SCB fabric never quiesces (or wakes instantly).

A vs B is the difference between a real cure (replicate the traffic on core 0, or set the register) and
confirming it is unfixable silicon. No AIOS-internal experiment can tell them apart. These can.

Mechanism recap (settled): core 0 clocked-but-wedged on the FIRST fabric op after the BCM2711 SCB fabric
quiesces during idle; the A72 cluster's own ACE master port idles via `ACINACTM` (a CPU *input*; SoC-driven);
the 32.4s is a fixed, clock-independent silicon force-complete that *self-resolves* (not a classic abort).

---

## E1 -- bare-metal minimal reproducer  [STARTING -- highest value, lowest risk]

**Goal:** does the freeze reproduce with NO seL4 -- a minimal program with AIOS's exact bring-up but none
of its scheduler/servers/BKL? And which *operation* triggers it?

**Form:** a standalone `kernel8.img` (RPi firmware loads it to 0x80000 directly -- no U-Boot needed, and
no U-Boot console UART-poll to mask the quiesce). Reuse AIOS's PROVEN RPi4 bring-up verbatim (image header,
`CPUECTLR.SMPEN=1`, the `L2ACTLR` clock-force bits, the MMU/cache setup with DRAM = Normal WB-cacheable
inner-shareable, mini-UART) so the fabric path is byte-identical to the configuration that stalls. Run on
core 0 only (cores 1-3 parked, as in AIOS where all work pins to core 0).

**Method (per trial):**
1. Touch + then evict (`dc civac`) a target DRAM line so it is genuinely cold.
2. Read `CNTPCT_EL0` (sysreg -- 0 bus txns).
3. Busy-wait ~30s polling `CNTPCT` ONLY -- no memory/MMIO access -- so the SCB fabric quiesces.
4. Read `CNTPCT` (t0), do ONE test op, read `CNTPCT` (t1); print `(t1-t0)` ms over the mini-UART.
5. Repeat, cycling the **test op** to disambiguate the trigger:
   - cold cacheable DRAM **load** (the s8 "cold-load" hypothesis -- the TCB-restore `ldp`);
   - `tlbi vae1; dsb` (the DVM-completion path);
   - `dsb sy` alone;
   - uncached/Device load.

**What each result proves:**
- **Repros bare-metal (~32.4s on some op)** -> the freeze is SILICON+FIRMWARE, OS-independent, NOT anything
  seL4 does -> the cure axis is config/power (E2/E4), and we now have a tiny, single-core, instantly-
  resettable, flash-free-iterable testbed. The *which-op* result pins the exact trigger (load vs DVM) that
  AIOS's 4 mixed sites could not isolate.
- **Does NOT repro single-core** -> the freeze needs OS/SMP state (the multi-core coherency/DVM domain, or
  a specific MMU config). Follow-up variant: bring up a 2nd core (enable real DVM traffic) and retest; if
  *that* repros, it is the cluster ACE/DVM path, not a lone cold load.

**Faithfulness caveats:** copy AIOS's `SMPEN`/`L2ACTLR`/`MAIR`/`TCR`/`SCTLR` exactly (a mismatch could give
a false negative). Single-core is intentional (matches core-0 pinning) but is itself a variable -- hence the
2-core follow-up. Pace serial at core_freq=250 + enable_uart=1 (AIOS's mini-UART-stable config).

**Build/flash: DONE -- `experiments/e1_repro/`.** Standalone bare-metal AArch64 (boot.S + repro.c, links at
0x80000, "ARMd" header at 0x30, no relocator needed; mirrors AIOS's SMPEN + L2ACTLR[27:26] clock-force + MAIR
idx4=0xff Normal-WB + TCR inner-shareable + mini-UART). `./build.sh` -> kernel8.img (16KB, structural checks
pass: magic@0x30, l1_table 4KB-aligned, _start@0x80000). `python3 mkbootimg.py` -> `e1_boot.img` (Etcher-ready
FAT32 + firmware + config.txt[core_freq=250] + kernel8.img; built via the firmware-compatible newfs_msdos
path). Flash e1_boot.img to the SPARE SD (Etcher), sercap on the mini-UART, power on -- runs ~6-7 min
autonomously (4 ops x 3 trials x 30s) then hangs. Full procedure + interpretation: experiments/e1_repro/README.md.
Iterate flash-free by swapping just kernel8.img on the FAT.

---

## E2 -- U-Boot blind MMIO scan of 0xFD000000-0xFD5FFFFF

Our docs flag this SCB-region read-scan as NEVER done -- from AIOS an undecoded read = SError = wedge.
U-Boot's `md` is the right tool: read-only, and a fault just means power-cycle. Build U-Boot for rpi_4,
`md.l 0xFD000000 ...` across the region, log what decodes vs faults. Maps the fabric/QoS registers that
actually exist (the force-complete timeout may live here even though it is not the STB GISB `ARB_TIMER`).

## E3 -- Linux immunity bisect  [highest-information on the cure]

Boot stock RPi OS (immune), then progressively make it behave like AIOS until it freezes; the step that
triggers it IS the immunity factor. A ~40-line kernel module issues the suspect fabric op (cold load /
`tlbi;dsb`) after a forced full-system quiesce and times it via `CNTPCT`. Peel away immunity in order:
`idle=poll` -> `nohz_full=` + isolate core 0 -> kill Ethernet/USB DMA -> stop the cpuidle governor. First
config that freezes names the factor (and which traffic on which core grants immunity -> feeds E5).

## E4 -- Linux fabric/clock/power-domain register diff

From Linux: `clk_summary` + `pm_genpd_summary` (debugfs) + `devmem2` of ARM_LOCAL (0xFF800000) + a module
to read `CPUECTLR_EL1`/`L2CTLR`/`L2ACTLR`. Diff vs AIOS's dumps. We checked the A72 IMP-DEF regs but NOT the
power-domain/QoS state -- if Linux keeps a fabric power domain on that AIOS lets gate, that is the cure (and
AIOS can set it via the same VC mailbox).

## E5 -- core-0 timer-driven DRAM keep-warm  [AIOS-side, Linux-informed]

The one untried keep-warm: a CORE-0, timer-driven heartbeat (wake ~1kHz, coherent cache-miss DRAM read,
sleep -- never busy-loop, which hangs the shell). Every prior keep-warm was core-1/external; the localization
says core 0 is what matters. If E3 shows immunity = "core-0 coherent traffic," this is the direct cure.

---

## Logistics
- **Spare SD** for all bare-metal/U-Boot/Linux boots -- the AIOS SD stays intact (board recovered, healthy).
- One serial reader at a time (`scripts/sercap.py /tmp/x.log`); mini-UART at core_freq=250.
- Order: **E1 first** (silicon-vs-OS, binary, low-risk) -> if silicon, E2/E4 hunt the knob; if OS/SMP, E3.
- Keep results appended here per trial.

## Trial log

### E1 trial 1 -- single-core EL2 bare-metal (2026-06-22)
- **Setup:** experiments/e1_repro kernel8.img (dual magic 0x30+0x38) on the RPi-OS spare SD (config.txt +
  kernel8.img swapped; RPi OS backed up to *.rpios). Core 0 only (cores 1-3 WFE-parked), **EL2**, MMU+caches
  ON, SMPEN + L2ACTLR[27:26] clock-force, DRAM = Normal-WB inner-shareable. 30s quiesce, 3 trials/op.
- **Result: ALL 12 ops `dur=0ms` -- NO STALL** (coldload / tlbi+dsb / dsb / devload x3). The 12 lines are
  EXACTLY 30s apart -> the quiesce is genuinely 30s (cntfrq read = 54MHz), i.e. the fabric really did idle
  and the first post-idle op still returned instantly.
- **Caveat (cosmetic):** op *names* garbled + the two banner lines lost -> the image loaded at an address
  other than 0x80000 (the standard arm64 magic at 0x38 I added for robustness likely triggered Linux-style
  placement), so `opname[]`'s ABSOLUTE pointers are wrong. Everything that matters is PC-relative (code, MMU
  setup, test_buf, CNTPCT timing) -> the **durations are valid**. (Fix: drop the 0x38 magic, or make opname a
  2D char array.)
- **VERDICT: single-core EL2 bare-metal does NOT reproduce the freeze.** -> it is NOT a lone-core cold-DRAM-
  load / DVM op; it needs OS/SMP state. Strongly refocuses onto the **multi-core coherency/DVM domain** (fits
  ACINACTM = the cluster ACE/snoop master quiescing on *cluster* snoop traffic, which a single active core
  barely exercises). **NEXT (decisive): the 2-core variant** -- bring up core 1 so the cluster runs real
  inter-core snoop/DVM, quiesce, retest. Also worth: an EL1 variant (drop to EL1, `tlbi vae1`).

### E1 trial 2 -- 4-core EL2 (multi-core coherency domain) (2026-06-22)
- **Setup:** NCORES=4. Core 0 released cores 1-3 via the firmware spin-table (wrote secondary_entry to
  0xd8/e0/e8/f0). Each secondary: SMPEN + shared MMU + coherent RMW burst, then WFE. Confirmed
  **`secondaries up: 3/3 -> 4 cores in coherency domain`** (valid multi-core test). Firmware log:
  **`Loaded kernel8.img to 0x200000`** (the 0x38 standard magic triggers Linux-style placement -> explains
  trial 1's garbled opname; fixed via a 2D char array = PC-relative).
- **Result: ALL ops `dur=0ms` -- NO STALL** even with 4 cores in the coherency domain, idle, after 30s.
- **BUT the TLBI op was `tlbi alle2` = LOCAL** (no DVM broadcast -> the `dsb` has nothing to wait on from the
  fabric). Our model says the freeze is the **DVM/coherency-COMPLETION** path -- a *broadcast* TLBI's DVM-Sync
  waiting on the quiesced SCB -- NOT a data load. So coldload `0ms` is CONSISTENT (data does not exercise the
  DVM path), and the local TLBI **never tested the actual trigger.** A methodology bug, not a refutation.
- **NEXT (trial 3): inner-shareable BROADCAST TLBI** (`tlbi alle2is` + `tlbi vae2is` = the EL2 analog of
  AIOS's `tlbi vae1is`), which generate the DVM-Sync the `dsb` blocks on. If THAT hangs ~32.4s -> reproduced,
  and it is the DVM-completion path. If not -> the trigger needs EL1 / the seL4 regime, or a longer quiesce.

### E1 trial 3 -- 4-core EL2, BROADCAST inner-shareable TLBI (DVM-Sync) (2026-06-22)
- **Setup:** same 4-core EL2 (3/3 secondaries up confirmed), ops = coldload / `tlbi alle2is`+dsb /
  `tlbi vae2is`+dsb / dsb. The two broadcast IS TLBIs are the EL2 analog of AIOS's `tlbi vae1is`: they emit a
  DVM-Sync, and per the A72 TRM the `dsb` cannot retire until the SoC fabric returns DVM-Complete -- exactly
  the op AIOS's stall is attributed to.
- **Result: ALL 12 ops `dur=0ms` -- NO STALL**, including both broadcast DVM-Sync TLBIs, after a real 30s
  idle with 4 cores in the coherency domain.
- **VERDICT (important): minimal bare-metal does NOT reproduce the freeze -- even the exact broadcast
  DVM-Sync op, 4 cores, EL2, after 30s idle.** So the 32.4s freeze is **NOT a pure-silicon "any fabric/DVM op
  after idle hangs" property** -- if it were, this `tlbi alle2is; dsb sy` after 30s idle would hang too.
  Either (a) it needs the **seL4 EL1&0 environment** (the EL1 translation regime / the kernel-exit->re-entry
  exception context AIOS's localization pins it to -- my repro never leaves EL2, never does an EL0 excursion),
  or (b) **the SCB does not actually quiesce in minimal bare-metal** (something in the full AIOS/VideoCore
  runtime is needed to drive it into the quiesced state). This is in TENSION with the keep-warm refutations'
  "it's the A72 ACE port, silicon" conclusion -- my bare-metal IS the A72 doing exactly that op, and it does
  not hang -> the seL4 context matters more than "pure silicon" implied.
- **NEXT options:** (i) cheap disambiguation -- bump QUIESCE_S to 90-120s; if still 0ms, the SCB genuinely
  is not quiescing here (rules out a threshold). (ii) EL1+EL0 variant -- set up EL1, drop to EL0, re-enter via
  exception, then the fabric op (matches AIOS's idle->wake context); big complexity step. (iii) PIVOT to E3
  (Linux): run the same broadcast-tlbi-after-idle as a kernel module under Linux on this Pi -- the reference
  oracle. If Linux also does not hang this way, it confirms the trigger is the seL4 idle->wake path, not
  silicon.

### E1 trial 4 -- 4-core EL2, broadcast DVM-Sync, IDLE SWEEP (2026-06-22)
- **Setup:** 4-core EL2 (3/3 up), op = `tlbi alle2is; dsb sy`, swept idle = {30,60,120,240}s (up to 8x AIOS's
  30s threshold), all 4 cores idle.
- **Result: `dur=0ms` at EVERY idle duration, including 240s.** NOT a threshold -- the bare-metal SCB does not
  quiesce / does not exhibit the hang regardless of idle length.

### E1 CONCLUSION (trials 1-4)
Minimal bare-metal on this exact Pi (1-core and validated 4-core, EL2, SMPEN + L2ACTLR clock-force + Normal-WB
inner-shareable DRAM), doing a cold cacheable load AND the broadcast IS DVM-Sync (`tlbi alle2is/vae2is; dsb`),
after idle up to 240s, **does NOT reproduce the 32.4s freeze.** So the freeze is **NOT a context-free silicon
"any fabric/DVM op after idle hangs" property** -- it needs something the full AIOS/seL4 runtime provides.
Leading explanation: **the BCM2711 SCB only enters the quiesced state under a full VideoCore-firmware runtime**
(power management settled after long operation), not a freshly-booted minimal program. In TENSION with the
keep-warm refutations' "pure A72-ACE-port silicon" framing.
**=> PIVOT to E3 (Linux): the reference FULL runtime that is immune.** Same broadcast-tlbi-after-idle as a
kernel module under Raspberry Pi OS on this Pi (card has it, backed up). Hangs -> silicon/firmware both full
runtimes hit (reference reproducer to bisect). Does NOT hang -> Linux's activity keeps the SCB warm (its
immunity), seL4's idle lets it quiesce -> the cure is to keep the SCB warm the Linux way (re-opens keep-warm
with the RIGHT target).

## E3 RESULT (2026-06-22) -- Linux does NOT reproduce it either

Kernel module `experiments/e3_linux/e3_dvm_test.c` on RPi OS (kernel 6.18.34+rpt-rpi-v8): `stop_machine`
forces all 4 cores quiet (CNTVCT spin, no memory) for `idle_s`, then CPU 0 times `tlbi vmalle1is; dsb sy`
(the EL1 IS-broadcast DVM-Sync).
- **First run (idle_s=30) REBOOTED the Pi** -- but that was a RED HERRING: systemd arms the BCM2835 hw
  watchdog at **RuntimeWatchdogSec=1min**, and my 30s all-cores-IRQs-off freeze spanned the 60s deadline ->
  watchdog reset. A methodology artifact, NOT a DVM hang.
- Disabled it (drop-in `RuntimeWatchdogSec=0` + `daemon-reexec`). **Re-run: idle = 10 / 30 / 60s all give
  `dur=0ms` -- NO HANG.** Same as bare-metal, not a threshold.

### COMBINED CONCLUSION (E1 bare-metal + E3 Linux)
**NEITHER minimal bare-metal NOR full-runtime Linux reproduces the freeze by "force all cores idle + issue a
broadcast TLBI DVM-Sync."** On this exact silicon a deliberate DVM-Sync after a forced idle completes in 0ms
(Linux AND bare-metal). Therefore:
- The AIOS 32.4s stall is **NOT a context-free silicon "fabric/DVM op after idle hangs" property**, and
- this **CONTRADICTS the long-attributed mechanism** ("the teardown `tlbi;dsb` DVM-Sync hanging on the
  quiesced SCB") as a standalone trigger -- because issuing exactly that op after idle does NOT hang here.

### Leading remaining variable: WFI vs busy-spin idle
My `stop_machine` idle is a BUSY-SPIN (cores executing CNTVCT, no WFI). The A72/SoC asserts `ACINACTM`/
quiesces on `STANDBYWFI` (cores actually entering WFI), which a busy-spin never signals -> the SCB may never
quiesce in my test. AIOS reportedly uses no-WFI busy-yield idle yet stalls -- so either AIOS's idle path DOES
hit WFI somewhere (idle.S), or the trigger is the natural idle->wake EXCEPTION context (kernel-exit -> WFI/idle
-> IRQ wake -> first fabric op in the entry path), which a deliberately-issued op never reproduces.
**NEXT options:** (a) Linux natural-idle test -- a timer that fires after the system genuinely idles (cores
WFI), doing the `tlbi` in the wake/IRQ context (faithful to AIOS's idle->wake); (b) go BACK to AIOS with this
result -- a deliberate DVM-Sync after idle does NOT hang, so re-examine what the stall actually IS (maybe not
the DVM-Sync; re-check the [STAGECP]/PMU localization). Pi left with watchdog disabled + module in ~/e3.

### E3-wfi RESULT (2026-06-22) -- the faithful idle->wake test, and it's DECISIVE
`experiments/e3_linux/e3_wfi.c`: arm an hrtimer for `idle_ms` and RETURN so the system genuinely idles
(cores enter WFI), then the broadcast `tlbi vmalle1is; dsb sy` runs IN THE TIMER HARD-IRQ callback (the
real wake-from-idle context, == AIOS's kernel-exit -> idle -> IRQ-wake -> first-fabric-op path).
- **Result: after 30000ms of WFI idle, `dur=0ms` -- NO HANG.** (Armed 2555.5s, fired 2585.5s, 37s total ->
  no freeze.)

### FINAL CONCLUSION of the E1/E3 experiments -- the attributed mechanism is OVERTURNED
Across minimal bare-metal (EL2, busy-spin idle) AND full Linux (EL1, both stop_machine busy-spin AND genuine
WFI idle + timer-IRQ wake), a **broadcast TLBI DVM-Sync** (and a cold cacheable load) issued after idle of
10-240s **completes in 0ms on this exact silicon.** Therefore the long-held mechanism -- "the BCM2711 SCB
quiesces during idle, and the FIRST fabric/DVM op after idle hangs ~32.4s" -- is **DIRECTLY CONTRADICTED**:
that op, done deliberately after genuine WFI idle in the wake-IRQ context, does NOT hang here.
**The clincher (logic, not just one test):** AIOS has MORE ARM activity during its "idle" (prio-200 servers
yield-spinning, touching the BKL + scheduler = coherent memory traffic) than Linux's WFI idle, yet AIOS HANGS
and Linux does NOT. If the trigger were "no ARM traffic -> SCB quiesces -> next op hangs," the quieter Linux
idle should hang MORE, not less. So the trigger is NOT idle-driven SCB quiescence.
**=> The ~32.4s stall is specific to the AIOS/seL4 environment, NOT a context-free silicon property, and is
NOT the "DVM-Sync on a quiesced SCB."** The [STAGECP]/PMU localization ("first fabric op after idle") and the
keep-warm-refutation framing both need REINTERPRETATION. NEXT: back to AIOS -- with "a deliberate fabric op
after idle does not hang" as a hard constraint, re-examine what the wedge instruction/context actually is
(register-exact bracketing of the restore path; is it a specific seL4 code path, a particular MMU/ASID state,
or an interaction the experiments did not isolate -- not a generic fabric op). The experiments did their job:
they FALSIFIED the dominant hypothesis. (Caveat kept: my Linux WFI idle had RPi-OS background activity, but
the AIOS-has-more-activity-yet-hangs logic makes the conclusion robust to that.)

