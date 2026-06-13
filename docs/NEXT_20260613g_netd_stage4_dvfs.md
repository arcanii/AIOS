# NEXT: session seed -- netd Stage 4 (re-home + default ON) + RPi4 DVFS

Paste-this-style brief for a fresh session. Read `HANDOVER.md` (top + the DONE
section "netd Stage 3 CUTOVER"), then `project_demono_netd` + `feedback_rpi4_thermal_clock`
in the memory index, then `DESIGN_NETD.md` s9 (stages) + s11 (follow-ups) for Stage 4.

---

## Paste-this brief

AIOS (research microkernel OS on seL4, repo `~/Desktop/github_repos/AIOS`, branch
`main`, at **v0.4.240**, ahead of origin -- origin at `a6b6473`; Bryan pushes via
GitHub Desktop; commit only when asked / when told to keep going, never amend /
force-push, no apostrophes in C comments). Develop + verify on QEMU; deploy to the
real Pi FLASH-FREE over the network.

**Last session COMPLETED + HW-VERIFIED the netd Stage 3 cutover.** The net stack
(socket server + TCP/UDP/DHCP + the NIC driver dev half) now runs in the
MMU-isolated `netd` CPIO process behind `AIOS_NETD`; root keeps prov (DMA/IRQ/MAC/
frames) and serves the SAME `net_ep` so the client ABI is unchanged. flag-OFF stays
byte-identical. Real Pi (v0.4.240 @ 192.168.0.8): netd serves DHCP (the **real MAC**
lease `.8` -- the retry-for-low <1GB DMA fixed the long-standing `.127` fallback),
ping, ssh, netconsole; `/proc/net` heartbeat + socket occupancy; the s10 crash demo
recovered (fault contained, reply-sweep woke the parked caller, IRQ cleared, root +
shell alive, net dead, clean reboot). ALL QEMU gates green: socket 8/8 (flag-ON AND
flag-OFF), ssh 6/6, no-`--net` zero delta, crash demo 10/10, 30-pipeline ceiling 30,
forced-degrade. Also shipped `/proc/temp` + `/proc/cpufreq` (VC-mailbox SoC temp +
ARM clock; Pi reads ~57C @ 600MHz). The Pi was updated v0.4.239 -> v0.4.240 entirely
over the network (`scripts/pi_flash.py` + `fatswap`). Commits `a6b6473`..`531aca6`.

**This session -- pick a thread (Stage 4 is the primary netd arc):**

