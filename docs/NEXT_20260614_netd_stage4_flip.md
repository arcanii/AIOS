# NEXT: session seed -- netd Stage 4 FINAL (flip default ON) + RPi4 DVFS

Paste-this-style brief for a fresh session. Read `HANDOVER.md` (top + the DONE
section "netd Stage 4 items 1-3"), then `project_demono_netd` +
`feedback_rpi4_thermal_clock` in the memory index, then `DESIGN_NETD.md` s9 (stages)
for the item-4 flip.

---

## Paste-this brief

AIOS (research microkernel OS on seL4, repo `~/Desktop/github_repos/AIOS`, branch
`main`, at **v0.4.241**, ahead of origin -- origin at `a6b6473`; Bryan pushes via
GitHub Desktop; commit only when asked, never amend / force-push, no apostrophes in
C comments). Develop + verify on QEMU; deploy to the real Pi FLASH-FREE over the
network.

**Last session COMPLETED + HW-VERIFIED netd Stage 4 items 1-3** (the re-home prep,
DESIGN_NETD s6/s9), commit `eeb2785`, flag-gated (`AIOS_NETD` default still OFF):
- **Item 1 -- `/proc/genet` read-only, UMAC/MDIO-free.** flag-ON root is prov-only
  (`genet_regs` NULL) but keeps its GENET MMIO at `dev_genet_vaddr`; `genet_diag_cmd`
  `#ifdef AIOS_NETD` -> `genet_diag_readonly()` renders SYS/EXT/RBUF/INTRL2/RDMA/TDMA
  + RX descriptors + `.peek.OFF`, never UMAC/MDIO/PHY. flag-OFF unchanged (`#else`).
- **Item 2 -- active ops in `/bin/netdiag`.** peek/poke/mr/mw/tx/reinit/irqon/irqoff/
  mac via `aios_net_diag()` -> `net_ep` `NET_DIAG`(103) -> netd `plat_net_diag()` HAL
  (net_genet.c + net_virtio.c). The fs thread never Calls netd; netdiag is the
  sacrificial userland caller.
- **Item 3 -- explicit `SVC_PING` -> 0** in net_server.

Verified QEMU: new `scripts/netdiag_qemu_test.py` 6/6, `netd_qemu_test` 10/10,
`net_socket` 8/8 (flag-ON + flag-OFF), `ssh` 6/6, all 4 trees build. **HW-verified on
real GENET (v0.4.241 deployed flash-free, Pi @ 192.168.0.8):** `/proc/genet` read-only
view (`SYS rev=06000000`, `EXT oob=f10050`, no UMAC); netdiag live ops -- `mr 1 2`=`600d`
+ `mr 1 3`=`84a2` (live MDIO reads the BCM54213 PHYID), `mr 1 1`=`794d` (link up),
`mac`=`dc:a6:32:1c:2e:e1`, `peek 0`=`06000000`, `tx` ret 0; ping 0%, netd heartbeat
advancing, no crash. The new netdiag ELF was PUSHED to the Pi `/bin/netdiag` (kernel
swap does not update disk apps; durable copy lands on the next SD rebuild).

**This session -- pick a thread:**

### Thread A (primary): netd Stage 4 item 4 -- flip AIOS_NETD default ON (the FINAL step)

The behavioral + HW work of Stage 4 is DONE; only the default flip remains.
1. **Flip the CMake default OFF -> ON** in `projects/aios/CMakeLists.txt`
   (`option(AIOS_NETD ... OFF)` -> `ON`). This makes netd the production net path
   for fresh configures (and the SD-card build, `build-rpi4`).
2. **Fix the OFF/ON test matrix.** Once the default is ON, the "flag-OFF" trees
   (`build-04`, `build-rpi4`) need an explicit `-DAIOS_NETD=OFF` to keep testing the
   in-root path. The test scripts already take `AIOS_KERNEL=`/`AIOS_NETD_KERNEL=`
   overrides; reconcile which tree each gate points at so both paths stay covered.
