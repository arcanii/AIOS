# 2026-06-19 -- BCM2711 UBUS-timeout register = DEAD END + DVFS/uname deliverables

Session goal (seed): cure the residual ~2-3% RPi4 idle-teardown freeze by finding + bounding the
BCM2711 SCB/ARM-cluster UBUS-timeout register (default 0x80000 ticks @ ~16.2kHz = 32.4s), the ONLY
remaining cure lever per the prior A72-register dead ends. Plus two feature requests: build-time in
uname, and raise the core clock to 900-1000MHz max.

## 1. THE REGISTER HUNT IS A CONFIRMED DEAD END (do NOT re-chase)

Two independent primary-source research passes (Linux/DT mining + U-Boot/datasheet/bcm2712 mining) both
concluded, decisively, that **no writable non-PCIe BCM2711 fabric-timeout register defaulting to 0x80000
exists or is wired**, AND that the premise itself (0x80000 = 32.4s) is almost certainly a coincidence.

Evidence (all primary-sourced):
- **GISB arbiter** (`drivers/bus/brcmstb_gisb.c`) -- the one Broadcom block with a writable `ARB_TIMER`
  fabric-bus-timeout -- is **STB-only** (bcm7038/7278/7400/7435/7445/74165). NOT instantiated on BCM2711
  or BCM2712: no `brcm,*-gisb-arb` compatible, no DT node. BCM2711's `scb` is a plain `simple-bus`
  (address container, zero registers). Canonical STB GISB base 0xf0400000 is not in BCM2711's map.
- **ARM-local 0xFF800000 is ONLY the L1-intc + GIC-400** (DT-proven, bcm2711.dtsi soc ranges). No
  fabric/arbiter/timeout/sun_top_ctrl block. The seed's "~0xff8xxxxx" hypothesis is REFUTED.
  - **CORRECTION 2026-06-20 (session-3 primary-source research):** this bullet is too strong. The
    BCM2711 ARM-local block at 0xFF800000 DOES contain a fabric-related register the DT does not name:
    **AXI_QUIET_TIME @ offset 0x30** (BCM2711 peripherals datasheet 6.5.2 -- the ARM-local "AXI Quiet"
    block raises a Core-0 IRQ "if no AXI bus traffic from the ARM cluster to VideoCore for a
    programmable time"). It is a fabric-quiesce DETECTOR, not a writable transaction-timeout, so it can
    only INSTRUMENT the freeze (timestamp bus-quiesce onset vs stall onset) + measure a keep-warm
    period -- it cannot BOUND the hang. So the doc's HEADLINE conclusion (no software-writable fabric
    timeout that could turn the 32s freeze into a blip) STILL STANDS; only the "no quiesce block at all"
    phrasing was wrong. ACTION: map the ARM-local page + arm AXI_QUIET_TIME as the #1 cause-instrument.
- The **only** `UBUS_TIMEOUT` in all of Linux/U-Boot is the PCIe RC's at 0xFD500000+0x40a8 (already
  bounded v0.4.213). No second non-PCIe copy. `biuctrl.c` (A72 CPU<->fabric tuning) has only
  credits/throttle/prefetch, no DVM/transaction timeout, and is unwired on BCM2711.
- **The killer arithmetic:** every documented BCM2711/2712 fabric timeout ticks at 216 MHz (PCIe
  completion timeout @0xFD509208) or 750 MHz (UBUS_TIMEOUT on bcm2712). At those rates 0x80000 =
  2.4ms / 0.7ms, NOT 32.4s. The ~16.2kHz needed for 32.4s matches no documented BCM2711 clock (osc
  54MHz, legacy 19.2MHz, sys-timer 1MHz, PM watchdog 62.5kHz). So 0x80000<->32.4s is a coincidence,
  not a register expiry. A real fabric timeout raises a CPU abort in ~ms, not a 32s self-resolving spin.

**Conclusion:** the freeze is the A72 `tlbi vae1`+dsb **DVM completion hang** on a quiesced SCU/L2, and
BCM2711 has **no fabric arbiter to bound it** -- which is exactly why every A72-register lever (probe,
L2ACTLR[27] B, full-cluster B+) and now the fabric-register lever all fail. There is no software-writable
timeout on this silicon. **The blind read-only MMIO scan of 0xFD000000-0xFD5FFFFF was NOT done**: the
premise is disproven, and the Pi was offline this session anyway (see below). Memory:
`project_stall_ubus_deadend`. The remaining levers are clock severity-mitigation (below) and the SMP
re-architecture (BACKLOG).

## 2. DELIVERED (code landed, QEMU-verified; HW verification PENDING -- Pi was offline)

### A. uname / /proc/version now show the full kernel BUILD TIME
- `scripts/bump-build.sh` (runs PRE_BUILD on aios_root, every build) now also writes
  `include/aios/build_time.h` = `#define AIOS_BUILD_TIME "Wkd Mon DD HH:MM:SS TZ YYYY"` from the host
  `date` (weekday + TZ are not available from C `__DATE__`/`__TIME__`). Gitignored + created by
  `setup-linux.py setup_build_time()`, same pattern as build_number.h.
- `include/aios/version.h`: `AIOS_BUILD_TIME` via `__has_include("build_time.h")` with a
  `__DATE__ " " __TIME__` fallback (never breaks a fresh-tree compile).
