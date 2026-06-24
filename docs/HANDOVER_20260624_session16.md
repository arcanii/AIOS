# HANDOVER -- session 16 (2026-06-24)

Staying on seL4 (stall mitigated, not cured -- [[feedback_stall_open_concern]]). 10 commits on
main this session; Bryan pushes. Board left on build-rpi4 v0.4.300 build 2953 (default POLL,
keyboard works). Big session: bulk-write SHIPPED+HW-validated, netconsole socket fix, a strategic
seL4-vs-Linux review, and a two-lead USB-keyboard-stall investigation (lead #1 dead, lead #3 mid-flight).

## 1. PRIMARY TASK DONE + HW-VALIDATED: SHM-ring bulk file write (PIPE_PWRITE_BULK, v0.4.298)
Tier-1 #2. Large writes packed ~800B/FS_PWRITE into MRs; now PIPE_PWRITE_BULK page-bounces the
caller's buffer into pipe_server (file_bounce_map -- the SAME cacheable bounce PIPE_MSYNC uses) +
vfs_pwrites it, ~64KB/IPC. Wired into write / `>` redirect / pwrite64 / **writev** (the stdio path
tcc output rides). Default OFF (/proc/pwritebulk); non-root /etc+/bin authz replicated. Commits
b39e945 (feature) + e4364e7 (writev) + b7b42f9 + 9e83dfa (same-core soak rework).
- QEMU: serial 5/5, ssh_pty 7/7, smp 4/5, **pwritebulk_qemu_test 8/8** (byte-exact OFF==ON sha).
- **HW (full SD reflash, balenaEtcher, build 2933): same-core soak 7/7** (bulkwrite 12/12 + cp/sha
  byte-exact; calls=168 bytes=10637184 mapfail=0 denied=0); **tcc speedup CONFIRMED** (output
  211329B in 8 bulk IPCs vs 265 legacy = ~33x; binary runs rc=42). Cross-core coherency INHERITED
  (coresched.1 wedges fork on this timer-masked build -- pre-existing; needed a power-cycle).
  Detail: [[project_pwrite_bulk]]. Tooling: scripts/pwritebulk_{qemu_test,hw_soak,tcc_speedup}.py.

## 2. netconsole-under-load: MAX_NET_SOCKETS 8 -> 16 (v0.4.299, commit 605771a)
The post-soak `dash: Cannot fork` wedge. DISPROVED the "netconsole never reaps" theory (run_command
waitpids; slots free at fault-time; scripts/netcon_leak_repro.py shows NO per-cmd/per-conn leak in
QEMU). Real cause = the 8 socket slots + GRACEFUL TCP close (holds a slot up to 10s) churning under
load. Bumped to 16 (NETD_REPLY_SLOTS 24->48 lockstep). Non-regressive; both HW trees build. Detail:
[[project_netconsole]].

## 3. Strategic review (kernel state + seL4 vs Linux + the keyboard)
EVAL_20260623 + DR_20260623 + web check: seL4 SMP = RESILIENCE not throughput (BKL; 2.6x slower
distributed); RPi4-SMP is UNVERIFIED (seL4 proofs are uniprocessor + RPi4 not a verified platform)
so the seL4 thesis isn't realized here; the stall is AIOS-specific not "seL4 unstable on Pi". The
USB keyboard stall is IMMUNE to KernelMaxNumNodes 1 vs 4 -> a DRIVER fix, NOT an SMP redesign.
Decision (Bryan): pursue the driver fix. Detail: [[project_usb_kbd_dma_stall]].

## 4. USB keyboard DMA-stall -- two leads
- **Lead #1 (cacheable DMA) = DEAD END (HW-tested).** Mapped the xHCI DMA pool cacheable + manual
  seL4 clean/invalidate (~17 sites, AIOS_XHCI_DMA_CACHEABLE, default OFF). HW A/B: flag-ON breaks
  the event handshake (evt_deq STUCK at 0, kbd_ok=0, nothing enumerates); flag-OFF same board =
  kbd_ok=1 evt_deq=42. The non-cacheable mapping is DELIBERATE/correct; manual invalidate is
  insufficient for the brcmstb PCIe inbound path. Commits 42641f0 + cf9e310 (kept default-OFF as a
  documented dead end). GREAT diagnostic: /proc/xhci over netconsole (kbd_ok/evt_deq/key_events/
  int_errs/USBSTS). pi_flash kernel-swap = fast reversible A/B.
- **Lead #3 (MSI vs masked polling) = IMPLEMENTED + BINDS, but MSI NOT FIRING yet (mid-flight).**
  docs/NEXT_20260624_xhci_msi.md (turnkey recipe). plat_pcie_xhci_irq->180 (GIC_SPI 148+32);
  plat_pcie_xhci_msi_enable programs brcmstb legacy MSI (CPU INTR2 0x4300 bits[31:24], target
  0xfffffffc, data 0xfff86540) + walks/enables the VL805 MSI cap; plat_pcie_xhci_msi_ack clears the
  RC bit; xhci_irq_enable arms it on /proc/xhci.irq.1; block path acks before the GIC Ack. Default
  POLL (no compile flag -- the /proc toggle IS the gate). Commits 91246fa (plan) + e548719 (impl) +
  fe951ff (HW result). HW (build 2953): keyboard enumerates poll (kbd_ok=1); **seL4 IRQ 180 BINDS
  (irq: bound=1 num=180)**; armed + typed -> **count=0 key_events=0 evt_deq stuck** (driver blocked
  in seL4_Wait, never woken; reboot recovers to poll). seL4 side fine; the MSI SOURCE is not
  delivering.

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue enriching AIOS (STAYING on seL4 -- the ~32.4s idle-teardown stall is MITIGATED not cured,
MAJOR OPEN CONCERN, [[feedback_stall_open_concern]]). READ FIRST: docs/HANDOVER_20260624_session16.md,
then memory [[project_usb_kbd_dma_stall]] + [[project_pwrite_bulk]] + [[project_netconsole]] +
docs/NEXT_20260624_xhci_msi.md.

DONE this session: SHM-ring bulk file write (PIPE_PWRITE_BULK) -- QEMU 8/8 + HW-validated (soak 7/7,
tcc ~33x fewer IPCs), default-OFF, commits b39e945+e4364e7+b7b42f9+9e83dfa. netconsole MAX_NET_SOCKETS
8->16 (605771a). USB-keyboard strategic review (it's a driver fix, not SMP). Keyboard lead #1
(cacheable DMA) HW-tested -> DEAD END (42641f0+cf9e310). Board on v0.4.300 build 2953 (poll, keyboard works).

PRIMARY TASK -- finish lead #3 (xHCI MSI): it is IMPLEMENTED + the seL4 IRQ 180 BINDS, but the MSI is
not firing on HW (count=0 on keypress; driver blocks). DEBUG OVER NETCONSOLE (no serial): add to
/proc/xhci a read of the brcmstb RC INTR2 STATUS (rd(0x4300)) + the read-back VL805 MSI cap
(control/addr/data) + an INTR2-bit24-seen counter. Flash build-rpi4 (pi_flash --build, kernel-only,
reversible), arm /proc/xhci.irq.1, type, read /proc/xhci -> SPLIT the failure: INTR2 bit24 SETS on
keypress => the GIC-SPI/IRQ-180 mapping is wrong (confirm 148 is the MSI SPI, try alternatives);
NEVER sets => the VL805 Message Data is wrong (my BRCM_MSI_DATA_MATCH=0x6540 guess -- recheck Linux
brcm_msi_compose_msg) or the cap was not enabled / target addr wrong. Then fix + re-test: count climbs
+ keyboard interrupt-driven (key_events, int_errs=0), then the TLBI load test -> do the ~10.8s quanta
stop? That is the lead-#3 verdict. Default stays POLL; arm only via /proc/xhci.irq.1 (safe, runtime
A/B). If MSI also does not stop the quanta, the 32ms poll clamp (v0.4.221) stays the interim. Commit
per step on main; Bryan pushes. Be gentle on HW (stall + a stuck-IRQ keyboard need a power-cycle;
Bryan is near the board).

ALTERNATIVES if Bryan prefers: V3D texturing; ext3 journaling; seam-extraction refactor; zsh Phase-3
job control. Toolchain target = musl + tcc.