3. **Re-run the full gate suite** against the now-default-ON build: `netd_qemu_test`
   10/10, `netdiag_qemu_test` 6/6, `net_socket` 8/8, `ssh` 6/6, `smp_qemu_test`
   (>=30-pipeline ceiling). Then a HW pass (the Pi already runs flag-ON v0.4.241;
   a fresh default-ON build is byte-equivalent, so this is mostly a re-confirm).
4. **After one stable release: delete the in-root net path + retire the flag**
   (DESIGN_NETD s9 Stage 4 end state). NOT this session unless Bryan wants it --
   v0.4.241 is the stable-release candidate; let it soak first.

### Thread B (adjacent, self-contained): RPi4 DVFS governor -- the power lever

`/proc/cpufreq` READS the ARM clock and the VC mailbox can SET it (`SET_CLOCK_RATE`,
id 3 -- `v3d.c` has the helper). The Pi runs `cur==max==600`: NO idle downclock, the
no-WFI spin makes the firmware see 100% load. **Core-parking is OFF the table** (a
core in WFE quiesces the A72 SCU -> the v0.4.228 32.4s `tlbi` stall). So the only
compatible power lever is a load-driven DVFS governor: lower the ARM clock toward
idle, raise under load, NEVER WFI. Watch via `/proc/temp` + `/proc/cpufreq`. See
`feedback_rpi4_thermal_clock` + `project_stall_hunt`. Self-contained, RPi4-only,
doesn't touch netd.

### Small backlog
- A multi-hour Pi soak to confirm DHCP **T1 lease renewal on real GENET** (`/proc/net`
  shows `renews: 0`, lease 86400s, T1 ~12h; renewal rides the serverstats badge-2 kick).

## Workflow / deploy

- **QEMU dev:** `build-04` (flag-OFF), `build-netd` (flag-ON QEMU), `build-rpi4`
  (flag-OFF RPi4), `build-rpi4-netd` (flag-ON RPi4), `build-netd-degrade`. Test
  scripts take `AIOS_KERNEL=`/`AIOS_NETD_KERNEL=` overrides. After the item-4 flip,
  re-examine which tree is flag-OFF (you may need a fresh `-DAIOS_NETD=OFF` tree).
- **Build BOTH trees after shared-code changes**; `build_apps.py` rebuilds the disk +
  relinks dash/sshd against `libaios_posix` (needed after posix changes).
- **Flash-free Pi deploy:** kernel changes = `mkkernel8.py --kernel
  build-rpi4-netd/images/aios_root-image-arm-bcm2711 --output disk/kernel8.img` then
  `python3 scripts/pi_flash.py --host 192.168.0.8` (push 1.89MB over netconsole ->
  fatswap FAT rewrite -> 3-way sha -> watchdog reboot -> `/proc/version` check;
  crash-safe -- a failed swap keeps the old kernel). **App-only changes** (e.g.
  netdiag) ship over the LAN with `pi_filexfer.py push <local-elf> /bin/<tool>
  192.168.0.8` (no reflash). Drive netconsole GENTLY (one held connection for many
  commands; it can drop the first connect post-reboot -- retry).

## State to verify (point-in-time)

- `main` v0.4.241, clean tree, ahead of origin (origin at `a6b6473`). Commit `eeb2785`
  = Stage 4 items 1-3.
- Pi at **192.168.0.8** on v0.4.241 (netd serving; `/proc/genet` read-only view live,
  netdiag GENET ops work). netconsole 2323, sshd 2222 (password `root`).
- Key refs: `DESIGN_NETD.md` (s6 stats/diag, s9 stages); memories `project_demono_netd`
  (the netd record + Stage 4 1-3 HW PASS), `feedback_rpi4_thermal_clock` (DVFS),
  `project_fatswap` (flash-over-network), `project_stall_hunt` (why no WFI).
