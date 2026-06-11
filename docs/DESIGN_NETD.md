# DESIGN: netd — de-monolithize the root task, net subsystem first

**Goal:** move the network subsystem (socket server + protocol stack + NIC driver) out of the
root task into its own MMU-isolated VSpace as a spawned process (`netd`), with zero client ABI
change, establishing the template for later extraction of display/blk.

**Motivation:** fault isolation (a net-stack crash must not take down the system), memory
protection, reduced pressure on the root task's single unlocked allocman/vka, and a reusable
driver-process pattern.

**Branch:** `demono/net-server-vspace`. **Status:** design complete, not implemented.

**Provenance:** synthesized from a multi-agent design review (7 codebase mappers, 3 competing
designs, 3-judge panel, 4 adversarial reviewers, completeness critic; 44 verified findings).
Every file:line below was checked against the tree at v0.4.190. The two blocker findings and
all major findings are folded into this text — sections marked **[FIX]** are corrections the
adversarial pass forced on the original winning design; do not "simplify" them away.

> Build discipline: builds run in the MAIN checkout (`~/Desktop/github_repos/AIOS`), never the
> worktree (its `projects/*` symlinks point at a `deps/` that only exists in the main checkout).
> Build BOTH `build-04` and `build-rpi4` for every stage. CPIO changes need `rm -rf` full
> rebuilds, then `python3 scripts/build_apps.py` (the clean nukes dash/sbase/sshd artifacts).

---

## 0. End state in one paragraph

`netd` is a **single-threaded**, MMU-isolated CPIO process (tty_server/auth_server template:
sel4utils spawn, 2^12 cnode, prio 200, raw seL4 IPC) that owns the entire net stack:
`src/servers/net_server.c`, `src/net/net_stack.c`, `net_tcp.c`, `net_dhcp.c`, and the platform
NIC driver refactored from a thread into a `plat_net_drain()` function. The NIC IRQ notification
is **self-bound by netd to its own TCB** at startup, so netd's blocking `seL4_Recv(net_ep)`
wakes on both client IPC and packet arrival — the same bound-notification trick
`boot_services.c:124-126` uses today. Root retains all allocator-touching duties: device frames,
the 128KB DMA untyped, notifications, the IRQHandler cap, and (RPi4) the VC-mailbox MAC query.
The client-visible endpoint is **the same `net_ep` object root allocates today**
(`boot_services.c:105-108`), so `exec_server.c:336-339`, `pipe_server.c:1467-1470` (child cap
copies, argv[5]), pipe_server's op-98 exit cleanup, and serverstats need **zero ABI change**.
The end state has **no cross-vspace shared mutable data structures** except one cacheable
single-writer stats page; device MMIO and DMA are attribute-identical non-cacheable dual
mappings.

---

## 1. Why staged, and which staging

Four stagings were evaluated:

- **A — move net_server first, driver stays in root: REJECTED.** The intermediate state needs a
  cross-vspace `net_rx_ring` shared frame (the exact v0.4.164 A72 cache-attribute bug class,
  QEMU-invisible) plus a new netd→root TX protocol (1518B frames > the 900B/120-MR IPC ceiling,
  since `net_tx_send` → `plat_net_tx` is a direct call at `net_stack.c:83`). Two throwaway
  shared-memory protocols, both deleted by the end state. Strictly riskier than the goal.
- **B — move the driver first: REJECTED.** Mirror image of A; all of its intermediate risk, and
  the crash-prone TCP/socket state machine (the actual isolation payoff) stays in root.
- **C — big-bang two-thread netd behind a flag: DOMINATED.** Safe intermediate states, but needs
  second-thread spawn machinery (root cannot know the driver entry point in netd's image without
  an ELF parse or handshake) and one giant cutover commit changing five coupled mechanisms at
  once — the un-bisectable-on-HW failure mode this project has learned to avoid.
- **D — RECOMMENDED: thread-merge in root → skeleton netd → flagged cutover → re-home.** Every
  intermediate state has no more cross-vspace sharing than the end state (Stages 0–2: none), and
  every stage boots and is independently verifiable on QEMU and HW.

---

## 2. Process architecture (end state)

### What moves into netd
`net_server.c` (modified), `net_stack.c`, `net_tcp.c`, `net_dhcp.c`, plus the **dev half** of the
platform driver: device-register init, TX, and `plat_net_drain()` from
`src/plat/qemu-virt/net_virtio.c` / `src/plat/rpi4/net_genet.c`.

### What stays in root
The **prov half** of the drivers (DMA/notification/IRQ allocation — `net_virtio.c:57-211`,
`net_genet.c:375-425,704-741` today), `boot_net_init.c` (becomes a `plat_net_prov()` call), a new
`src/boot/spawn_netd.c` (spawn + fault listener), the stripped mailbox MAC query (§7), and a
rewritten root-local read-only diag (§6).

**[FIX] prov/dev split mechanism: ONE file per platform driver, gated in place with
`#ifdef NETD_BUILD` / `#ifndef NETD_BUILD`.** A two-file split cannot be mechanical: the halves
share a dozen file statics (`genet_regs`, `genet_dma`, `rx_cons_idx`, `net_rx_irq_mode`, …) and
hand-retyping is exactly how the HW-verified orderings (SWINIT-before-UMAC `net_genet.c:611-620`,
EXT_RGMII_OOB_CTRL `:672-683`, INTRL2 unmask/Ack `:759-764`, UMAC_MAC1 low-half packing) get
broken. Verify with `git diff --ignore-all-space` showing only added `#ifdef` lines and zero
modified register-sequence lines; rely on link-time failure (root no longer references drain
symbols, netd never references vka) to catch wrong gating.

