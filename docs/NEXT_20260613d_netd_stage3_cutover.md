# NEXT: session seed -- 2026-06-13d (netd Stage 3 CUTOVER, 3b/3c/3d)

Paste-this-style brief for a fresh session. Read `HANDOVER.md` (top + the DONE
section "netd Stage 3 FOUNDATION") and the memory `project_demono_netd` first --
that memory holds the full file-level runway; this doc is the orientation + plan.

---

## Paste-this brief

AIOS (research microkernel OS on seL4, repo `~/Desktop/github_repos/AIOS`, branch
`main`, at **v0.4.236**, **ahead-1 of origin** -- Bryan pushes via GitHub Desktop;
commit only when asked / when told to keep going, never amend / force-push, no
apostrophes in C comments). Develop + verify on QEMU; flash the real RPi4 only at
milestones.

Last session landed the **netd Stage 3 FOUNDATION** -- the compile/link/gating work
for moving the net stack out of the root task into the isolated `netd` process, in 5
reviewable **flag-OFF-inert** commits (`ec20d24`..`7cbee78`), each verified flag-OFF
(QEMU socket suite 8/8) + flag-ON (`netd_qemu_test` 9/9):
- `ec20d24` v0.4.236: reverted the failed GENET MAC fixes (clears the ground).
- `815659b` + `4f9b84c`: net_virtio.c + net_genet.c **prov/dev split** behind two
  compile defines (prov `#ifndef NETD_BUILD`, dev `#ifndef NETD_PROV`); new
  `plat_net_prov()` + `plat_net_dev_attach()`; net_genet got the stripped prov MAC
  query; the HW-verified GENET register sequence is byte-identical.