- `/proc/version` (src/procfs.c): trailing date -> full build timestamp.
- `uname` version field (src/servers/fs_server.c FS_UNAME + src/lib/posix_misc.c): the 16-byte version
  field was too small for the 28-char timestamp, so the FS_UNAME IPC now packs version as 32 bytes,
  **backward-compatibly**: MR0-9 unchanged (old disk uname binaries still read sysname/nodename/release/
  machine + a truncated 16-byte version), new clients read MR10-11 for the full 32-byte version. So a
  kernel8-only flash does NOT break the old on-disk `uname`; a disk redeploy makes `uname -v` show the
  full stamp.

### B. Core clock raised to 1000MHz max + floor raised to 600 (feature + freeze severity-mitigation)
- `src/cpu_gov.c`: `GOV_MIN_MHZ 300->600`, `GOV_MAX_MHZ 600->1000`.
- `scripts/mksdcard.py`: `ARM_FREQ_CAP_MHZ 600->1000`, new `ARM_FREQ_MIN_MHZ=600`; config.txt now emits
  `arm_freq=1000` + `arm_freq_min=600`.
- Rationale: the feature ask was max 900-1000. The floor-raise is the seed's fallback after the register
  cure died -- the freeze scales inversely with clock (HW-proven 600->~33s vs 300->~164s), so never
  idle-downclocking below 600 caps the worst-case disconnected-idle freeze at ~33s instead of ~164s.
  600 is thermally proven-stable (the old fixed cap). Net effect: connected use runs at up to 1000MHz
  (fast), disconnected-idle holds 600 (worst-case freeze ~33s). Strictly better freeze profile + the
  requested perf.
- THERMAL: 1000 < 1500 stock max, no over_voltage needed; firmware throttles at 85C. Dial CAP to 900 if
  HW temps run hot under sustained 4-core load -- one constant. To favor idle cooling over freeze
  severity, drop ARM_FREQ_MIN_MHZ + GOV_MIN_MHZ back to 300.

## 3. HW VERIFICATION -- DONE 2026-06-19 (Pi came back online at 192.168.0.8 mid-session)

All deployed + verified on the real Pi over the network (flash-free):
1. **QEMU gate (pre-flash): GREEN** -- smp 7/7, shmring 26/26, socket 8/8, netd 10/10.
2. **Kernel flashed**: `mkkernel8 --kernel build-rpi4-netd/images/aios_root-image-arm-bcm2711 --output
   disk/kernel8_v262.img` -> `pi_flash.py --host 192.168.0.8 --kernel disk/kernel8_v262.img`. Banner
   verified: **AIOS v0.4.262 (build 2610)**. (My kernel == the running candidate B -- zero kernel changes
   this session -- so the flash was low-risk.) Rollback preserved: disk/kernel8_v261_candidateB_l2clk.img.
3. **Build time LIVE on HW**: `/proc/version` =
   `AIOS v0.4.262 (build 2610) (seL4 15.0.0, arm,cortex-a72, 4-core SMP) Fri Jun 19 08:39:26 JST 2026`.
4. **Clock raised to 1000MHz**: config.txt edited flash-free -- generated the canonical config via
   mksdcard.create_config_txt (arm_freq=1000, arm_freq_min=600), pushed + `fatswap` (sha-verified
   src==disk), reboot. HW-confirmed `arm_cur_mhz=1000 arm_max_mhz=1000` (was 600). Temp 59.9C at boot,
   63.7C after the soak -- well under the 85C throttle (21C headroom). NO throttling
   confirmed: arm_cur stayed 1000 even at governor busy=945. (A 4-wide compute burst
   hit the existing process-capacity "Cannot fork" ceiling -- NOT thermal; a sustained
   heavy build would run hotter but the firmware auto-throttles at 85C.)
5. **Soak (pingmon + netstall --idle 30 --trials 40 at 1000MHz)**: 1/40 STALLED (2.5%), trial 8 =
   33.0s residual, 0 conn-deaths, 39 clean. pingmon GAP #1 = 33.1s COINCIDED with trial 8 = a confirmed
   real whole-system freeze. **KEY: the freeze is ~33s = ONE quantum, NOT worsened at 1000MHz** -- the
   ~32.4s fabric timeout is CLOCK-INDEPENDENT (a fixed UBUS-class timeout, not ARM-clock-scaled). So:
   - The CEILING raise (600->1000) is purely the perf feature; it does NOT shorten the freeze and does
     NOT regress it (still ~2.5% / 1 quantum, same as 600MHz baseline).
   - The FLOOR raise (300->600) is the real freeze-severity mitigation: it caps the worst case at ~33s
     (1 quantum) by never idle-downclocking to 300, where the prior session measured ~164s (multi-quantum).
   - The freeze RATE is unchanged (the cure is dead -- section 1). Light/normal use stays mostly fine.
6. **uname full timestamp on HW**: rebuilt sbase (`build_sbase.py`) for the new client + pushed the new
   `uname` to `/bin` (the disk binary; `/proc/version` and the getty banner are root-task and shipped via
   the kernel8 flash). NOTE: this /bin overwrite is a manual network deploy -- it reverts on a full disk
   image reflash unless mkdisk is rebuilt; the canonical path is a disk reflash.

Discipline notes: netconsole WEDGES on back-to-back connections / chokes on large pushes (the 460KB uname
push broke once, succeeded on retry with settle) -- drive it gently, 1-cmd-per-conn, settle ~15-20s. Keep
disk/kernel8_v261_candidateB_l2clk.img + the balenaEtcher SD recovery ready.

## 4. Notes
- `hw/rpi4/config.txt` is VESTIGIAL (the old v0.4.98 static file; referenced only in comments).
  `scripts/mksdcard.py create_config_txt()` is the real generator. Left untouched.
- All changes are local on `main`; Bryan pushes. The build-time headers (build_number.h, build_time.h)
  are gitignored.
