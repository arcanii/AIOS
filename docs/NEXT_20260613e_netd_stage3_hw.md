# NEXT: session seed -- 2026-06-13e (netd Stage 3 -- the real-RPi4 HW pass)

> **HW PASS DONE 2026-06-13f (v0.4.239).** The real Pi booted netd, took the
> real-MAC lease **192.168.0.8**, and passed every gate (DHCP/ping/ssh/netconsole,
> `/proc/net`, s10 crash recovery). The first boot caught + fixed a HW-only bug
> (prov-time UMAC write with `genet_regs` NULL -> fault `0x80c`; fix = the
> `genet_in_prov` guard, commit `b0a34fc`), and the retry-for-low DMA resolved the
> long-standing `.127` fallback. Pi is healthy at 192.168.0.8 on v0.4.239. The
> sections below are the (completed) HW-pass plan, kept for reference. **What still
> remains: the forced-degrade QEMU gate + Stage 4 (re-home + default ON)** -- see
> the bottom two sections. Full record: `project_demono_netd` memory "HW PASS".

Read `HANDOVER.md` (top + the DONE section "netd Stage 3 CUTOVER") and the memory
`project_demono_netd` first. This doc is the orientation + the HW-pass plan.

---

## Paste-this brief

AIOS (research microkernel OS on seL4, repo `~/Desktop/github_repos/AIOS`, branch
`main`, at **v0.4.238**, **ahead-2 of origin** -- origin at `a6b6473`; Bryan
pushes `532fccd` + `bc590ef` via GitHub Desktop; commit only when asked, never
amend/force-push, no apostrophes in C comments).

The **netd Stage 3 CUTOVER is COMPLETE + QEMU-VERIFIED** (2026-06-13e). With
`AIOS_NETD=ON` the entire net stack runs in the MMU-isolated `netd` CPIO process;
root keeps prov (DMA/IRQ/MAC/frames) and serves the SAME `net_ep` so the client
ABI is unchanged. flag-OFF is byte-identical. QEMU gates all green: bring-up +
DHCP + ping, socket suite 8/8, ssh 6/6, netconsole, serverstats, SNTP, the s10
crash-containment demo (`netd_qemu_test.py` 10/10 -- fault contained, reply-sweep
woke sshd+netconsole, IRQ cleared, shell alive, net dead), no-`--net` zero delta,
30-pipeline ceiling 30, flag-OFF socket 8/8. All three trees build.

**This session: the real-RPi4 Step-4 HW pass** -- the FIRST netd-on-Pi boot. QEMU
cannot model the GENET datapath, cache attributes, the VC mailbox, eMMC latency,
or the brcmstb DMA reach, so every netd HW risk is here. Plus a small
forced-degrade QEMU gate, then Stage 4 (re-home + default ON).

---

## Build + deploy (flash-free kernel8 swap; netd is a root-task-image change)

netd-in-CPIO + the cutover live in the root task image, so deploy is a kernel8
swap, NOT a userspace push. There is no flash-over-network for kernel8 yet (FAT
write pending), so each HW iteration + rollback is a PHYSICAL SD shuffle -- plan
to get it right in few boots; QEMU is already exhausted.

```
# build the RPi4 image with the flag ON (build-rpi4 is currently flag-OFF;
# either reconfigure it ON, or use a dedicated build-rpi4-netd dir):
cd ~/Desktop/github_repos/AIOS
cmake -G Ninja -S . -B build-rpi4-netd \
    -DCMAKE_TOOLCHAIN_FILE=deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- \
    -DAIOS_PLATFORM=PLAT_RPI4 -DKernelPlatform=bcm2711 -DAIOS_NETD=ON   # match build-rpi4 cache
ninja -C build-rpi4-netd
python3 scripts/mkkernel8.py --build build-rpi4-netd      # -> disk/kernel8.img
# physically move the SD card; copy disk/kernel8.img onto the FAT (AIOSBOOT) part;
# verify the sha on the card + the serial banner build number after boot.
```

KEEP a known-good flag-OFF `kernel8.img` (build-rpi4 v0.4.238) on hand for instant
rollback (SD shuffle). The flag-OFF image is byte-identical to today's net
behaviour, so rollback is safe.

Capture serial at 115200: `python3 scripts/aios_console.py monitor
/dev/cu.usbserial-0001`. ONE serial reader at a time. Do NOT hammer the v1
netconsole (it wedges on back-to-back HW connections -- power-cycle to recover);
drive it gently, one held connection.

---

## HW gates (in order; the headline DHCP+ping is passive)

1. **netd boots + provisions on real GENET.** Serial: `netd: provisioned
   (net_hw_present=1)`, `[netd] spawned ...`, `[netd] up: ...`, `netd: DEVD_READY
   received`, `netd: READY -- net_ep published`. Then netd runs the GENET register
   sequence (SWINIT/UMAC/RBUF/RGMII/ring/INTRL2) ITSELF -- watch for the
   `[net] cfg: OOB=... RDMActrl=... CMD=...` line and `RPi4 GENET ready`. The
   sacred sequence is byte-identical to the in-root driver (it just moved); if it
   halts right after the rev read, suspect a UMAC-before-SWINIT-release ordering
   regression (`[[feedback-genet-umac-swinit]]`).