- `e3a5418`: the net stack (net_server.c + src/net/*.c + driver dev half) now
  **compiles AND links into netd** under `NETD_BUILD` -- the isolation proof; new
  `src/apps/netd_shim.c` defines the root-owned net globals for netd; net_server.c
  SaveCaller cnode is the `NET_REPLY_CNODE` macro. netd `main` is STILL the skeleton.
- `7cbee78`: `driver_handoff_t` (`include/aios/netd_handoff.h`) + prov frame/DMA
  retention; `plat_net_prov(driver_handoff_t*)` fills it (still uncalled = inert).

**This session: do the behavioral CUTOVER (3b/3c/3d).** It is ONE atomic arc -- nothing
boots-and-serves until spawn + netd-main + boot-select all land. QEMU-developable now
via `build-netd` (the AIOS_NETD=ON build); the first real netd boot WILL need a few
build-netd debug iterations. Read **`DESIGN_NETD.md` s3 (cap handoff) + s8 (boot
handshake) + s9 Stage 3 + s10 (failure/sweep)**.

---

## The plan (3b -> 3c -> 3d)

### 3b -- boot cutover (the core; flag-ON)
1. **spawn_netd.c** (extend the Stage-2 skeleton spawn): call `plat_net_prov(&ho)`
   FIRST (root-side, before spawn), then:
   - donate the REAL `net_ep` (the client endpoint root already owns,
     `boot_services.c:105-108`) -- this is what makes the ABI unchanged.
   - donate `ho.irq_handler` + `ho.irq_ntfn` (unbadged; netd self-binds it to its
     TCB). Root re-issues `seL4_IRQHandler_SetNotification` with a badge-1 mint and
     keeps a badge-2 kick mint (the prov `genet_irq_bind`/`net_irq_bind` already mint
     these -- thread them through the handoff or re-mint root-side).
   - **copy + map the MMIO frame caps** (`ho.mmio_frames[0..mmio_nframes)`) into
     `netd_proc.vspace` at `ho.mmio_vaddr`, non-cacheable, via
     `vspace_map_pages(&netd_proc.vspace, ..., 0)` -- per `sel4utils_copy_cap_to_process`
     for the caps then map. Same for the **32 DMA frame caps** at `ho.dma_vaddr`,
     non-cacheable.
   - **reserve 24 SaveCaller slots** (`3*MAX_NET_SOCKETS`) past
     `netd_proc.cspace_next_free` (the skeleton reserves `NETD_RESERVED_SLOTS`=4 --
     bump to 24); pass the base via argv.
   - **argv (s3 protocol)**: `[0]=net_ep slot, [1]=irq handler slot, [2]=ntfn slot,
     [3]=mmio vaddr, [4]=slot index, [5]=dma vaddr, [6]=dma paddr, [7]=mac packed,
     [8..10]=cfg ip/gw/mask, [11]=flags(platform,irq-mode), [12]=reply-slot base`.
   - **s8 handshake**: split `net_hw_present` (set by plat_net_prov; gates the spawn)
     from `net_available`/`net_ep_cap` (published only on DEVD_READY). netd sends
     DEVD_READY (label 100, **Send not Call**) AFTER dev-init, BEFORE DHCP. Root waits
     with the cntpct-bounded NBRecv+Yield poll (~10s, the skeleton already has it) ->
     publish `net_ep_cap` + `net_available=1`; on timeout/FAIL **degrade-and-continue**
     (net off, boot proceeds). Drop the skeleton selftest client.
   - retry-for-low DMA: land it now in `net_genet dma_init` (the marked TODO,
     xhci_dma_reserve pattern, limit 0x40000000, fail LOUD) -- it is the netd-prov DMA.
2. **netd.c** (replace the skeleton main): parse argv; `seL4_TCB_BindNotification(5,
   ntfn)` (self-bind, assert); `plat_net_dev_attach(mmio_vaddr, slot, dma_vaddr,
   dma_paddr, irq_handler, mac)`; `plat_net_init()` (dev-init -- the SWINIT/UMAC/RBUF/
   RGMII/ring/INTRL2 sequence); set `net_available=1`; set `netd_reply_slot_base` (the
   shim global) from argv; **DEVD_READY (Send)**; `net_dhcp_acquire()`; then call
   `net_server_fn((void*)net_ep)` (the merged loop already does drain + ring + dispatch
   + SVC_PING). NOTE the attach functions currently set the per-device base directly --
   confirm QEMU slot math (net_vio_priv = mmio_vaddr + slot*0x200) vs RPi4
   (genet_regs = mmio_vaddr); adjust `plat_net_dev_attach` if needed.
3. **boot_services.c**: `#ifdef AIOS_NETD` -> `plat_net_prov` + `spawn_netd` (do NOT
   start the in-root net_server thread, do NOT call boot_net_init); `#else` -> the
   current `boot_net_init()` + in-root net_server thread. Root KEEPS compiling both
   paths (the in-root path is the flag-OFF default + the rollback). Also: serverstats
   must NOT SVC_PING netd (wire it to the 3c stats page).

### 3c -- stats page + /proc/net (the soak liveness instrument, s6)
One cacheable-both single-writer frame: root allocs + maps it, copies the cap to netd
(argv), netd writes heartbeat + dhcp/ip/mac/socket-occupancy each loop iter. `/proc/net`
renders it IPC-free in the fs thread (works when netd is wedged -- the ONLY hung-netd
detector, non-MCS Calls cannot time out). serverstats SRV_NET row reads it (heartbeat
age -> `dead`). Map BOTH ends with the SAME memory type (cacheable) -- the v0.4.165
pipe-SHM cache-coherency rule ([[feedback-pipe-shm-cache]]).

### 3d -- crash recovery (s10)
In the fault listener (already in spawn_netd.c): on a netd fault, run the **reply-sweep**
(CNode_Move each of the 24 reserved reply slots out to a root scratch slot + Send -EIO --
the mechanism PROVEN in Stage 2) so parked recv/accept/connect callers (sshd,
netconsole) get an error instead of hanging forever; `seL4_IRQHandler_Clear` the device
IRQ; zero `net_ep_cap` + `net_available`; flip the /proc row.

---

## Gates (run on QEMU, flag-ON, before any HW)

- `build-netd` boots, DHCP lease, the socket suite (`net_socket_qemu_test.py`), ssh,
  netconsole all green AGAINST the isolated netd.
- no-`--net` boot: netd never spawns, ZERO delta vs flag-OFF.
- forced-DEVD_FAIL + delayed-READY: the degrade path (children get -ENOTSUP, boot ok).
- crash-containment demo driven over the **QEMU stdio serial console, NOT netd** (the
  demo destroys the channel): NETD_CRASH -> fault log -> shell/fs/pipe alive ->
  serverstats `dead` -> reply-sweep wakes parked callers -> new children -ENOTSUP.
- op-98 cleanup across the boundary; 8-socket exhaustion/recovery; **>=30-pipeline
  capacity regression + vka_audit** (the netd CPIO eats ~2150 RPi4 pool pages, s4).

Then the **Step-4 HW pass** (SD shuffle / flash-free kernel8 swap): capture the SERIAL
boot log (do NOT hammer the v1 netconsole, it wedges -- power-cycle to recover); the
headline check (DHCP + ping) is passive. Verify retry-for-low DMA there.

## State to verify before trusting (point-in-time)

- Repo `main` v0.4.236, clean tree, ahead-1: `7cbee78` (3a handoff plumbing). Origin at
  `e3a5418`. Both trees build; `build-netd` builds + `netd_qemu_test` 9/9.
- The cutover is ALL `#ifdef AIOS_NETD` / `NETD_BUILD` -- flag-OFF stays byte-identical,
  so a broken WIP cutover never regresses the production path. Commit 3b only once
  build-netd boots+serves.
- Key refs: `docs/DESIGN_NETD.md` (s3/s8/s9/s10), memory `project_demono_netd` (the
  file-level runway), `feedback_pipe_shm_cache` (stats page), `feedback_genet_umac_swinit`
  (the deferred GENET MAC + retry-for-low DMA), `project_proc_capacity` (the >=30 gate).
