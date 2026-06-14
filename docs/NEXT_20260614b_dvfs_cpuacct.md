# NEXT: session seed -- DVFS governor + CPU accounting (the open thread)

Paste-this brief for a fresh session. Read `HANDOVER.md` (top + the DONE/WIP section
"RPi4 DVFS + CPU accounting"), then the `feedback_rpi4_thermal_clock` memory (the full
DVFS saga + the cpuacct findings), then `project_stall_hunt` (why no WFI / the stall
cure -- relevant to the idle=0 mystery).

---

## Paste-this brief

AIOS (research microkernel OS on seL4, repo `~/Desktop/github_repos/AIOS`, branch
`main`, at **v0.4.243**, ahead of origin -- origin at `555d915`; `5b09d6f` + `e0cbb3a`
are ahead, Bryan pushes via GitHub Desktop; commit when asked, never amend /
force-push, no apostrophes in C comments). Develop + verify on QEMU; deploy to the
real Pi FLASH-FREE over the network. **Run the FULL QEMU gate suite before any Pi
flash** (BACKLOG process requirement): `netd_qemu_test.py` 10/10, `net_socket_qemu_test.py`
8/8 (flag-ON + flag-OFF), `ssh_qemu_test.py` 6/6.

**Last session shipped a /proc/cpuacct per-thread CPU-cycle table** (`5b09d6f`,
`KernelBenchmarks=track_utilisation`) to diagnose why the **load-driven DVFS governor
(`ee655e7`, v0.4.242) is BUGGY -- it holds 600MHz in idle and never cools. DO NOT
PUSH `ee655e7` AS WORKING.** The DVFS MECHANISM works: `config.txt arm_freq_min=300`
opened the floor so the VC mailbox `SET_CLOCK_RATE` pins 300/450/600 and
`/proc/cpufreq.set.MHZ` drives it manually; the firmware does NOT auto-boost under
load, so an explicit governor is needed. The governor's root-loop-rate metric is
confounded. The cpuacct HOG-HUNT verdict: **NO single hog** -- the ~50% core-0 "stall"
is (1) the netconsole RELAY (a connected session shows pipe ~42% + unaccounted ~32% --
the observer effect, proven) and (2) the spread of background threads (tlbi_probe is
only ~11%, EQUAL to root/serverstats/flush).

**This session -- pick a thread:**

### Thread A (primary): fix /proc/cpuacct + rewire the DVFS governor

1. **Fix the two cpuacct measurement bugs (src/cpuacct.c):**
   - The `%` total is from `PMCCNTR` (32-bit, WRAPS ~7s at 600MHz; no `KERNEL_PMU_IRQ`
     on BCM2711) -> windows >7s give >100% per-thread. Fix: compute the total as the
     SUM of the 64-bit per-TCB deltas (+ idle), OR keep reads <7s. (The per-TCB
     `utilisation` is a 64-bit accumulator, wrap-safe; only the PMCCNTR total wraps.)
   - **The kernel idle thread reads 0** (`BENCHMARK_IDLE_LOCALCPU_UTILISATION`) yet
     root is only ~11% -- so something consumes core 0 that the idle accounting misses.
     CRUX: does the RPi4 root actually SPIN (`seL4_Yield`, aios_root.c main loop) or
     BLOCK (`seL4_Wait`) at idle? Check `irq_uart_active` on RPi4. If it BLOCKS, the
     kernel idle thread should run (and reading 0 is a tracking bug to chase); the
     "no-WFI spin" assumption (the v0.4.228 stall cure, [[project-stall-hunt]]) may be
     wrong/partial -- IMPORTANT, since the stall cure depends on cores not quiescing.
2. **Rewire the governor (src/cpu_gov.c) onto a cpuacct work-load metric.** Replace
   the loop-rate metric: load = the WORK threads (pipe + fs + net + the unaccounted
   user procs) EXCLUDING the background (root/tlbi_probe/serverstats/flush), as a
   fraction of the total, over a <7s window. Low load -> downclock to 300, high ->
   600. This sidesteps idle=0. Verify by TEMPERATURE over a disconnected idle (the
   ground truth -- ~53C downclocked vs ~64C held; the observer effect makes any
   connected read unreliable). Then HW-flash + confirm it cools at idle + boosts
   under a real workload (e.g. tcc compile).
3. Once it works: clean up the gov_dbg diagnostics, version-bump, commit, retire the
   "DO NOT PUSH" warning on the governor.

### Thread B (secondary): netd Stage 4 item-4 -- flip AIOS_NETD default ON

The netd arc's last step (items 1-3 are DONE + HW-verified). Flip the CMake `option`
OFF->ON for both targets, fix the OFF/ON test matrix (the flag-OFF trees then need an
explicit `-DAIOS_NETD=OFF`), re-run the full gate suite; after one stable release
delete the in-root net path. Seed `docs/NEXT_20260614_netd_stage4_flip.md` + memory
`project_demono_netd`.

### Backlog
- **General FAT32 mount / config.txt-over-network** (BACKLOG.md) -- AIOS cannot edit
  its FAT today (fat32.c is KERNEL8.IMG-swap-only); the arm_freq_min edit needed a
  physical SD mount. A general FAT read/write would make boot-config tweaks flash-free.
- Multi-hour Pi soak for DHCP T1 lease renewal on real GENET (`renews: 0`).

## Workflow / deploy

- Trees: `build-04` (flag-OFF QEMU), `build-netd` (flag-ON QEMU), `build-rpi4`
  (flag-OFF RPi4), `build-rpi4-netd` (flag-ON RPi4 -- the FLASH TARGET). ALL now build
  with `KernelBenchmarks=track_utilisation`. Build BOTH trees after shared-code changes.
- Flash-free deploy: `mkkernel8.py --kernel build-rpi4-netd/images/aios_root-image-arm-bcm2711
  --output disk/kernel8.img` then `python3 scripts/pi_flash.py --host 192.168.0.8`.
  No version bump? the build-number in the banner still differs, so pi_flash detects it.
- `/proc/cpuacct` reads DELTAS between two reads -- read once to prime, then again.
  For a CLEAN (relay-free) idle read: prime, disconnect, idle <7s, reconnect, read.
  TEMPERATURE (`/proc/temp`) is the only observer-effect-proof governor signal.
- Drive netconsole GENTLY (one held conn; ~4-9s settle between conns; it wedges on
  back-to-back). The Pi is parked 300/gov-off; the governor is ON at boot (buggy).

## State to verify (point-in-time)

- `main` v0.4.243, clean, ahead of origin (`555d915`). Key commits: `5b09d6f` cpuacct,
  `ee655e7` buggy governor, `f12eddc` DVFS Phase 0, `eeb2785` netd Stage 4 items 1-3.
- Pi at **192.168.0.8** on build 2231, parked 300MHz / governor OFF / ~64C. netconsole
  2323, sshd 2222 (pw root).
- Key refs: memories `feedback_rpi4_thermal_clock` (the saga), `project_demono_netd`,
  `project_stall_hunt`; `BACKLOG.md` (FAT mount + the pre-flash-gate requirement).
