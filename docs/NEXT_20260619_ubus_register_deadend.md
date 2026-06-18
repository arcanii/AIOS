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

## 3. PENDING HW VERIFICATION (Pi was OFFLINE this session -- MAC dc:a6:32:1c:2e:e1 absent from ARP)

When the Pi is back on the network (re-check 192.168.0.8 / .250 / .197 and ARP):
1. **QEMU gate FIRST** (mandatory pre-flash): smp 7/7, shmring 26/26, socket 8/8, netd 10/10.
2. **Build + flash the kernel** (safe -- with the old arm_freq=600 still on the card, GOV_MAX=1000
   clamps to 600, a harmless no-op until config.txt changes):
   `ninja -C build-rpi4-netd` -> `mkkernel8 --kernel build-rpi4-netd/images/aios_root-image-arm-bcm2711`
   -> `pi_flash.py --host <ip>`. Verify the banner build number + `cat /proc/version` shows the build
   timestamp.
3. **Raise the clock ceiling** (flash-free): `fatswap` config.txt `arm_freq=600 -> 1000` (and
   `arm_freq_min=300 -> 600` if the card still has the old floor), reboot. OR reflash the SD with the
   updated mksdcard.py (bakes 1000/600). Then `cat /proc/cpufreq` under load should show up to 1000;
   watch `/proc/temp` (should stay < ~75C; firmware throttles at 85C).
4. **Soak** with the gold detector: `python3 /tmp/pingmon.py` + `python3 scripts/netstall.py --host <ip>
   --idle 30 --trials 60`. A ping GAP coinciding with a netstall stall = a real freeze. Expectation: the
   freeze is NOT cured (no register cure exists) but worst-case severity drops (idle-teardown freezes
   capped ~33s, never ~164s). Recreate /tmp/pingmon.py from the seed if gone.
5. Keep a known-good kernel8 + the balenaEtcher SD recovery ready (a bad config.txt is fatswap-revertible;
   a bad flash needs the recovery image).

## 4. Notes
- `hw/rpi4/config.txt` is VESTIGIAL (the old v0.4.98 static file; referenced only in comments).
  `scripts/mksdcard.py create_config_txt()` is the real generator. Left untouched.
- All changes are local on `main`; Bryan pushes. The build-time headers (build_number.h, build_time.h)
  are gitignored.