### Thread structure inside netd — single event loop

```
main(argc, argv):
  parse caps/vaddrs/paddr/mac/config from argv (decimal strings; tty_server precedent)
  seL4_TCB_BindNotification(SEL4UTILS_TCB_SLOT=5, NTFN_SLOT)   # self-bind, assert success
  plat_net_dev_init()              # device-register programming only (MMIO writes)
  seL4_Send(ctrl_ep, DEVD_READY)   # BEFORE DHCP -- see section 8
  net_dhcp_acquire()               # unchanged; dhcp_poll_rx() calls plat_net_drain() first
  gratuitous ARP, gateway ARP      # unchanged (net_server.c:384-387)
  loop:
    plat_net_drain()               # HW ring -> net_rx_ring (netd BSS); loops until empty (FIX below)
    process net_rx_ring            # unchanged (net_server.c:392-400)
    bump heartbeat in stats page   # section 6
    msg = seL4_Recv(net_ep, &badge)
    if label==0 && badge!=0: continue          # ntfn wake (badge 1=IRQ, 2=kick)
    dispatch labels 90-98, 103 (NET_DIAG), SVC_PING
```

**[FIX] drain contract — the GENET NAPI re-check must live INSIDE `plat_net_drain()`.** The
re-check at `net_genet.c:915-926` exists because a frame completing between the last
`RDMA_PROD_INDEX` read and the blanket `INTRL2_CLEAR` write gets its completion IRQ cleared
without being consumed; if the 16-slot HW ring fills in that window, no future completion can
fire and RX is dead. `plat_net_drain()` must loop: after INTRL2 clear + `seL4_IRQHandler_Ack`,
re-read `RDMA_PROD_INDEX` and re-drain until producer == consumer. QEMU mirror: after the ISR
ack (`net_virtio.c:324-326`), re-read `rx_used->idx` and re-drain if it advanced. Add a
concurrent ping-flood + bulk-pull burst test to the Stage 1 AND Stage 3 HW checklists.

Why single-threaded is safe: both net threads today are prio 200 pinned to core 0, so the driver
never preempts the server mid-handler anyway. Burst absorption drops from HW-ring(16)+SPSC(32)
to HW-ring(16) only while one IPC handler runs (~µs); there is no local TCP retransmit to lose
(`net_server.c:537-539` — peer retransmit covers it). Side benefit: the documented
`diag_tx_test` cross-thread TX race (`net_genet.c:952-954`) becomes structurally impossible.

GENET poll-mode RX (`net_rx_irq_mode=0`, the Yield loop at `net_genet.c:927-929`) is **not
supported** in the merged loop (it would starve IPC and violates the QEMU busy-poll ban).
**[FIX]** Because the IRQ path is being reworked in the same change that removes the poll-mode
escape hatch, Stage 3 ships a deterministic bring-up RX pump: a root-side optional periodic
kicker signaling the badge=2 copy (`/proc/genet.kick.N`, off by default), and an explicit HW
gate "IRQ wake proven via genet_irq_count climbing under ping flood, then pump disabled".
Also: dump both GENET DTB interrupt triples at prov time (one line) to finally pin the
INTRL2_0/INTRL2_1 routing question (`boot_dtb.c:199-213` parses only the first) before poll mode
is gone.

### Priorities, affinity, the core-0 invariant

netd: prio 200 (same as tty/auth), authority = root TCB, no SetAffinity. **The whole system runs
on core 0 today** — both targets are SMP=4 kernels (`settings-rpi4.cmake:30` KernelMaxNumNodes=4,
HW-verified), but TCB affinity inherits the creating core (`deps/kernel/src/object/objecttype.c:533`)
and every creator is pinned to core 0 (`boot_services.c:55`). This invariant is load-bearing for:
the cleanup-proxy SPSC queue, the stats-page single-writer discipline, and "root never preempts
netd mid-handler". State it in code next to spawn_netd, and assert/log netd's affinity at spawn.
If user processes are ever spread across cores, every one of those needs a cross-core
memory-order re-audit. netd holds no allocator, so it is *eligible* to move off core 0 later —
explicitly deferred.

netd never busy-polls between IPCs (blocking `seL4_Recv`, SLIRP constraint). Sanctioned
exception, unchanged: the DHCP startup Yield loop (`net_dhcp.c:216-225`), now calling
`plat_net_drain()` per iteration.

---

## 3. Capability handoff

Spawn sequence (new `spawn_netd()` in `src/boot/spawn_netd.c`, modeled on `spawn_util.c:12-39`
but custom: it needs map/copy steps between configure and resume, retains the global
`netd_proc` struct — NOT the tty/auth stack-local pattern — and needs >16-char argv buffers for
64-bit vaddrs):

`sel4utils_configure_process_custom` (cnode 12 bits, fresh vspace, CPIO ELF, prio 200, fault EP)
→ copy caps → map frames into `netd_proc.vspace` → start fault listener (Recv-ing BEFORE resume)
→ `sel4utils_spawn_process_v(..., resume=1)`.