### Thread A (primary): netd Stage 4 -- re-home + default ON (DESIGN_NETD s9/s11)
1. **`/proc/genet` root-local rewrite, UMAC/MDIO-free.** In the flag-ON root
   `genet_regs` is NULL, so `genet_diag_cmd` already returns "not present/
   initialized" (safe but BLIND -- an operator loses GENET register visibility).
   Make it a dead-netd-safe MMIO-only reader (SYS/EXT/RBUF/RDMA/TDMA ring+ctrl,
   INTRL2 status, descriptor RAM, HW prod/cons -- NO UMAC, NO MDIO, NO PHY: ANY
   UMAC access while SWINIT is latched halts the kernel, and root must not race
   netd's MDIO engine). The netd-software half renders from the `/proc/net` stats
   page. DECIDE: map a read-only GENET MMIO view in root, OR route the reads via
   NET_DIAG to netd (the fs thread must NEVER block-Call netd -- use a userland
   tool). DESIGN_NETD s6.
2. **NET_DIAG (label 103) ops in `/bin/netdiag`.** Only the crash op is wired today
   (the `/proc/netd.crash` demo). Move `.poke`/`.mw`/`.tx`/`.reinit`/`.irqon`/
   `.irqoff`/`.mac`/PHY reads there -- a userland tool Calling `net_ep` directly
   (sacrificial; a hung netd must not wedge `/proc`). DESIGN_NETD s6/s11.
3. **explicit SVC_PING reply in netd** (cosmetic -- serverstats no longer pings it;
   today SVC_PING falls through to the unknown-op `-1` reply).
4. **flip `AIOS_NETD` default ON both targets** (the big one -- makes netd the
   production net path). Re-run the full QEMU gate suite (`netd_qemu_test.py` 10/10,
   `net_socket_qemu_test.py` 8/8, `ssh_qemu_test.py`, `smp_qemu_test.py`) + a HW
   pass (pi_flash). After one stable release, **delete the in-root net path** +
   retire the flag (DESIGN_NETD s9 Stage 4). NOTE the capacity gate: netd's CPIO
   eats pool pages; the >=30-pipeline ceiling held at 30 on QEMU -- re-check on HW.

### Thread B (adjacent, self-contained): RPi4 DVFS governor -- the power lever
`/proc/cpufreq` now READS the ARM clock and the VC mailbox can SET it
(`SET_CLOCK_RATE`, id 3 -- v3d.c has the helper). The Pi runs `cur==max==600`: NO
idle downclock, because the no-WFI spin makes the firmware see 100% load and hold
the `arm_freq=600` cap. **Core-parking is OFF the table** (a core in WFE quiesces
the A72 SCU -> the v0.4.228 32.4s `tlbi vae1` stall returns). So the ONLY compatible
power lever is a load-driven DVFS governor: lower the ARM clock toward idle, raise
under load, NEVER WFI. Watch via `/proc/temp` + `/proc/cpufreq`. See
`feedback_rpi4_thermal_clock` + `project_stall_hunt`. Self-contained, RPi4-only,
doesn't touch netd.

### Small backlog
- A multi-hour Pi soak to confirm DHCP **T1 lease renewal on real GENET** (the one
  QEMU-only-proven path; `/proc/net` shows `renews: 0`, lease 86400s, T1 ~12h). The
  renewal now depends on the serverstats badge-2 kick waking idle netd.

## Workflow / deploy

- **QEMU dev:** `build-04` (flag-OFF), `build-netd` (flag-ON QEMU), `build-rpi4`
  (flag-OFF RPi4), `build-rpi4-netd` (flag-ON RPi4), `build-netd-degrade` (the
  forced-degrade gate) -- all configured. Test scripts take `AIOS_KERNEL=` /
  `AIOS_NETD_KERNEL=` env overrides.
- **Flash-free Pi deploy (no SD shuffle):** `python3 scripts/pi_flash.py --host
  192.168.0.8` (add `--build` to ninja `build-rpi4` + mkkernel8 first; here use
  `build-rpi4-netd` for the flag-ON image -- point `--kernel` or copy its image to
  `disk/kernel8.img`). It pushes kernel8 over netconsole -> `fatswap` rewrites the
  FAT32 boot partition -> 3-way sha verify -> watchdog reboot -> `/proc/version`
  check. Preflight: `fatswap /nonexistent` -> `-4` (boot part seen). Rollbacks on
  disk: `kernel8-oncard-v235-backup.img`, `kernel8-flagoff-rollback.img`.
- **Drive the Pi over netconsole GENTLY** (one held connection for many commands;
  ~4s settle between separate connections -- it wedges on back-to-back conns). The
  mini-UART serial is lossy; use short probes. ONE serial reader at a time.
- Commit as you go (Bryan pushes); build BOTH trees after shared-code changes; no
  apostrophes in C comments.

## State to verify (point-in-time)

- `main` v0.4.240, clean tree, ahead of origin (origin at `a6b6473` = the 3b commit).
- Pi at **192.168.0.8** on v0.4.240 (netd serving; ~57C @ 600MHz). Reachable;
  netconsole 2323, sshd 2222 (password `root`).
- Key refs: `DESIGN_NETD.md` (s6 stats/diag, s9 stages, s11 follow-ups); memories
  `project_demono_netd` (the netd record + HW PASS), `feedback_rpi4_thermal_clock`
  (the readouts + DVFS), `project_fatswap` (flash-over-network), `project_stall_hunt`
  (why no WFI), `feedback_genet_umac_swinit` (the SWINIT/UMAC + the .8/.127 history).
