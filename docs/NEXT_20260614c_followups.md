# NEXT: session seed -- follow-ups after the DVFS / netd / getty / FAT session

Paste the brief below into a fresh session. Then read `HANDOVER.md` (top + the
arc DONE sections), the memory index (`MEMORY.md`, auto-loaded), and -- per the
thread you pick -- the named memories.

---

## Paste-this brief

AIOS (research microkernel OS on seL4, repo `~/Desktop/github_repos/AIOS`, branch
`main`, at **v0.4.244**, ahead of origin -- Bryan pushes via GitHub Desktop;
commit only when asked, never amend / force-push, no apostrophes in C comments).
Develop + verify on QEMU; deploy to the real Pi FLASH-FREE over the network.
**Run the FULL QEMU gate suite before any Pi flash:** `netd_qemu_test.py` 10/10,
`net_socket_qemu_test.py` 8/8 (flag-ON + flag-OFF), `ssh_qemu_test.py` 6/6.

**Last session (v0.4.243 -> v0.4.244) shipped FOUR HW-verified features** (see the
HANDOVER arc sections + the memories named):
1. **RPi4 DVFS governor** -- load-driven ARM clock (idle 300 / load 600), cooling
   confirmed (`feedback_rpi4_thermal_clock`).
2. **netd Stage 4** -- `AIOS_NETD` default flipped ON; netd is the production net
   path (`project_demono_netd`).
3. **getty respawn-supervisor** -- respawns crashed netconsole/sshd
   (`project_netconsole`).
4. **FAT config-over-network** -- `fatswap --read/write` any boot file; config.txt
   editable flash-free (`project_fatswap`, `BACKLOG.md`).

The Pi runs build **2268 / v0.4.244** at 192.168.0.8 (governor ON, netd serving,
supervisor getty, new fatswap, config.txt intact with `arm_freq_min=300`).

### This session -- pick a thread

**Thread A (quick, HW): ssh multi-session test.** The one open follow-up from the
getty supervisor: does respawning sshd fix the "one ssh session per boot" limit?
(`project_ssh_recovered`: sshd "shell fork corrupts conn2".) If sshd DIES after a
session, getty respawns it -> conn2 works = a real UX win; if it stays
alive-but-corrupted, respawn will not help. Test: `ssh -tt` once, wait ~15s,
`ssh -tt` again; check whether conn2 succeeds + whether sshd's pid changed in
`/proc/status`. Needs ssh-with-password tooling against the Pi (see
`ssh_qemu_test.py` for the auth pattern). Drive netconsole gently.

**Thread B: netd Stage 4 end state -- delete the in-root net path + retire the
flag.** DESIGN_NETD s9. After the default-ON release soaks, remove the `#else`
(in-root) branches of `#ifdef AIOS_NETD`/`NETD_BUILD` and the option. Big,
mechanical, removes the dual-path test matrix. Confirm soak stability first.

**Thread C: governor pure-compute gap.** The work-server load metric misses a
pure-compute-no-IO loop (no pipe/fs/exec traffic). Options: register netd's main
thread, or a hybrid metric. Tune thresholds via `/proc/cpufreq.tune.LO.HI.IT`.
Low priority (real workloads do I/O).

### Backlog
- A real FAT `/boot` MOUNT (vs the current single-file root-dir read/write).
- A memory-consolidation pass (`anthropic-skills:consolidate-memory`) -- the
  governor/netd/getty/FAT memories grew a lot this session.
- Multi-hour Pi soak for DHCP T1 lease renewal on real GENET (`renews: 0`).
- The residual heavy-spawn-storm TLBI stall (`project_stall_hunt`, deferred kernel
  change) -- the root cause of every netconsole wedge.

## Workflow / deploy

- **Trees:** `build-04` (flag-OFF QEMU), `build-netd` (flag-ON QEMU), `build-rpi4`
  (flag-OFF RPi4), `build-rpi4-netd` (flag-ON RPi4 -- the FLASH TARGET). `AIOS_NETD`
  now defaults ON, so the flag-OFF trees need an explicit `-DAIOS_NETD=OFF`. Build
  BOTH (QEMU + RPi4) after shared-code changes.
- **DEPLOY -- know what is where:** root-task / kernel changes = a kernel FLASH
  (`mkkernel8.py --kernel build-rpi4-netd/images/aios_root-image-arm-bcm2711
  --output disk/kernel8.img` then `pi_flash.py --host 192.168.0.8`). DISK apps
  (getty, fatswap, netconsole, sshd, ...) = a network PUSH
  (`pi_filexfer.py push <local> /bin/aios/<app> 192.168.0.8`), NO flash. GOTCHA
  that cost an HW attempt this session: a feature split across BOTH (FAT: engine in
  the root task + the `fatswap` CLI on disk) needs a flash AND a push.
- **netconsole is stall-prone** (the ~32s TLBI kernel stall freezes it). Drive
  gently (one held conn, settle between conns) and ride out stalls with an
  escalating-backoff retry -- reuse the robust `Pi`/`nc_cmd` pattern in
  `scripts/gov_cooling.py` / `scripts/fatconfig_hw.py`. NEVER blind `pkill`.
- **QEMU cannot model:** RPi4 cache attributes, the VC mailbox (governor/temp), the
  FAT boot partition (use a partitioned MBR+FAT+ext2 test disk via `mksdcard
  --output disk/sdcard-test.img`, as `fatconfig_qemu_test.py` does), eMMC timing,
  GENET. Verify those on the Pi -- via push-over-net + the robust netconsole driver.

## State to verify (point-in-time)

- `main` v0.4.244, clean tree. Key commits: `72a00f0` governor, `6659dce` netd
  flip, `d70b0a2` cooling, `a5b7273`+`a206129` getty supervisor, `ca595ce`+
  `a2f7179` FAT config, `4c58dfa` docs.
- Pi at **192.168.0.8**, build **2268**, governor ON / cool at idle, config.txt has
  `arm_freq_min=300`. netconsole 2323, sshd 2222 (pw root).
- Key memories: `feedback_rpi4_thermal_clock`, `project_demono_netd`,
  `project_netconsole`, `project_fatswap`, `project_stall_hunt`.