| # | Cap / resource | Creator | How it reaches netd | Notes |
|---|---|---|---|---|
| 1 | `net_ep` (Endpoint) | root, unchanged (`boot_services.c:105-108`) | `sel4utils_copy_cap_to_process`, argv slot | Same object clients already hold. Root keeps `net_ep_cap` for exec/pipe/serverstats distribution. |
| 2 | IRQHandler | root (`simple_get_IRQ_handler`, moves into prov) | copy_cap_to_process, argv slot | netd Acks per drain. Root retains the original (never Acks) for respawn re-`SetNotification`. IRQHandler caps copy legally (deriveCap default case) and IRQ teardown happens only on FINAL deletion. |
| 3 | IRQ notification (unbadged original) | root, prov | **donated** (copy, AllRights), argv slot | **[FIX] netd SELF-BINDS** via `seL4_TCB_BindNotification(5, NTFN_SLOT)` and asserts the result (a silently failed bind = RX never wakes = the historical netconsole-stall signature). Root does NOT bind — bind fails if the TCB already has a bound ntfn or the ntfn is bound elsewhere, so exactly one binder. Self-bind is what a respawned netd must do anyway. |
| 3b | badge=1 mint of the ntfn | root | not donated; root re-issues `seL4_IRQHandler_SetNotification` with it (replacing the unbadged registration made at `net_virtio.c:199-200` / `net_genet.c:722-731`) | **Badge invariant: every Signal aimed at netd's bound ntfn goes through a badged copy** (1=IRQ, 2=root kick, 4 reserved). Signals through unbadged originals deliver badge 0 and silently bypass the `label==0 && badge!=0` wake test (`net_server.c:413`); the dispatch tail happens to tolerate it today, but treat badge-0 wakes as a loud-logged anomaly. |
| 3c | badge=2 mint | root | kept root-side | The diagnostic kick (`/proc/genet.kick`), and the bring-up RX pump. |
| 4 | Device MMIO frames (QEMU: virtio window pages; RPi4: 16 GENET pages) | root (`plat_virtio_probe.c:30-35` / `prealloc_rpi4_devices` + `map_dev`) | **frame-cap COPIES** (one frame cap = one mapping) mapped by root via `vspace_map_pages(&netd_proc.vspace, ..., 0 /*non-cacheable*/)`; vaddr via argv | **[FIX] Retention prerequisite:** today the caps are stack locals and discarded. Retain the full `vka_object_t` arrays (not just cptrs): `dev_genet_frame_objs[16]` in boot_device_map.c, `vio_frame_objs[4]` in the probe info, and `dma_ut` + `dma_caps[32]` in each driver's prov half. Record everything in a root-side `driver_handoff_t` (respawn re-map + the cookie-NULL explicit-unmap lesson on destroy). |
| 5 | DMA frames (32 × 4K from one size-17 untyped) | root, prov | originals stay root-mapped non-cacheable; copies mapped into netd non-cacheable; paddr via argv (`seL4_ARM_Page_GetAddress` on frame 0, contiguity unchanged) | **[FIX] RPi4: allocate the DMA untyped with a retry-for-low loop, limit `0x40000000`** (the HW-proven `xhci_dma_reserve` pattern, `src/usb/xhci.c:230-252`). The mailbox alias `|0xC0000000` truncates to 32 bits (`net_genet.c:525`) and the allocator provably hands out >3.9GB frames late in boot (`display_vc.c:275-279`). On failure: fail LOUD (net unavailable), never fall back silently to the fake MAC. Never claim anything in the GPU-region device untyped from net prov (forward-only watermark — that region belongs to the display tag buffer). Log `genet_dma_pa` once at prov. |
| 6 | SaveCaller reply slots ×24 | — (empty slots in netd's own cnode) | reserve by bumping `proc.cspace_next_free` past the range AFTER all donations | Replaces the 24 root-vka allocations at `net_server.c:361-370`. Mechanism verified: sel4utils mints the child's own cnode cap into slot 1 with the guard word, so `seL4_CNode_SaveCaller(1, slot, seL4_WordBits)` resolves exactly like root's `seL4_CapInitThreadCNode` form. |
| 7 | ctrl/fault EP | root, spawn_netd | `process_config_fault_endpoint` (slot 2); also used for DEVD_READY/FAIL | Dedicated root listener thread (prio 200, core 0), Recv-ing BEFORE resume. |
| 8 | TCB | sel4utils (slot 5) | automatic | needed for the self-bind. |

argv protocol (decimal strings, 24-char buffers): `[0]=net_ep slot, [1]=irq handler slot,
[2]=ntfn slot, [3]=MMIO vaddr, [4]=QEMU virtio slot index / RPi4 0, [5]=DMA vaddr, [6]=DMA paddr,
[7]=MAC packed 48-bit, [8..10]=cfg ip/gw/mask, [11]=flags (platform, irq-mode)`.

What netd deliberately does NOT get: vcmbox MMIO (MAC query is root-side, §7), serial_ep
(printf goes via `seL4_DebugPutChar`, like tty_server), any untyped/vka, pipe/fs/auth/crypto
endpoints. netd is a **pure leaf server** — grep-verified zero outbound `seL4_Call`/vfs use in
all four net sources.

---

## 4. Memory

| Region | Mapping | Attributes | Notes |
|---|---|---|---|
| Device MMIO | root (originals) + netd (copies) | non-cacheable both | attribute-identical per the A72 rule; root keeps its mapping for postmortem reads (§6 restrictions apply). |
| 128KB DMA | root + netd | non-cacheable both | preserves today's no-cache-maintenance driver code. Do not change DMA cacheability in this project. Root writes only pre-resume (provisioning) and the mailbox tag slice pre-spawn. |
| `net_rx_ring` (~48.7KB) | netd BSS only | cacheable | single-vspace AND single-thread; SPSC `dmb` publication kept as-is for diff minimization. |
| socket table, tx frames | netd BSS | cacheable | moves wholesale. |
| stats page (1 frame, §6) | root + netd | **cacheable both** | the HW-proven v0.4.165 pipe-SHM pattern; single writer (netd). |
| Stack | sel4utils default 64KB | — | identical to today's thread budget. |
| Heap | `morecore_area` static BSS: 6MB QEMU / **8MB RPi4** | — | see capacity below. |

**[FIX] Capacity is gated by the 8000-page allocman static pool, identical on BOTH targets
(`aios_root.c:41-42`), not by RAM.** CPIO spawns are fully eager; measured: tty_server memsz
6.14MB (build-04) / 8.15MB (build-rpi4). netd adds ~1,600 pool pages on QEMU but **~2,150 on
RPi4**, raising the RPi4 CPIO-resident baseline to ~6,350/8,000 before any user process — eroding
the v0.4.180/181 ≥30-pipeline ceiling worst on the Pi. Gates: run the vka_audit before/after
spawn measurement AND the ≥30-parallel-pipeline regression on **both** targets, not QEMU only.
Pre-planned mitigation (pick when the gate fires): (a) bump `ALLOCATOR_STATIC_POOL_SIZE` by
~2,500 pages (~10MB root BSS; re-verify boot both targets), and/or (b) patch vendored
libsel4muslcsys to make `morecore_area`/`morecore_size` weak so netd (which mallocs ~nothing)
declares a ~256KB heap, cutting its eager cost to ~250 pages.

Also note: the CPIO copy of netd stays resident in root memory alongside the spawned frames
(~300KB double-residency); spawn wall-time on the root init thread delays the serial console —
measure it (cntpct delta, printed once) at the Stage 2 gate.

VKA/device ordering (RPi4): unchanged — `prealloc_rpi4_devices` remains the only claimer of
peripheral MMIO; netd consumes frame-cap copies and never claims device untypeds; the
forward-only watermark is structurally un-violatable by this design (verified). The one trap is
the GPU-region tag-frame idea — rejected above.

---

## 5. Client ABI continuity

- **Endpoint object identity:** unchanged. Children before/during/after the move hold copies of
  the same object; `__wrap_main` argv[5] parsing and fork inheritance untouched.
- **Labels 90–98:** byte-identical wire protocol. Stage 0 factors the constants into
  `include/aios/net_proto.h` (values unchanged) consumed by `root_shared.h`,
  `posix_internal.h:151-158`, and netd — with a **compile-time static assert** that the client
  mirror matches, so dash/zsh/sshd (not rebuilt by ninja) can never silently skew.
- **SaveCaller across the move:** verified mechanism (§3 row 6); `#ifdef NETD_BUILD` macros
  select cnode (`1` vs `seL4_CapInitThreadCNode`) and slot source (static table vs vka), one
  `net_server.c` source for both build modes. Proven by the Stage-2 skeleton before any net
  logic depends on it.
- **[FIX] Reply-cap ground truth (non-MCS):** `finaliseCap` on a reply cap is a **no-op** for
  the blocked caller on this kernel — deleting a SaveCaller'd reply cap never wakes or unblocks
  anyone; only the caller's own cancelIPC (teardown) frees it. op-98's delete-without-reply is
  correct *only* because the owner is being reaped. No recovery step may assume deleting a
  saved reply cap wakes a caller.
- **op 98 (exit cleanup):** unchanged on the wire. **[FIX]** Replace the raw deferred Call with
  a **cleanup proxy**: `handle_child_fault` enqueues the pid into an SPSC ring of
  MAX_ACTIVE_PROCS (64) entries with `dmb` publication (the `net_rx_ring` pattern — the current
  single-int `pending_net_cleanup_pid` at `pipe_server.c:253` already drops a second exit
  today) + `seL4_Signal`; a sacrificial root proxy thread issues the blocking
  `seL4_Call(net_ep_cap, 98)`, re-checking `net_ep_cap` per dequeue. This protects pipe_server
  from both a **crashed** netd (cap zeroed by the fault listener) and a **hung** netd (only the
  expendable proxy parks). On overflow: drop, log, bump a `cleanup_lost` counter in the stats
  page. Document: a hung netd means no socket reclamation system-wide until it recovers.
- **owner_pid:** still caller-asserted MR3 (spoofable; same single-user threat model as the
  v0.4.190 finding). Follow-up (easier post-isolation): root-badged privileged copy of net_ep
  for the proxy/netdiag path, per-child badging later. One-line fix to take now: copy socket()'s
  lazy-PID-resolve into `aios_sys_connect` (`posix_net.c:229-233`) — a forked child calling
  connect() before getpid() currently registers owner_pid=0 (leak: no exit ever cleans it).
- **serverstats:** **[FIX] under AIOS_NETD do not SVC_PING netd at all.** The untimed
  `seL4_Call` (`serverstats.c:88`) is a permanent observability wedge against a hung netd (and
  its sequential loop freezes ALL rows). Feed the SRV_NET row from the stats-page heartbeat
  (§6); add a `dead` render state (`*ep_p==0` or heartbeat age > 3× period).

---

## 6. Cross-cutting

**Logging.** netd's printf reaches serial via muslc → `seL4_DebugPutChar` (KernelPrinting ON
both targets) — all runtime net logging keeps working. `AIOS_LOG_*` in netd-compiled sources
maps to printf via a shim header (the log ring is same-VSpace-only). The fault listener must
AIOS_LOG decoded fault PC/addr/type AND DEVD_FAIL errcodes, so early bring-up failures land in
`/proc/log` and survive the lossy mini-UART. Record for the future: any raw thread without
sel4runtime TLS must not call musl printf — use a `seL4_DebugPutChar` shim (moot while netd is
single-threaded; the trap if a driver thread ever returns).