2. **retry-for-low DMA (HW-ONLY).** `net_genet dma_init` now requires the 128KB
   DMA region `< 0x40000000`. Serial: `[net] DMA region: ... phys=0x... (128KB,
   <1GB, N reject(s))`. Early boot has low RAM, so N is usually 0; if it logs
   `no <1GB region found in 8 tries -- net unavailable`, the allocator handed only
   high frames -- net degrades LOUD (not the fake MAC). Verify the mailbox MAC
   read still works with a low region (`[net] real MAC (mailbox, prov): ...`).
3. **DHCP + ping (headline, passive).** Real-LAN lease (`.8` with the real MAC, or
   `.127` fallback -- both fine; the GENET real-MAC read is a separate backlog
   item, harmless). Bidirectional ping 0% loss.
4. **GENET IRQ-RX under netd.** netd self-bound the unbadged ntfn; the IRQ signals
   root's badge-1 mint -> netd's Recv wakes. Confirm `cat /proc/net` heartbeat
   advances and (over netconsole) `cat /proc/genet.ip` shows `rxp`/`irq` climbing
   under a ping flood. The merged-loop NAPI re-check is HW-load-bearing.
5. **ssh + netconsole served by netd.** `ssh -tt -p 2222 root@<ip>` (password
   `root`); netconsole on 2323. Both ride netd's accept/recv now.
6. **`/proc/net` IPC-free + serverstats.** `cat /proc/net` (heartbeat, dhcp, mac,
   ip, sockets); `cat /proc/serverstats` net row `ok` (heartbeat-fed; NOT a Call).
7. **s10 crash recovery on real GENET.** `cat /proc/netd.crash` over SERIAL (NOT
   netconsole -- the crash kills it). Expect `[netd-listener] FAULT ... net DOWN,
   sweeping` + `swept N parked caller(s); IRQ cleared`, then the SERIAL shell
   stays alive + serverstats net `dead`. CAVEAT: on a standalone Pi a netd crash
   leaves HDMI+USB as the only console, and the HDMI console freezes on first
   scroll (`NEXT_20260610`) -- so do the crash test with the SERIAL cable attached.
8. **Soak.** A multi-hour run with periodic transfers + USB kbd + HDMI attached
   (the xHCI poll thread shares core 0). Confirms the lease RENEWAL on an idle
   netd works -- it now depends on the serverstats badge-2 kick waking netd, which
   is QEMU-proven but not HW-soaked. Watch `/proc/net` heartbeat + dhcp_renews.

If networking breaks on the Pi, `/bin/netdiag` is already on the disk (pushed
pre-cutover) for a socket-liveness probe.

---

## Two small QEMU follow-ups (do before or after the HW pass)

* **forced-DEVD_FAIL / delayed-READY degrade gate.** The degrade TAIL (net off,
  boot continues) is shared with the verified no-`--net` route + the Stage-2
  bounded wait, but the "netd spawned but never READY" route is untested. Add a
  throwaway hook -- simplest is a `NETD_TEST_NO_READY` compile define in `netd.c`
  that skips the `DEVD_READY` Send -- build build-netd with it, boot, and assert:
  `netd: READY timeout/fail -> degrade`, boot reaches login, a child `socket()`
  gets `-ENOTSUP`. Then remove the define.

## Stage 4 (re-home + default ON, DESIGN_NETD s9)

* `/proc/genet` root-local rewrite that is UMAC/MDIO-free (the current dump issues
  MDIO + reads UMAC; post-split root can race netd or touch UMAC before SWINIT
  release). The netd-software half of the dump renders from the stats page.
* `NET_DIAG` (label 103) ops in `/bin/netdiag` (the crash op is the only one wired
  today). The fs thread must NEVER block-Call netd; a user process is sacrificial.
* explicit `SVC_PING` reply in netd (cosmetic; serverstats no longer pings it).
* flip `AIOS_NETD` default ON both targets; after one stable release, delete the
  in-root net path and retire the flag.

---

## Key refs

`docs/DESIGN_NETD.md` (s3 cap handoff / s6 stats page / s7 platforms / s8 boot
handshake / s9 stages / s10 failure); the `project_demono_netd` memory (the
file-level record); `feedback_genet_umac_swinit` (the SWINIT-before-UMAC halt +
the GENET real-MAC backlog); `feedback_pipe_shm_cache` (the cacheable-both stats
page rule); `project_proc_capacity` (the 30-pipeline ceiling); `feedback_flashfree_kernel`
(kernel8 swap). Test scripts (all take `AIOS_KERNEL=` / `AIOS_NETD_KERNEL=` now):
`netd_qemu_test.py` (the cutover + crash demo, 10/10), `net_socket_qemu_test.py`
(8/8), `ssh_qemu_test.py` (6/6), `smp_qemu_test.py` (30-pipeline).