**Stats page — `/proc/net`, platform-agnostic, ships in Stage 3 (not 4).** One cacheable-both
single-writer frame: heartbeat (per loop iteration), dev-init-done flag, DHCP state/counters,
ip/gw/mask/mac, socket-table occupancy (8 slots: state, owner_pid), `cleanup_lost`, last_err.
Rendered IPC-free by the fs thread, so it works when netd is wedged — it is the ONLY hung-netd
detector (non-MCS Calls cannot time out) and the only liveness instrument during the Stage-3 HW
soak. It exists on QEMU too (today QEMU has no live net node at all, and the boot-time IP
printfs move off the root console with the cutover).

**/proc/genet split. [FIX] The root-local "read-only" set must be UMAC/MDIO-free.** The current
dump issues MDIO transactions (write-then-poll on `UMAC_MDIO_CMD`, `net_genet.c:1009-1010`) and
reads `UMAC_CMD` — and ANY UMAC access while SWINIT is latched bus-errors → kernel halt
(v0.4.151 lesson, `net_genet.c:611-620`); post-split, root can race a live netd's MDIO engine or
touch UMAC before netd's dev-init released SWINIT. Therefore:
- Root-local (fs thread, dead-netd-safe): SYS/EXT/RBUF/RDMA/TDMA ring+ctrl, INTRL2 status,
  descriptor RAM, HW prod/cons indices — MMIO reads only, no UMAC, no MDIO, no PHY lines.
  This is a REWRITE of `genet_diag_cmd`, not a retention (half its current content is netd
  software state that no longer links root-side; that half renders from the stats page).
- netd-serialized via new `NET_DIAG` (label 103): `.poke`, `.mw`, `.tx`, `.reinit`,
  `.irqon/.irqoff`, `.mac`, PHY/MDIO reads — issued by a **userland `/bin/netdiag` disk tool**
  Calling net_ep directly. The fs thread must NEVER block-Call into netd (a hung netd would
  wedge every /proc read); a user process is sacrificial.
- `/proc/genet.kick` (root-local): Signal the badge=2 copy — the unwedge that works when netd's
  IRQ path is broken.
- Any future root-side UMAC need gates on the stats-page dev-init-done flag, default "not safe".

**Config delivery.** `boot_load_config` still parses `/etc/network.conf` into root globals
before spawn; values pass via argv[8..10]; netd owns its own `net_cfg_*` after that (root copies
go intentionally stale). Hostname: unused by net (verified).

**Process identity.** netd follows tty/auth: a `proc_add` row, NO `active_procs[]` entry —
invisible to ps/pidof/pkill/PIPE_SIGNAL, unkillable by operator tooling. This is the deliberate
"kernel furniture" decision; document it, have the fault listener flip the /proc row state, and
state the invariant that `thread_server` must never be pointed at netd (`create_child_thread`
dereferences `active_procs[i].proc`). An operator restart hook is part of the v2 respawn
follow-up.

**Shutdown/reboot.** No netd quiesce on reboot — acceptable (SoC reset) but now documented.
A reboot requested OVER the network (ssh/netconsole → `/bin/reboot`) reaches pipe_server, which
arms the watchdog and spins at prio 200/core 0 immediately after replying — netd never gets CPU
to flush the final TCP segments. Every soak script that reboots the Pi over the network must
tolerate lost output and a half-open peer connection. (Adjacent pre-existing bug, independent of
netd, flagged separately: `aios_system_reboot` calls `blk_cache_flush()` from the pipe_server
thread, violating the fs-thread-only invariant documented at `flush_server.c:12-21`.)

---

## 7. Platforms

### QEMU virtio-net
- Prov (root): consume `plat_virtio_get_info()`, DMA alloc/map/GetAddress, ntfn + IRQ `48+slot`
  handler + SetNotification(badge-1 mint). Probe must retain its 4 frame-cap `vka_object_t`s.
- netd dev half (`NETD_BUILD` sections of net_virtio.c): identity check, legacy init +
  QUEUE_PFN programming, RX descriptor arming, MAC read from config space (netd reads it
  itself; argv MAC unused on QEMU), `plat_net_tx`, `plat_net_drain` (with the used-idx re-check,
  §2). TX used-ring never reaped: pre-existing, moves as-is.
- **Isolation honesty: the MMU-isolation claim is RPi4-only.** qemu-boot.py attaches blk, blk
  (log), net in slots 0–2 — all in the FIRST 4K page of the virtio window (8 × 0x200 slots per
  page), so even single-page mapping hands netd write access to virtio-blk registers. Document
  the co-tenancy; do not pretend page math fixes it.

### RPi4 GENET
- Prov (root): GENET MMIO already pre-mapped ascending (retain the 16 frame objs); DMA alloc
  with the **retry-for-low <1GB loop** (§3 row 5); ntfn + DTB IRQ handler; **stripped mailbox
  MAC query**.
- **[FIX — was a guaranteed boot-halt] The prov-time MAC query must be a STRIPPED copy:**
  tag-buffer setup + `genet_mbox_call` (pure vcmbox MMIO) + MAC byte extraction ONLY. The
  current `read_mac_from_mailbox` (`net_genet.c:548-573`) also writes `UMAC_MAC0/MAC1` — UMAC
  registers — and at prov time nothing has cleared SWINIT since power-up, so those writes
  bus-error → external abort → kernel halt on every Pi boot (v0.4.151 lesson; QEMU-invisible).
  netd programs UMAC_MAC0/MAC1 from argv[7] in dev-init AFTER its own SWINIT release + UMAC
  reset (incl. the v0.4.161 MAC1 16-bit low-half packing), and keeps `read_mac_from_umac` as
  fallback when argv MAC is zero. Gate the UMAC writes with `#ifndef NETD_PROV` so the diff
  stays mechanical.
- netd dev half: SWINIT release / UMAC reset / RBUF / phy_init / RGMII OOB / dma_init /
  ring_init (MMIO-only sequence `net_genet.c:578-700` minus allocations), UMAC MAC programming
  from argv, `plat_net_tx`, `plat_net_drain` (with NAPI re-check), INTRL2 unmask. All HW poll
  loops keep cntpct real-time bounds — EL0 cntpct access is verified working in spawned
  processes (getty/netconsole/aios_top all use it).
- GENET DMA reach: descriptors carry addr_hi (>32-bit representable) and GENET is a direct SCB
  master (not behind the PCIe 3GB window), but AIOS has never exercised a high pool on HW —
  the retry-for-low allocation moots the question on the 4GB Pi (8GB Pi would add a ceiling).

---

## 8. Boot handshake and degrade semantics **[FIX — original design had a circular gate]**

Split the flag: **`net_hw_present`** (set by `plat_net_prov`; gates the netd spawn at
`boot_services.c:104`; the boot banner at `aios_root.c:399-400` reads THIS) vs
**`net_available` + published `net_ep_cap`** (set only on DEVD_READY; children spawned before
READY get `child_net=0` → `socket()=-ENOTSUP`, the existing guards already do this).

- netd sends **DEVD_READY (label 100) / DEVD_FAIL(errcode) (label 101)** on the ctrl/fault EP
  with `seL4_Send` (NOT Call — a Call would park netd BlockedOnReply where its bound
  notification cannot wake it), labels discriminated above the seL4 fault-label range (≤6 on
  aarch64; the `pipe_server.c:787` label-range trick).
- READY is sent **after device init, BEFORE `net_dhcp_acquire`** — device init is milliseconds;
  DHCP keeps today's concurrent, clients-queue-on-EP semantics. (If READY waited for DHCP, every
  slow-DHCP boot would either delay tty/getty by ~8s, or — with async publication — getty's
  fork-and-forget netconsole/sshd/sntp would capture `net_ep_cap==0` at spawn and come up
  netless forever, with no cap re-fetch. The second failure mode is QEMU-invisible: SLIRP
  answers DHCP in <1s; a real LAN needing one retransmit loses all remote access on a headless
  Pi.)
- Root waits with a **cntpct-bounded `seL4_NBRecv` + Yield poll** (~10s ceiling; non-MCS has no
  timed Recv; boot-time bounded poll is the sanctioned exception class). Timeout or DEVD_FAIL or
  fault ⇒ degrade: `net_available=0`, `net_ep_cap=0`, AIOS_LOG the reason, **continue boot** so
  tty/getty/serial come up for triage. A wedged netd init must never brick the boot (the tty/auth
  `if (error) return;` abort pattern is explicitly NOT copied — net is optional).
- The fault listener thread is Recv-ing before resume, so an early netd crash (pre-printf) is
  still loud.
- QEMU regression: a test that artificially delays netd's READY past getty's spawn and asserts
  the degrade path (children get -ENOTSUP; system boots normally).

---

## 9. Implementation stages

Each stage boots, ships, and is independently verifiable. QEMU first, then HW (flash-free:
`ninja -C build-rpi4 && python3 scripts/mkkernel8.py && cp disk/kernel8.img /Volumes/AIOSBOOT/`
— netd-in-CPIO is a root-task-image change). **HW rollback honesty: kernel8.img cannot ship over
the network (FAT write pending), so every Stage-3 HW iteration and rollback is a physical SD
shuffle.** Plan accordingly: QEMU exhaustively first.

### Stage 0 — zero-behavior-change refactors + hardening (in-root, 1–2 commits)
1. `include/aios/net_proto.h` (labels 90–103; name `NET_CLEANUP_PID=98`, retire the colliding
   unused `NET_GETINFO`); consumers switch with values unchanged; static assert vs the
   `posix_internal.h` client mirror.
2. **RST/close reply-slot poisoning fix** (pre-existing F4, will bite the Stage-3 soak): the RST
   path wakes/deletes only connect waiters (`net_server.c:108-127`) — make it wake parked recv
   with `-ECONNRESET` and parked accept with `-1`, deleting each cap; make all three SaveCaller
   sites delete-first and check the rc (reply `-EIO` on failure instead of parking a bogus cap);
   make `NET_CLOSE_SOCK` delete parked caps like op-98 does.
3. Cleanup-proxy (§5): SPSC pid ring + Signal in `handle_child_fault`, sacrificial proxy thread,
   `net_ep_cap` guard per dequeue.
4. serverstats: net row `enabled=1` unconditional (the `*ep_p==0` skip already exists); `dead`
   render state.
5. connect() lazy-PID-resolve one-liner (`posix_net.c:229-233`).
6. **New `scripts/net_socket_qemu_test.py`** (SLIRP hostfwd): outbound connect, ECONNREFUSED on
   RST, UDP send/recv, double-close, parked-recv + RST → -ECONNRESET, process-exit op-98 cleanup
   (open sockets in a child, exit, assert 8 fresh sockets allocatable). No automated coverage of
   any of this exists today (the 5 net scripts are all listen/accept-side).
   
Verify: both builds; QEMU boot + ssh + netconsole + the new socket suite; behavior otherwise
identical. Rollback: revert.

### Stage 1 — single-threaded net, still in root (1–2 commits)
Add `plat_net_drain()` to `net_hal.h`; extract each driver loop body into it (WITH the NAPI
re-check / used-idx re-check inside, §2); `net_server_fn` calls it at loop top;
`dhcp_poll_rx` calls it; drop the driver-thread start + `net_driver` proc row; bind
`net_drv_ntfn` to the server TCB; re-issue `SetNotification` with the badge=1 mint; delete
`net_srv_ntfn` and its Signal sites; re-point irqoff's unwedge Signal at the badge=2 copy.
Verify (QEMU): boot `--net`, DHCP banner, gateway-ping selftest, ssh suite, netconsole, the
Stage-0 socket suite, ping-flood-during-bulk-pull burst test. Verify (HW, flash-free): DHCP on
real LAN, ping 0% loss, netconsole `__get` pull throughput parity (~1MB/s), sshd, `/proc/genet`,
irqon/irqoff/kick, burst test. The badge≠0 wake path is NEW code — test IRQ-driven RX wake
explicitly on both platforms. Rollback: revert (in-root, no flag needed).

### Stage 2 — netd skeleton process (1 commit; full rebuild — CPIO change)
New `src/apps/netd.c`: argv parse; ntfn self-bind + assert; Recv loop; `SVC_PING→0`;
`NETD_BLOCK`/`NETD_KICK` selftests exercising child-cnode SaveCaller (slot via reserved range) +
Send + Delete; **the CNode_Move reply-sweep experiment** (root moves a saved reply cap out of
the skeleton's cnode into a root scratch slot and Sends it — this is the one kernel-semantics
bet in the crash-sweep story; prove or kill it here); `NETD_CRASH` op (null deref).
New `src/boot/spawn_netd.c` (custom spawn, global `netd_proc`, fault listener, bounded READY
wait, degrade-and-continue failure path). CMake: `AiosChildApp(netd)` +
`target_sources(netd PRIVATE ...)` + `target_compile_definitions(netd PRIVATE ${AIOS_PLATFORM}
NETD_BUILD)` (AiosChildApp compiles only `src/apps/<name>.c` and adds no PLAT define);
`option(AIOS_NETD)` default OFF; skeleton spawns on a TEST EP when ON.
**Build `/bin/netdiag` now** (works against the in-root path too) and push it to the Pi's disk
BEFORE any HW cutover — if Stage 3 breaks networking on the Pi, netdiag cannot be pushed
afterwards.
New `scripts/netd_qemu_test.py`: skeleton ping, block/kick, reply-sweep, crash → fault-listener
log line → system still serves a shell.
Gates: flag OFF = behavioral parity (boot-log diff modulo the version banner — bump-build.sh
runs every ninja, so bit-for-bit is unfalsifiable) + full QEMU suite. Flag ON: selftests, crash
containment, **vka_audit before/after spawn page count (both targets' configs)**, **boot-latency
cntpct delta**, HW smoke over netconsole.

### Stage 3 — cutover behind `AIOS_NETD` (the one real move)
`#ifdef NETD_BUILD`/`#ifndef NETD_BUILD` gating in the two driver files (prov vs dev); retention
of frame/DMA `vka_object_t`s in prov + probe + boot_device_map (`driver_handoff_t`); stripped
prov MAC query (§7); retry-for-low DMA (§3); `net_server.c` + `src/net/*.c` gain `NETD_BUILD`
mode (cnode/slot macros, AIOS_LOG→printf shim, `net_proto.h`, netd-local globals in
`src/apps/netd_shim.c`); netd main = dev-init + READY + DHCP + loop; **stats page + `/proc/net`
ship NOW** (it is the soak's liveness instrument); serverstats SRV_NET row reads it; the
`net_hw_present`/READY handshake (§8); root keeps compiling the in-root path — `boot_services.c`
selects by flag, so rollback is a reconfigure.
Gates (QEMU, flag ON): full Stage-1 list + socket suite; no-net boot (`qemu-boot.py` WITHOUT
`--net`: netd never spawns, zero delta); forced-DEVD_FAIL boot (degrade path); delayed-READY
test (§8); **crash-containment demo driven over the QEMU stdio serial console, not over netd**
(the demo destroys the channel that drives it): NETD_CRASH → fault log → shell alive, fs/pipe
alive, process exit completes (proxy guard), serverstats `dead`, new children get -ENOTSUP;
op-98 across the boundary; 8-socket exhaustion/recovery; ≥30-pipeline capacity regression.
Gates (HW, flag ON, SD shuffle): DHCP lease, ping, netconsole, sshd, `__get` pull parity,
root-local dump + NET_DIAG ops via netdiag, kick, **IRQ-proven gate** (genet_irq_count climbs
under ping flood with the pump disabled), burst test, multi-hour soak with periodic transfers
**with the USB keyboard + HDMI attached** (the Pi's real steady state — the xHCI poll thread
shares core 0) and an eMMC-class held-connection echo (~10min). Note: on a standalone Pi a netd
crash leaves HDMI+USB as the only console, and the HDMI console currently freezes on first
scroll (open bug, NEXT_20260610) — until that is fixed, netd-crash recovery on a standalone Pi
needs the serial cable; say so in the runbook.
Rollback: `-DAIOS_NETD=OFF` reconfigure (QEMU: minutes; HW: SD shuffle).

### Stage 4 — re-home + default ON
/proc/genet root-local rewrite (UMAC-free) + NET_DIAG ops in netdiag; `/proc` rows + fault
listener flips state; explicit SVC_PING reply; docs; flip `AIOS_NETD` default ON both targets.
After one stable release: delete the in-root net path; the flag retires.

---

## 10. Failure / restart semantics (v1: no respawn)

On netd fault, the listener: AIOS_LOGs decoded PC/addr/type; zeroes `net_ep_cap` +
`net_available`; flips the /proc row; **clears the IRQ** via root's retained IRQHandler
(`seL4_IRQHandler_Clear` or the quiesce helper — otherwise every subsequent device IRQ signals a
dead-but-undestroyed TCB and the line stays masked-and-unacked forever); runs the **reply-slot
sweep** if Stage 2 proved it (CNode_Move each of the 24 known slots to a root scratch slot, Send
-EIO) so parked recv/accept/connect callers get an error instead of hanging; optionally runs the
root-side device quiesce (virtio STATUS=0; GENET UMAC_CMD=0 + DMA disable — bounds wild
in-flight DMA to netd's own never-freed pool).

Honest blast radius: netconsole and sshd live in blocking accept inside netd — without the
sweep, **every netd crash permanently hangs both** until reboot (nobody kills them: getty is
fork-and-forget). With the sweep they get -EIO and exit/retry. Clients queued-but-not-received
on net_ep survive (the EP object is root-owned) and would be served by a respawned netd. Callers
already parked CANNOT be rescued by any cap deletion (§5 reply-cap truth) — only the sweep
(move-and-reply) or their own teardown frees them. Children spawned after the crash get
-ENOTSUP; children holding live caps to the receiver-less EP block forever on their next call
(process-fatal, system-safe). netd's ~6–8MB frames + cnode leak until reboot (accepted v1; or
`sel4utils_destroy_process`, which is the respawn prerequisite anyway — its TCB finalisation
also auto-unbinds the notification).

Respawn (v2) needs: the retained `driver_handoff_t` (re-copy/re-map frames, explicit unmap of
netd copies before destroy — cookie-NULL lesson), re-`SetNotification` on the retained
IRQHandler, device quiesce/reinit from unknown state, restore `net_ep_cap`, accept all socket
state lost (old fds dead; netconsole/sshd need a service-respawn story — same follow-up), and
the operator restart hook (netd is outside `active_procs`).

---

## 11. Non-goals (v1) and follow-ups

Non-goals: respawn (requirements in §10); badged net_ep / kernel-derived caller identity; SHM
bulk-data path (labels 99–102 stay dormant; 900B MR ceiling unchanged — note netd holds no Frame
caps, so a future cacheable shared path must either donate Frame caps or use per-page
`seL4_ARM_VSpace_Clean_Data` via slot 3, one page per call); socket POLLIN for ppoll (verified
orthogonal — nothing in readiness polling touches sockets today; a netd-side NET_POLL label is
the natural shape); TCP retransmit; QEMU TX used-ring reaping; GENET poll-mode RX; moving netd
off core 0; per-app morecore shrink; multi-session sshd; moving display/blk (the template:
prov/dev `#ifdef` split, root-retained handoff struct, single-threaded bound-ntfn server,
reserved child-cnode SaveCaller slots, ctrl-EP READY/FAIL + fault listener, flag-gated cutover).

Follow-ups, priority order: (1) netd respawn manager + getty service respawn; (2) socket sid
**generation tags** (dup/F_DUPFD/fork + double-close today frees a recycled slot owned by
another process — `posix_misc.c:243-250`, `posix_file.c:759-766`, `net_server.c:637-654`; pack a
generation counter into the opaque sid, validate per op); (3) badged net_ep; (4) /proc/log IPC
for isolated processes; (5) DHCP lease renewal; (6) per-app morecore sizing.

## 12. Verified ground truth (rely on these; citations checked)

IRQHandler caps copy via CNode ops (deriveCap default returns the cap; teardown only on FINAL
deletion). TCB destroy auto-unbinds a bound notification. BindNotification needs a CanReceive
ntfn cap in the INVOKER's cspace and fails if either side is already bound — exactly one binder.
Badge delivered on a ntfn wake is the SIGNALING cap's badge (unbadged Signal ⇒ badge 0).
Non-MCS reply-cap deletion is a no-op for the parked caller. Child-cnode SaveCaller via the
guarded slot-1 cnode cap works with plain slot numbers. EL0 cntpct/cntfrq access works in
spawned processes (proven by getty/netconsole). The RPi4 device-untyped watermark cannot be
tripped by cap-copy donation. Endpoint message queues survive the receiver's death. Affinity
inherits the creating core. fork resets the cached PID, so child-created sockets get correct
owner_pid (the connect() corner aside). net code calls no other server and reads no files at
runtime. xHCI/UART/net IRQ-notification infrastructure is fully disjoint (no badge collisions).
sshd/netconsole do not close inherited socket fds, so they do not trigger the sid-aliasing bug.

## 13. Residual risks consciously accepted

QEMU netd sees virtio-blk MMIO (same 4K page; isolation claim is RPi4-only). Root retains
writable MMIO/DMA mappings (postmortem capability kept: isolation is netd-can't-hurt-root, not
the reverse). ~6–8MB eager morecore per CPIO app against the 8000-page pool (mitigations
pre-planned, §4). GENET INTRL2_1 unprobed (one-line DTB dump in Stage 3 to close it). The
mailbox <1GB constraint now enforced at prov (retry-for-low) instead of assumed. HW rollback is
a physical SD shuffle until flash-over-network lands.
