# DESIGN: V3D 4.2 Driver Implementation (RPi4 hardware 3D)

Status: implementation design, ready to build. Supersedes the bring-up/memory/shader
sections of `DESIGN_RPI4_3D.md` (research doc, 2026-06-05) wherever they conflict --
see the Corrections table below. Branch: `design/rpi4-v3d-driver`. Date: 2026-06-11.

Scope: Option A from the research doc (minimal register-level V3D driver in the root
task; no Mesa port). Deliverable: GPU-rendered clear -> triangle -> spinning cube on
the physical HDMI monitor, drivable over netconsole.

All AIOS file:line refs verified at HEAD `34490d6`.

## 0. Executive summary

- The driver lives **in the root task** (like xHCI/GENET), with all V3D mutation
  executing **on the existing display_server thread** via new `DISP_V3D_*` IPC labels.
  No new server, no new endpoint, no new thread -- single-writer FB arbitration and
  SMP safety by construction.
- **Power-on is three passworded MMIO writes (PM_GRAFX + RPiVid ASB), not mailbox
  tags.** The research doc's mailbox plan (0x00028001 / ENABLE_QPU) is wrong for Pi 4;
  the HW-proven sequence is the Linux `bcm2835-power.c` BCM2711 path, validated
  bare-metal by Random06457 and gyabo. Success probe: `V3D_HUB_IDENT0` flips from the
  bus poison `0xDEADBEEF` to `0x04443356`.
- **There is no NV-shader shortcut on V3D 4.2.** The vc4 "pre-transformed vertices +
  tiny fragment shader" trick does not exist; v42 requires the GL Shader State path
  with three QPU programs (fragment + vertex + coordinate). We borrow Random06457's
  three proven, annotated shader blobs verbatim.
- **No <1GB memory constraint for V3D.** The V3D MMU addresses 36-40 bits of physical
  RAM; PTE = raw `phys >> 12`, no 0xC0000000 alias. The <1GB rule survives only for
  VC-mailbox allocations (firmware FB, tag buffers). GPU memory = one 8MB contiguous
  untyped anywhere in RAM, non-cacheable, bump-allocated.
- **Mesa's simulator is NOT usable** (its core is the Broadcom NDA-only
  `v3d_hw`/simpenrose library). Pre-HW validation = host-side golden control-list
  diff against Random06457's emitters + macoy's open MIT `v3d-toolkit` decoder.
  QEMU has no V3D model at all: QEMU validates plumbing only; first light is HW-only.
- Completion waits are **deadline-bounded polls on BFC/RFC** (never untimed
  `seL4_Wait`); a hung GPU dumps CT*CA/MMU_VIO state and takes a full reset path,
  returning the display_server handler in <=350 ms. This is the standing defense
  against the display->tty->USB wedge class.
- Estimated effort to the spinning cube: **~4-6 weeks** (Phase 3, the GL Shader
  State / CL packing bring-up, dominates).

## 1. Corrections to DESIGN_RPI4_3D.md (verified 2026-06-11)

Adversarially fact-checked against raspberrypi/linux rpi-6.6.y + torvalds master
(`v3d_regs.h`, `v3d_mmu.c`, `v3d_sched.c`, `bcm2835-power.c`, `bcm2711.dtsi`), Mesa
main (`v3d_packet.xml`, `v3dx_rcl.c`, `v3d_util.c`), the BCM2711 datasheet, QEMU
source, and the working bare-metal repos.

| Research-doc claim | Verdict | Corrected fact |
|---|---|---|
| MMIO: hub 0xFEC00000 / core0 0xFEC04000, 0x4000 each | CONFIRMED | bcm2711.dtsi `gpu@7ec00000`; scb bus alias +0x80000000 |
| IRQ GIC SPI 74 level-high | CONFIRMED | seL4 IRQ = 74+32 = **106** |
| Register offsets (IDENT, MMU, CLE, INT) | CONFIRMED | All match Linux `v3d_regs.h`; see additions below |
| Power via mailbox tag 0x00028001, domain 10, QPU-enable 0x00030012 | **WRONG** | 0x00028001 has no V3D device ID; ENABLE_QPU is vc4-era (leaves IDENT0=0xDEADBEEF on Pi 4); firmware SET_DOMAIN_STATE would be 0x00038030 domain **11** but is unproven bare-metal. Proven path: **PM_GRAFX 0xFE10010C \|= 0x5A000000\|BIT(6)** (V3DRSTN deassert, PM password), ~2us delay, then clear REQ_STOP(bit0) in RPiVid ASB **V3D_M_CTRL 0xFEC1100C** and **V3D_S_CTRL 0xFEC11008** (each write OR'd with 0x5A000000), polling ACK(bit1) clear. "GRAFX_V3D index 10" conflated two bindings: BCM2835_POWER_DOMAIN_GRAFX_V3D=1 (the &pm block the v3d node actually uses) vs RPI_POWER_DOMAIN_V3D=10 (firmware binding, unused by v3d on Pi 4) |
| Clock id 5 = V3D, ~500 MHz | CONFIRMED | But optional: both working bare-metal examples set **no clock** and render fine (firmware default 500 MHz). SET_CLOCK_RATE(0x00038002){5,500MHz,0} is belt-and-suspenders |
| All GPU pages must be <1GB | **WRONG** | V3D MMU PTE PFN reaches 36-40 bit physical (Linux asserts <64GB); scb bus has no dma-ranges limit (identity-mapped); the PCIe 3GB bug does not apply (V3D is not behind PCIe). Page table base register takes any `phys>>12`. <1GB stays true ONLY for VC-mailbox/VPU allocations |
| NV Shader State record + 12.4 fixed-point vertices | **WRONG** | vc4-only (`VC4_PACKET_NV_SHADER_STATE=65`). v42 = **GL Shader State (code 64)** -> 36-byte, 32-byte-aligned GL Shader State Record with THREE QPU shaders (frag+vertex+coord) + 16-byte attribute records. Screen X/Y leave the VS as integer **1/256-pixel** units (not 12.4) |
| CT0QMA/QMS = binner overflow memory | **WRONG (mislabeled)** | QMA/QMS (0x170/0x174) = INITIAL tile-alloc pool, set per job. True overflow = `V3D_PTB_BPOA/BPOS` (0x308/0x30c) fed on the OUTOMEM interrupt (BIT(2)) |
| Tile-state address rides in the bin CL config packet | **WRONG for v42** | v33-only. On v42 the TILE_BINNING_MODE_CFG packet (code 120) has NO address fields; tile-state base goes in register **CT0QTS (0x15c) \| ENABLE(bit 1)** before the kick |
| Render CL = per-tile {coords, branch, store} loop | **WRONG (vc4 shape)** | v42 is supertile-based: TRMC(121) sub-packets (Common first, ZS Clear last) -> 126/123/122 -> dummy clear tile (+ a second dummy store tile, Random06457 race workaround) -> ONE generic tile list (125/26/56/54/21/29/27/18) registered via packet 20 -> one SUPERTILE_COORDINATES(23) per supertile -> END_OF_RENDERING(13) |
| Validate CLs with Mesa's simulator | **NOT ACTIONABLE** | Simulator core = Broadcom NDA `v3d_hw` (simpenrose), Linux-only, unbuildable here. Use: Mesa `v3d_packet.xml` (min_ver=42) as spec, Mesa CLIF decoder (`v3dx_dump.c`) + macoy's MIT **v3d-toolkit** (`v3dSimulator.h`/`v3dInspector.h`, desktop-runnable) for validation. PiV3D itself has no retrievable source -- Random06457 is the only complete open Pi-4 code reference |
| QEMU as dev environment | CONFIRMED unusable for V3D | No V3D model anywhere in QEMU; raspi4b inherits a 4KB read-as-zero unimp stub at 0xFEC00000. HW-only for GPU behavior |

Registers the research doc missed, all load-bearing: `CT0QTS` 0x15c (mandatory),
`V3D_MMU_ILLEGAL_ADDR` 0x1230 (scratch-page redirect for stray GPU accesses; Linux
always programs it), `V3D_MMU_VIO_ID` 0x122c, `V3D_MMU_DEBUG_INFO` 0x1238 (PA/VA
width), `CT0CA/CT1CA` 0x110/0x114 (current execution address -- pinpoints a hung
packet), `BFC/RFC` 0x134/0x138 (frame-done counters; the bare-metal example polls
these), `PTB_BPOA/BPOS` 0x308/0x30c, L2T/slice cache control (`L2TCACTL`,
`SLCACTL`). Interrupt traps: **FRDONE=BIT(0) is render-done, FLDONE=BIT(1) is
bin-done** (lower bit = render -- easy to swap), and **MMU faults arrive on the HUB
interrupt block** (hub+0x50, MMU_PTI=BIT(4)), not the core CTL_INT_STS -- check and
W1C-clear BOTH blocks.

## 2. Architecture and placement

**Decision: in-root driver; hardware init from the boot thread; all job execution on
the display_server thread.** V3D needs device-untyped frames, an IRQ handler cap,
vka/simple/vspace, and `gpu_fb`/`gpu_fb_pa` -- all root-task-only
(`include/aios/root_shared.h:271-276`). display_server already runs in the root
vspace, is core-0-pinned (`start_server_thread`, `src/boot/boot_services.c:38-48`),
and is the only post-boot toucher of the display: its one-op-at-a-time `seL4_Recv`
loop IS the FB arbitration model (a GPU job runs to completion inside the handler,
exactly like `DISP_CUBE` today, `src/servers/display_server.c:229-239`). No new
endpoint means zero churn in exec_server argv cap layout and both entry parsers.

**MMIO claiming** -- via `prealloc_rpi4_devices()` (`src/boot/boot_device_map.c:59`),
never self-mapped (GENET rule, `src/plat/rpi4/net_genet.c:594-598`):

| Region | Paddr | Pages | Global |
|---|---|---|---|
| V3D hub + core0 (contiguous) | 0xFEC00000 | 8 | `dev_v3d_vaddr` |
| RPiVid ASB (power bridges) | 0xFEC11000 | 1 | `dev_v3d_asb_vaddr` |
| PM block (PM_GRAFX at +0x10C) | 0xFE100000 | already mapped | `dev_pm_vaddr` (shared with watchdog/reboot; different registers, no conflict) |

Both new claims sit ABOVE the current highest claim (eMMC 0xFE340000), so the
forward-only ascending-paddr watermark is safe; they still go through the sorted
table (house convention). `struct dev_req reqs[8]` must bump to `reqs[12]` (7
entries today) and the stale `n <= 6` comment at `boot_device_map.c:89` gets fixed.

**DTB**: `parse_v3d()` in `src/boot/boot_dtb.c` cloned from `parse_genet`
(:199-214): compatible `brcm,2711-v3d`, VC-bus 0x7EC00000 -> +0x80000000 fixup
(the `parse_pcie` pattern :238-239), `irq = 74 + 32 = 106`. **Fixed-constant
fallback** (0xFEC00000 / 106) like PCIe (:267-289): the firmware runtime DTB strips
nodes, and downstream trees move v3d under a `v3dbus` node -- the path is not stable.
New `hw_info` fields: `has_v3d`, `v3d_paddr`, `v3d_irq`.

**Init sequencing** (`src/aios_root.c`):
- `v3d_mem_reserve()` immediately after `xhci_dma_reserve` (~:346) -- earliest
  possible, before fs cache/display consume RAM (capacity lesson).
- `v3d_init()` after the `xhci_init` block (~:375), before `boot_start_services`
  (:406): after `boot_display_init` (needs `gpu_fb_pa`) and after `plat_pcie_init`
  (the VC mailbox tag buffer pin at **0x3A002000** must be claimed after display's
  0x3A000000 and PCIe's 0x3A001000 in that untyped's own forward-only watermark).
  Single-threaded boot context required for IRQ binding (xHCI comment
  `src/usb/xhci.c:770-772`).
- **Zero V3D MMIO at boot.** `v3d_init()` only claims MMIO, parses DTB, binds IRQ
  106 (GENET recipe verbatim, `net_genet.c:703-741`), pins the tag buffer, reserves
  memory. The power sequence + IDENT probe run lazily in `v3d_ensure_on()` on the
  first poke/op. Rationale: pre-power V3D-block READS are HW-proven safe (they
  return the 0xDEADBEEF poison -- that is what `/proc/v3d` keys on); the unproven
  hazard is misordered/wrong PM/ASB WRITES and gated-block access generally (GENET
  SWINIT lesson), so the power sequence runs only on an explicit netconsole poke:
  a wrong sequence kills a fully-booted system with `.power.N` micro-poke bisection
  available, not mid-boot with a card pull.

**`v3d_ensure_on()`** (idempotent; micro-poke bisection via `/proc/v3d.power.N`):
1. `PM_GRAFX` (= `dev_pm_vaddr[0x10C/4]`) write `0x5A000000 | (val | BIT(6))`.
2. >=2 us delay via `cntpct_el0` (Linux waits 1 us).
3. ASB master `dev_v3d_asb_vaddr[0x0C/4]`: write `0x5A000000 | (val & ~BIT(0))`,
   poll BIT(1) clear, 10 ms deadline.
4. ASB slave, same at `[0x08/4]`. (Linux order: reset -> delay -> master -> slave.
   The Qiita writeup labels M/S swapped; trust offsets per `bcm2835-power.c`:
   S=+0x08, M=+0x0C. Every working sequence unstops both.)
5. Read `HUB_IDENT0` (hub+0x08): expect **0x04443356** (poison = 0xDEADBEEF =
   sequence did not take). Decode IDENT1 (TVER=4, REV=2, NCORES=1), IDENT2 bit 8
   (MMU present), core `CTL_IDENT1` (NSLC[7:4] x QUPS[11:8] = 8 QPUs). Abort
   (leave `v3d_ok=0`) on mismatch -- never proceed against a dead bus.
6. Optional: `SET_CLOCK_RATE(5, 500 MHz)` via a `vc_mbox_tag` clone
   (`src/plat/rpi4/pcie_brcmstb.c:206-264`, tag buffer 0x3A002000). Failure = warning.
7. `v3d_mmu_init()` (section 4), then unmask nothing -- run masked, poll-mode.

**Completion model: deadline-bounded polling, default and steady-state.** On non-MCS
seL4, `seL4_Wait` on the IRQ notification has no timeout -- a wedged GPU would block
display_server forever and cascade into the known display->tty->USB wedge. Poll
`BFC`/`RFC` with `seL4_Yield()` under `mono_deadline_ms` (bin 100 ms, render
250 ms); render completes in single-digit ms so poll cost is noise. IRQ 106 stays
bound as diagnostics: `v3d_wait` loops and `v3d_diag_cmd` `seL4_Poll` the
notification and increment `v3d_irqs` on a signal (in poll mode nothing else
consumes the ntfn -- without the seL4_Poll the Phase 0 "counter stays 0" check would
be unmeasurable). A `/proc/v3d.irq.1` opt-in blocking mode is
deferred until a watchdog exists outside display_server. Every wait in the driver
goes through one helper `v3d_wait(cond_fn, ms)` built on `cntpct_el0` --
grep-auditable, no raw loops (eMMC 32.6 s iteration-count lesson).

## 3. Files

**New** (always compiled into `aios_root` on both platforms, xhci.c precedent
`projects/aios/CMakeLists.txt:173`; `#ifdef PLAT_RPI4` only around PM/ASB/mailbox
specifics. On QEMU `has_v3d=0`, init no-ops, ops reply -1 -- keeps build-04 green by
construction and makes the IPC//proc plumbing QEMU-testable):

| Path | Purpose |
|---|---|
| `src/gpu/v3d.h` | Root-side API: `v3d_init`, `v3d_ok`, `v3d_clear(argb)`, `v3d_triangle()`, `v3d_cube_frame(ax, ay)`, `v3d_diag_cmd(args, buf, n)` |
| `src/gpu/v3d.c` | Power bring-up, IDENT probe, 8MB pool + bump allocator, submit + BFC/RFC deadline polling, both-INT-block clear, dump-and-reset, diag, counters. `LOG_MODULE "v3d"` |
| `src/gpu/v3d_regs.h` | Offsets/bits transcribed from Linux `v3d_regs.h` (hub IDENT/INT/MMU blocks; core IDENT/INT/CLE/PTB/cache-control). FRDONE/FLDONE bit-order trap documented at the define site |
| `src/gpu/v3d_mmu.c` | 4MB single-level PT build, `v3d_mmu_map(gpu_va, pa, npages, w)`, scratch page, Linux init order, two-step flush |
| `src/gpu/v3d_cl.c` / `v3d_cl.h` | v42 packet emitters (~25 packets), GL Shader State Record (36B/32B-aligned) + Attribute Record (16B) structs, tile sizing helpers (64x64 @1RT/32bpp; tile-state = tiles x 256B; tile-alloc = tiles x 128B initial-block, 4K-aligned, +8KB headroom). Freestanding C, compilable host-side under `-DV3D_HOST_BUILD` |
| `src/gpu/v3d_shaders.h` | The three borrowed Random06457 QPU blobs (frag 12 / vertex 25 / coord 18 x 64-bit words) as `uint64_t[]`, GLSL + disassembly provenance comments preserved |
| `tools/v3d_host_clgen.c` | Host build of `v3d_cl.c`; emits our CL byte streams for the fixed scene |
| `scripts/v3d_clcheck.py` | Field-by-field golden diff of our CLs vs Random06457's emitters (opcodes/lengths/non-address fields exact; address fields structural) |
| `scripts/pi_v3d_phase.py` | Netconsole phase driver: runs the per-phase probes, captures `/proc/v3d`, `/proc/log`, `/proc/fbcon`, `/proc/serverstats`, greps PASS/FAIL, `--regress` mode |
| `scripts/v3d_qemu_test.py` | QEMU plumbing regression (not-present path, graceful errors, DISP regression, ramfb pan if Phase 4b lands) |

**Modified**:

| Path | Change |
|---|---|
| `src/boot/boot_device_map.c` | `reqs[8]` -> `reqs[12]`; fix stale `n <= 6` comment (:89); add v3d (8 pg @ 0xFEC00000) + v3dasb (1 pg @ 0xFEC11000) entries gated on `has_v3d` |
| `include/aios/device_map.h` | `dev_v3d_vaddr`, `dev_v3d_asb_vaddr` externs |
| `include/aios/hw_info.h` + `src/boot/boot_dtb.c` | `has_v3d/v3d_paddr/v3d_irq`; `parse_v3d()` + fixed-constant fallback |
| `src/aios_root.c` | `v3d_mem_reserve()` ~:346; `v3d_init()` ~:375 |
| `include/aios/root_shared.h` | Labels 117-121 (section 6); 117+ confirmed free (nothing above `DISP_CONSOLE 116`). Update the "DISPLAY IPC labels (110-119)" range comment to (110-129) |
| `src/boot/boot_display_init.c` + `root_shared.h` extern | `gpu_fb_invalidate_page(idx)` beside `gpu_fb_flush` (:133-153): `seL4_ARM_VSpace_CleanInvalidate_Data(seL4_CapInitThreadVSpace, ...)` -- same no-cap-plumbing pattern as `gpu_fb_flush`, since `fb_caps[]` is static in `display_vc.c` |
| `src/servers/display_server.c` | Five `DISP_V3D_*` cases; consume `g_v3d_req` flag word at loop top |
| `src/boot/fb_console.c` + header | `fb_console_set_suspend(int)` gating `render_cell`/`scroll_up`/flush |
| `src/procfs.c` | `/proc/v3d` entry; match `path[1]=='3'` to dodge `version`/`vka` prefix collisions |
| `src/apps/fbshow.c` | `--gpu-clear[=AARRGGBB]`, `--gpu-tri`, `--gpu-cube[=frames]`, `--gpu-release` -> `seL4_Call(disp_ep, DISP_V3D_*)`. No new app: fbshow is the canonical display client, avoiding mkdisk/app plumbing |
| `projects/aios/CMakeLists.txt` | Add the three `src/gpu/*.c` to the `aios_root` source list (:136-176) |
| `src/plat/display_hal.h` + both impls | (Phase 4b only) `plat_display_pan(y_offset)`: RPi4 = runtime `TAG_SET_VIRT_OFF`; QEMU = ramfb fw_cfg rewrite with `addr += y*stride` (`display_ramfb.c:171-198`) |

## 4. GPU memory

**Reservation** -- `v3d_mem_reserve()`: one `vka_alloc_untyped(&vka, 23)` (8 MB,
naturally aligned => phys-contiguous), retyped into 4 x `seL4_ARM_LargePageObject`
(4 caps, sequential => contiguous; VKA-pool friendly vs 2048 small pages), one
`vspace_map_pages(..., 0 /* non-cacheable */)`, paddr via
`seL4_ARM_Page_GetAddress(caps[0])`. **No paddr constraint and no low-retry loop**
-- any RAM on the 4GB board works (corrected finding). 8 MB justified: 4 MB PT +
~350 KB live data => >10x headroom for textures/second RT later.

**Pool layout** (fixed offsets, non-cacheable end-to-end):

| pool offset | size | object |
|---|---|---|
| 0x000000 | 4 MB | GPU page table (zeroed = all-invalid; 4KB-aligned by construction) |
| 0x400000 | 4 KB | MMU scratch page (ILLEGAL_ADDR target) |
| 0x401000 | -> 0x800000 | bump region: CLs, tile state (48 KB), tile alloc (32 KB), 256 KB overflow reserve, shaders/records/uniforms/vertices |

Allocator: `struct v3d_bo { void *cpu; uint64_t pa; uint32_t gpu_va; uint32_t size; }`;
`v3d_bo_alloc(size, align)` bump pointer; `v3d_bo_reset_frame()` rewinds a per-frame
watermark above the static allocations. No free list -- whole-pool lifetime,
frame-transient objects rewind.

**GPU VA map** (single 4GB space, Linux-style; page 0 never mapped -- HW treats VA 0
as special):

| GPU VA | maps to | size | PTE |
|---|---|---|---|
| 0x0000_0000 | unmapped guard | 1 MB | invalid |
| 0x0010_0000 | pool pa+0x400000 (scratch + bump region) | 4 MB | VALID\|WRITEABLE |
| 0x1000_0000 | `gpu_fb_pa` (live HDMI FB, 768 pages; 1536 if Phase 4b doubles it) | 3 MB | VALID\|WRITEABLE |
| 0x2000_0000 | reserved unmapped | -- | invalid (Phase 1 fault-probe target) |

`gpu_va = 0x00100000 + (pool_off - 0x400000)` -- constant-offset translation, no
lookup structure. The PT itself is deliberately NOT GPU-mapped. Full 4 MB PT kept
(vs `V3D_MMU_ADDR_CAP` shrinking): any stray GPU VA indexes a zeroed-invalid PTE,
Linux never uses ADDR_CAP, and 4 MB is cheap. No big/super pages for bring-up
(alignment rules add risk for zero need).

**MMU init** (`v3d_mmu_init()`, copies `v3d_mmu_set_page_table` order):
`PT_PA_BASE = pt_pa >> 12` -> `MMU_CTL = ENABLE | PT_INVALID_{ENABLE,ABORT,INT} |
WRITE_VIOLATION_{ABORT,INT} | CAP_EXCEEDED_{ABORT,INT}` (copy the bit list verbatim
from `v3d_mmu_set_page_table` at implementation time -- omitting PT_INVALID_ENABLE
silently breaks the Phase 1 fault probe) -> `ILLEGAL_ADDR =
(scratch_pa >> 12) | BIT(31)` -> `MMUC_CONTROL = ENABLE` -> flush-all
(MMUC `FLUSH|ENABLE`, poll FLUSHING clear; then `MMU_CTL |= TLB_CLEAR`, poll
TLB_CLEARING clear; 10 ms deadlines). All mappings are established once at init
(static map above) so TLB flushes are init/reset-time only. PTE writes hit the
non-cacheable PT mapping -- no cache clean, just `dsb` before touching MMU regs.

Note: the proven Random06457 example runs with the MMU **off** (raw physical
addressing). We keep the MMU on from Phase 1 because a buggy CL on a system with a
live write-back fs cache must not be able to scribble arbitrary RAM -- the zeroed PT
+ ILLEGAL_ADDR scratch page is the containment. If MMU behavior is ever suspected
during bring-up, a `/proc/v3d` poke can disable it temporarily to bisect (diagnostic
only, never the shipped mode).

## 5. Cache-coherency contract

The pool is non-cacheable end-to-end (xHCI philosophy, `src/usb/xhci.c:9-14`:
coherent by construction, zero maintenance). CPU writes to CLs/records/vertices are
small and write-once-per-frame; uncached write cost is noise.

| buffer class | CPU mapping | GPU access | maintenance |
|---|---|---|---|
| GPU page table | non-cacheable | MMU reads pa | none; `dsb` after last PTE |
| bin/render CLs, generic tile list | non-cacheable | CLE fetch via MMU | none; `dsb` before QEA |
| shaders, records, uniforms, vertices | non-cacheable | QPU/VCD reads | none |
| tile state / tile alloc / overflow | non-cacheable | GPU r+w only | none (CPU never reads; diag dumps are uncached reads) |
| framebuffer (render target) | cacheable (unchanged, `display_vc.c:107`) | GPU writes via MMU | ownership protocol below |
| MMU scratch page | non-cacheable | stray-access sink | diag-read only |

**Framebuffer ownership protocol** (the one nontrivial case -- VC scanout and the
GPU both touch physical RAM while the CPU holds a cacheable mapping):
1. On first `DISP_V3D_{CLEAR,TRI,CUBE}` with `v3d_owns_display == 0`:
   `gpu_fb_flush_all()` (`boot_display_init.c:150-153`) then
   `fb_console_set_suspend(1)`. After the flush every CPU line over the FB is clean;
   clean-line evictions never write RAM, so cache writeback cannot overwrite GPU
   pixels. Load-bearing invariant: **the CPU must not dirty the FB while the GPU owns
   it.** Concurrency is structural (all FB writers run on the display_server thread),
   but interleaving is not: the legacy CPU draw ops (`DISP_CLEAR/TEXT/FILL_RECT/
   SHOW_FILE/CUBE`, 110-115) dirty cacheable FB lines between V3D ops. Rule: any
   legacy draw op clears `v3d_owns_display = 0`, so the NEXT V3D op re-runs the
   takeover (flush-all + suspend) instead of trusting a stale clean state.
2. While suspended, `DISP_CONSOLE` batches are dropped (server replies OK; serial
   still carries every byte). fb_console has no redraw-from-grid path, so feeding
   cells under GPU ownership would recreate the unprotected-writer race.
3. CPU pixel probes (PASS-line verification): the codebase's first invalidate --
   `seL4_ARM_VSpace_CleanInvalidate_Data(seL4_CapInitThreadVSpace, ...)` on the
   single probed page, then read (the VSpace form needs no frame cap -- `fb_caps[]`
   is static in `display_vc.c`; same pattern as `gpu_fb_flush`). CleanInvalidate is
   safe (lines are clean per step 1), wrapped as `gpu_fb_invalidate_page(idx)` in
   `boot_display_init.c` beside `gpu_fb_flush`, same never-cross-a-page-boundary
   rule. Bulk readback out of scope.
4. `DISP_V3D_RELEASE`: `v3d_owns_display = 0`, `fb_console_set_suspend(0)`,
   `fb_console_clear()` (`fb_console.c:230-240`) -- console restarts clean,
   overdrawing GPU remnants (also retroactively improves on the documented
   `DISP_CUBE` no-restore wart).

**Encoded AIOS lessons**: pipe-SHM same-attribute rule -- the FB keeps exactly one
CPU mapping (cacheable, root); the GPU's MMU mapping is not a CPU mapping so no
mismatch exists; any future client-mapped V3D BO must be non-cacheable both ends.
Per-page clean limit -- flush and invalidate helpers chunk at 4 KB. Capture-before-
notify (virtio-blk v0.4.147) -- BFC/RFC baselines latch BEFORE the QEA doorbell.

**GPU-internal caches** (CPU coherency is necessary, not sufficient): before each
bin kick, `v3d_invalidate_gpu_caches()`: `SLCACTL` all-slice invalidate +
`L2TCACTL` flush with `L2TFLSTA=0/L2TFLEND=~0`, polled under deadline. Offsets
mirrored from Linux `v3d_regs.h` at implementation time (checklist item -- not from
memory).

**Doorbell bracket** (mirrors `cmd_submit`, `xhci.c:196-207`): pool/PT writes ->
`dsb` -> CT0QTS/QMA/QMS/QBA -> `dsb` -> **CT0QEA (kick)** -> `dsb`. Same for
CT1QBA -> CT1QEA. Poll loops issue `dmb` before each BFC/RFC read.

## 6. IPC surface and job flow

Labels on the existing `disp_ep` (`include/aios/root_shared.h`, after :96):

```
#define DISP_V3D_INFO     117  /* MR0=v3d_ok, MR1=hub IDENT1, MR2=core IDENT1, MR3=counters */
#define DISP_V3D_CLEAR    118  /* MR0=AARRGGBB; GPU full-screen clear */
#define DISP_V3D_TRI      119  /* static RGB triangle over a clear */
#define DISP_V3D_CUBE     120  /* MR0=frames (default/cap 600); spinning cube in-handler */
#define DISP_V3D_RELEASE  121  /* resume console: clear + unsuspend */
```

Reply status in MR0: -1 = no V3D/not powered, -2 = job timeout, -3 = MMU fault
(matching the conventions at `display_server.c:116-121`). The cube monopolizes
display_server for its run (tty `seL4_Call`s stall, same as today's `DISP_CUBE`);
frames are paced to ~60 fps via a `cntpct_el0` delay (GPU render is single-digit
ms, so unpaced rotation speed would be frame-time-dependent), making the 600-frame
cap ~10 s so a forgotten cube self-terminates.

**`v3d_submit_frame()`** (display_server thread only):
1. `v3d_ensure_on()`; `v3d_invalidate_gpu_caches()`.
2. Latch `bfc0/rfc0` from BFC/RFC; clear stale INT_STS via INT_CLR (run masked;
   STS stays readable for diagnosis). Capture before kick.
3. `PTB_BPOS = 0` (clear stale overflow size, per `v3d_bin_job_run`);
   `CT0QTS = tile_state_va | ENABLE`; `CT0QMA/QMS = tile_alloc va/size`.
4. Doorbell bracket: CT0QBA -> CT0QEA. Wait bin: `(BFC & 0xFF) != (bfc0 & 0xFF)`,
   100 ms deadline; on `OUTOMEM`: supply `BPOA=overflow_va, BPOS=256K`, **W1C-clear
   the OUTOMEM status bit** (else the stale latched bit re-triggers the supply every
   poll pass), count, continue; a SECOND OUTOMEM in the same frame aborts the job
   (the single reserve must not be handed out twice).
5. Doorbell bracket: CT1QBA -> CT1QEA. Wait render: RFC advance, 250 ms deadline.
6. On deadline or MMU fault (hub MMU_PTI/WRV/CAP seen in the poll loop):
   `v3d_dump_and_reset()`, return error.

**`v3d_dump_and_reset()`**: dump first, to AIOS_LOG + the `/proc/v3d` stats struct
(survives for post-hoc `cat /proc/v3d`): CT0CS/CT1CS raw, **CT0CA/CT1CA minus QBA**
(byte offset of the hanging packet -- the single most useful number), CT0RA/CT1RA,
both INT_STS blocks, MMU_VIO_ADDR/VIO_ID/DEBUG_INFO, PT_PA_BASE readback, BFC/RFC.
Then full reset (one code path, mirrors Linux recovery): assert V3DRSTN
(`PM_GRAFX = 0x5A000000 | (val & ~BIT(6))`), 10 us, full power sequence,
`v3d_mmu_init()` re-program, sanity-read IDENT0 == 0x04443356 before declaring the
GPU usable. Increment `v3d_resets`.

## 7. /proc/v3d diagnostics

Modeled on `/proc/xhci` (`procfs.c:474-478`) + `genet.poke`. Threading model, two
regimes keyed on `v3d_ok`:
- **Bring-up regime (`v3d_ok == 0`, phases 0-1)**: `.power`, `.mmu`, `.fault`, and
  register peeks/pokes run DIRECT on the fs thread -- nothing else touches V3D yet
  (genet.poke precedent), and none of these touch the framebuffer.
- **Integrated regime (`v3d_ok == 1`)**: every mutating or mailbox-touching verb
  (`.test`, `.fault`, `.clock`, `.clock.N`, `.power`, `.w`) posts a `g_v3d_req`
  word consumed at the top of `display_server_fn` (the `g_led_request` pattern,
  `xhci.c:438-441`); documented kick: `fbshow --info`. The executed verb writes its
  result line into the stats struct; the caller harvests it with a subsequent
  `cat /proc/v3d` (`pi_v3d_phase.py` automates post -> kick -> read). This keeps
  two invariants: single-toucher V3D MMIO/mailbox (the VC mailbox FIFO is stateful
  and Phase 4b pans from display_server -- a fs-thread GET_CLOCK would race it),
  and the FB ownership protocol applied by display_server around `.test`.
- Pure register-read verbs (`cat /proc/v3d`, `.r`, `.c`, `.mmu` dump) stay direct
  in both regimes (status reads are benign).

| Verb | Function |
|---|---|
| `cat /proc/v3d` | One-screen summary: IDENT0 (flagged DEAD if 0xDEADBEEF), hub/core IDENT decode, MMU state (CTL, PT base, DEBUG_INFO widths), CLE snapshot (CT*CS, CT*CA, QBA/QEA, BFC/RFC), both INT blocks decoded, counters (jobs, bin/render done, outomem, mmu_faults, timeouts, resets), owns-display flag |
| `cat /proc/v3d.power` | **The minutes-not-days probe**: IDENT0 before (expect 0xDEADBEEF) -> PM/ASB sequence -> IDENT0 after (expect 0x04443356) + ASB ACK timings. Runnable on the first kernel8 swap; validates the whole power story before any other code exists. `.power.N` = micro-poke step N for SError bisection |
| `cat /proc/v3d.mmu` | MMU regs + VIO_ADDR/VIO_ID + first/FB-window PTE samples + scratch paddr. Post-fault postmortem |
| `cat /proc/v3d.clock[.N]` | GET_CLOCK_RATE(5) / SET_CLOCK_RATE(5, N MHz) |
| `cat /proc/v3d.r.<off>` / `.c.<off>` / `.w.<off>.<val>` | hub/core peek, hub poke (bring-up only; genet `mr/mw` analogue) |
| `cat /proc/v3d.test[.N]` | Canned clear job (Phase 2 smoke), N iterations; result line `bfc a->b rfc a->b bin_us rend_us pixel[512,384]=... PASS/FAIL` lands in the stats struct, harvested via `cat /proc/v3d` (request-flag verb -- executed by display_server with the FB ownership protocol) |
| `cat /proc/v3d.fault` | Deliberate 1-packet CL at unmapped VA 0x20000000; dump VIO_ADDR + hub INT_STS (expect MMU_PTI BIT(4) on the HUB block). Direct in the bring-up regime; request-flag after |
| `cat /proc/v3d.irq.1/.0` | Completion-wait mode toggle (deferred; default poll) |

All failure events also land in the AIOS_LOG ring -> `cat /proc/log` over netconsole
when the display is wedged.

## 8. Phased bring-up plan

Common to every phase: ships as a kernel8 swap (`ninja -C build-rpi4` ->
`scripts/mkkernel8.py` -> copy to AIOSBOOT -- seconds, no reflash); driven over
netconsole (gently: one held connection, ~4 s settle); one grep-able PASS/FAIL line
per probe; ends with the section 9 regression checklist; all waits deadline-bounded.

**Phase 0 -- power, IDENT, IRQ (2-3 days).** Registers: PM_GRAFX, ASB M/S,
IDENT0-3, CTL_IDENT0-2. Buffers: tag page only. Probe: `/proc/v3d.power`.
Success: `ident0=0x04443356 ver=4.2 cores=1 qpus=8 mmu=1`; survives reboot+rerun;
IRQ-106 counter stays 0 (everything masked).
Failures: IDENT stays 0xDEADBEEF -> sequence didn't take (diag exports PM/ASB
readbacks + ACK timings); SError halt on a `.power.N` micro-poke -> step N
identifies the faulting access, power-cycle and re-bisect; IDENT reads 0x0 ->
mapping bug (check the `prealloc` boot log for the v3d claim).
Exit: IDENT verified on HW, ASB acks < 1 ms, regression green.

**Phase 1 -- MMU + deliberate fault (2-3 days).** Buffers: 4 MB PT (zeroed),
4 KB scratch. `v3d_mmu_init()` exactly as section 4. Probe: `/proc/v3d.fault` --
kick CT1 at unmapped 0x20000000, poll **hub** INT_STS for MMU_PTI, read
VIO_ADDR/VIO_ID, clear, `v3d_dump_and_reset()`.
Success: `vio_addr=0x20000000 PASS, reset ok`; `ident` still passes after; no fault
when probing a mapped VA.
Failures: FLUSHING never clears -> MMU not powered (dump MMUC readback); no fault
latched -> PT base wrong/MMU disabled (dump PT_PA_BASE + MMU_CTL readbacks). No ARM
SError is expected here (GPU faults never trap the A72) -- if one occurs the bug is
in our MMIO mapping.
Exit: clean flush under deadline; fault detected, attributed, recovered remotely.

**Phase 2 -- GPU clear to the live HDMI FB (3-4 days).** Empty-bin + render pair
(not render-only): mirrors the proven Random06457 path and exercises both CLE
threads. Render target = live scanout FB (success is literally visible; console
suspended first). Buffers: bin CL ~256 B, render CL ~4 KB, tile state 48 KB
(192 tiles x 256 B at 1024x768), tile alloc 32 KB (192 x 128 B initial-block,
4K-aligned, +8 KB headroom so the PTB's first two chunk allocations never OUTOMEM
-- Mesa `v3d_util.c` rule), 256 KB overflow reserve idle.
Bin CL: TBMC(120) -> FLUSH_VCD_CACHE(19) -> OCCLUSION_QUERY_COUNTER(92, zero) ->
START_TILE_BINNING(6) -> FLUSH(4). Render CL: TRMC(121) sub-packets Common-first ->
Color -> Clear Colors -> **ZS Clear Values last** -> 126 (=128 B, must match bin
cfg) -> 123 (tile-alloc base) -> 122 (supertiles 4x4 tiles -> 4x3 grid; 1xN/Nx1
disallowed, <=256) -> dummy clear tile {124, 26, 29(NONE), 25, 27} -> **second dummy
store tile** (Random06457 race workaround, keep it) -> generic tile list {125, 26,
PRIM_LIST_FORMAT(56, triangles), SET_INSTANCEID(54, 0), 21, 29(color RT), 27, 18}
registered via 20 (56/54 are no-ops in a clear job but match the proven sequence,
so the Phase 3 diff only grows the bin CL) -> 12x SUPERTILE_COORDINATES(23) ->
END_OF_RENDERING(13).
Pixel format: the live FB is BGR byte order (`TAG_SET_PIXEL_ORD = 0`, memory
[B,G,R,X] -- the v0.4.169 R/B-swap history); pick the TRMC Color internal type +
Store(29) output format to match, and use an **R/B-asymmetric clear color (orange,
0xFFFF8000)** so the probe actually catches a channel-order bug (magenta is
invariant under R/B swap).
Success: solid orange on the monitor; `.test` result line shows counter deltas +
pixel probe PASS; repeatable x100.
Failures: RFC stays 0 -> CT1CA-minus-QBA pinpoints the hanging packet; hub MMU_PTI
-> FB PTEs wrong (VIO_ADDR says which VA); **RFC advances but screen unchanged** ->
stores redirected to the ILLEGAL_ADDR scratch page (diag dumps scratch first-words:
non-zero = smoking gun) or wrong RT address.
Exit: visible clear, zero MMU faults, 100-iteration stability, regression green.

**Phase 3 -- triangle via GL Shader State (1-2 weeks; highest risk).** Borrow the
three Random06457 QPU blobs verbatim (no NV path exists). New objects: GL Shader
State Record (36 B @ 32-byte alignment; frag/vertex/coord {code addr>>3 : 29 bits,
uniforms addr}, `fragment_shader_4_way_threadable=1`, `propagate_nans=1`, FS varying
count, VPM segment sizes -- copied field-for-field), 2x Attribute Records (float3
position stride 12; normalized ubyte4 color stride 4; values-read-by-VS/CS set),
default-attribute block 256 B, vertex buffer 48 B, VS/CS uniform streams (~32 B:
1/w, viewport half-width x 256, half-height x 256, Z scale/offset -- screen X/Y
leave the VS as integer **1/256-pixel** units; the "12.4" intuition is the #1 trap).
Bin CL grows between START_TILE_BINNING and FLUSH: CLIP_WINDOW(107), CFG_BITS(96,
no culling), POINT_SIZE/LINE_WIDTH, CLIPPER_XY_SCALING(110, (w/2)*256, (h/2)*256),
CLIPPER_Z_SCALE_AND_OFFSET(111) + Z min/max, VIEWPORT_OFFSET(108, u14.8 center),
COLOR_WRITE_MASKS, zeroed flat-shade/non-perspective/centroid flags, SAMPLE_STATE,
VCM_CACHE_SIZE(71, 4/4), **GL_SHADER_STATE(64, record VA | nattr)**,
VERTEX_ARRAY_PRIMS(36, mode 4, count 3, first 0).
Success: rainbow triangle on HDMI; center-pixel probe != clear color, 3 corner
probes == clear color.
Failures (all remotely distinguishable from {CT*CA offsets, VIO_ADDR, pixel
probes}): CT0CA frozen at the GL_SHADER_STATE offset -> record/blob VA unmapped;
bin done + render hung -> tile-list corruption (re-run the golden diff); completes
blank -> vertices scaled off-screen (check the x256 viewport uniforms) or varying
count mismatch; garbage colors -> attribute stride/normalized flags.
Exit: triangle bit-stable across reboots; CL bytes match the host golden check.

**Phase 4a -- depth + spinning cube, single-buffered (1 week). The deliverable.**
No external depth buffer: Z lives in the on-chip tile buffer (depth-test LESS +
early-Z in CFG_BITS, internal depth type in TRMC Common, Z=1.0 via ZS Clear Values,
never stored -- saves 3 MB and a load/store path). CPU transforms 36 vertices/frame
on the A72 into the same pre-viewport space (reuses the borrowed VS/CS unchanged --
zero new QPU code). Backface culling on. Per frame: write verts (576 B,
non-cacheable, no maintenance), rebuild the two small CLs in place, submit, wait
RFC. Tearing accepted (same as today's CPU `DISP_CUBE`).
Success: spinning shaded cube on the monitor; `fbshow --gpu-cube=300` prints
`300 frames avg=...us fps=... outomem=0 PASS`.
Exit = project deliverable: clear -> triangle -> cube all GPU-rendered, drivable
over netconsole, regression green, both builds green, committed.

**Phase 4b -- double-buffer via mailbox panning (optional follow-up).** Request
virt_h = 2x768 at display init (`TAG_SET_VIRT_WH`, `display_vc.c:318-354`), bump
`GPU_FB_MAX_PAGES` 1024 -> 2048 (`display_vc.c:52`), map both halves into GPU VA;
render to the back half, then `plat_display_pan(back_y)` = runtime `TAG_SET_VIRT_OFF`
(0x48009) through the existing `mbox_call` (single-toucher: only display_server
calls it post-boot). Pan-on-RFC is tear-minimal; no vsync IRQ exists. This is the
same primitive the backlogged HDMI scroll-perf item wants ("the real fix is HW
panning") -- landing it here produces reusable infrastructure. Fallback if the
firmware rejects virt_h x 2: stay on 4a single-buffer.

## 9. Validation strategy (no QEMU V3D exists)

1. **Host golden CL check (CI, no HW)**: `tools/v3d_host_clgen.c` builds `v3d_cl.c`
   under `-DV3D_HOST_BUILD` and emits our bin+render byte streams;
   `scripts/v3d_clcheck.py` diffs field-by-field against a one-time capture from
   Random06457's emitters (their `v3d_cl.hpp` also compiles host-side): opcodes,
   lengths, and scene-independent fields exact; addresses structural; and a
   **documented whitelist of expected divergences** -- tile-alloc initial-block-size
   enums (we use Mesa's 128 B, Random06457 uses 64 B; packets 120/126 must agree
   with each other, not with the capture), width/height-derived fields, supertile
   cfg(122) grid, SUPERTILE_COORDINATES count, clear colors. Without the whitelist
   a field-exact diff fails by design. Optional second opinion:
   macoy's `v3d-toolkit` decoder (the only open executable model of v42 CLs). Run
   before every HW deploy of a CL-touching change.
2. **QEMU smoke** (`scripts/v3d_qemu_test.py`): build-04 boots; `/proc/v3d` says
   not-present; `fbshow --gpu-clear` returns graceful -1; `fbshow --cube` (CPU demo)
   still works; existing suites stay green. Never interpret QEMU reads as GPU state.
3. **Staged HW probes**: each phase has a netconsole-greppable PASS line;
   `scripts/pi_v3d_phase.py` drives the sequence gently and captures `/proc/v3d`,
   `/proc/log`, `/proc/fbcon`, `/proc/serverstats` after each step.
4. **Triage asymmetry by design**: the net path survives display wedges (proven in
   the scroll-freeze investigation), so every failure artifact (stats struct, log
   ring, CT*CA offsets, VIO dumps) stays reachable over TCP with HDMI/tty/USB dead.
   If the box SErrors, the `.power.N` micro-pokes bound the faulting access. If
   results look haunted across kernel8 swaps in the boot/display handover path,
   reflash the whole card before deep-debugging (the HDMI scroll-"freeze" ghost).
5. **Regression checklist** (`pi_v3d_phase.py --regress`, after every HW deploy):
   ping 0% loss; netconsole echo; one `ssh -tt` session; USB keyboard types on the
   HDMI console; `/proc/fbcon` sane; `fbshow --cube` runs; `date` sane (SNTP);
   `/proc/serverstats` all servers answering; `reboot` works. Plus
   `ninja -C build-04 && ninja -C build-rpi4` green before any commit.

**Deploy matrix**: driver/display_server/procfs/boot changes (the bulk) = kernel8
swap (~1 min); `fbshow` = `pi_filexfer.py push` + reboot (~1 min); config.txt /
partitions = full balenaEtcher reflash -- **not needed by this project** (the Phase
4b virt-WH change is a runtime mailbox tag).

## 10. Risk register (ranked)

| # | Risk | Detection | Mitigation |
|---|---|---|---|
| 1 | Display wedge: GPU hang inside the display_server handler stalls tty `seL4_Call`s -> HDMI + USB dead (the scroll-freeze blast radius) | disp SVC_PING timeout; `/proc/fbcon` static; net still pings | Every wait through `v3d_wait` (deadline-bounded, no untimed `seL4_Wait`); dump-and-reset returns the handler <=350 ms; cube capped at 600 frames |
| 2 | SError on gated-block MMIO (wrong/reordered power sequence) -> kernel halt | Boot unaffected (zero boot-time MMIO); halt on `.power.N` identifies step N | Power-on only via explicit poke; strict PM -> delay -> ASB-M -> ASB-S order before ANY 0xFEC0xxxx read; kernel8 reswap is seconds |
| 3 | Silent scratch-page redirect: wrong GPU VA -> job "succeeds" (RFC++) with no pixels | RFC advanced but pixel probe unchanged; `mmu_faults` counter; non-zero scratch first-words | Hub MMU bits counted every poll pass; pixel probes are part of every PASS line |
| 4 | Borrowed blob / GL Shader State Record mismatch (the least-verifiable area) | CT0CA frozen at the GL_SHADER_STATE offset; completes-blank/garbage | Replicate Random06457 bit-for-bit; host golden diff gates every deploy; v3d-toolkit as second decoder |
| 5 | FB cache-coherency violation (CPU dirties cached FB lines under GPU ownership) | Photo vs pixel-probe disagreement; console text mid-render | Ownership protocol (suspend + flush-all before GPU owns); single-writer thread; CleanInvalidate probes |
| 6 | SMP/allocator hazard (V3D work off core 0 tears unlocked vka/CSpace) | Heap/CSpace corruption, delayed crashes | No new thread at all; IRQ ntfn bound during single-threaded boot; if a thread is ever added, `start_server_thread` only |
| 7 | 8 MB contiguous untyped unavailable (late reservation / VKA pressure) | `v3d_mem_reserve` FAIL in boot log; `/proc/v3d` `pool=0` | Reserve immediately after `xhci_dma_reserve`; fallback 2x2 MB halves for data (PT must stay 4 MB contiguous; ADDR_CAP shrink = last resort, flagged unverified) |
| 8 | Device-untyped watermark regressions (`reqs[]` overflow; 0x3A002000 tag pin ordering vs display/PCIe) | `prealloc`/`alloc_frame_at` FAIL lines at boot | `reqs[12]` + comment fix; tag pin inside `v3d_init()` which runs after pcie/display by boot order; document the 32 MB @0x3A000000 vs 64 MB @0x3C000000 untyped split at the claim site |
| 9 | Binner OUTOMEM as geometry grows | `outomem` counter | +8 KB headroom sizing (Mesa rule) + 256 KB BPOA/BPOS refill path wired from day one |
| 10 | Tearing / cube remnants on console resume (cosmetic) | Visual | Accepted for 4a; 4b panning double-buffer is the follow-up and doubles as the scroll-perf fix |

## 11. Load-bearing references

- Linux `drivers/gpu/drm/v3d/`: `v3d_regs.h` (every offset/bit), `v3d_mmu.c` (PTE
  format, init order, flush), `v3d_sched.c` (QMA/QMS/QTS/QBA/QEA submit order,
  "writing the end register is what starts the job"), `v3d_irq.c` (FLDONE/FRDONE/
  OUTOMEM), `v3d_drv.c` (DMA mask from MMU_DEBUG_INFO)
- Linux `drivers/pmdomain/bcm/bcm2835-power.c`: the BCM2711 V3D power path
  (PM_GRAFX V3DRSTN + RPiVid ASB M/S unstop; POWUP sequence skipped on 2711)
- raspberrypi/linux `bcm2711.dtsi` (+`bcm2711-rpi-ds.dtsi` v3dbus): reg, IRQ,
  power-domains, `firmware_clocks 5`
- Mesa `src/broadcom/cle/v3d_packet.xml` (min_ver=42 -- the packet spec),
  `v3dx_draw.c`/`v3dx_rcl.c` (CL ordering, "Common config must be first",
  "ZS Clear Values ends rendering mode config"), `common/v3d_util.c` (tile sizing,
  +8 KB PTB headroom), `clif/v3dx_dump.c` (open CL decoder)
- Random06457/rpi4-gpu-bare-metal-examples: `pm.cpp` (powerOnV3D), `v3d_cl.hpp`
  (packet structs), `gl_rainbow_triangle.cpp` (the full proven CL + shader-record
  sequence and the three QPU blobs), `qpu_instr.hpp` (v3d 4.x QPU encoding)
- macoy `v3d-toolkit` (MIT): `v3dSimulator.h`, `v3dInspector.h`, `v3dAssembler.h`,
  `data/v3d.xml` -- the only open executable v42 CL model
- gyabo (Qiita) RPi4 bare-metal V3D: the 0xDEADBEEF -> 0x04443356 power evidence
- Idein `py-videocore6`: working QPU assembler for this generation (stretch goal)
- BCM2711 datasheet 1.2.x: full 35-bit vs legacy-master addressing
- AIOS precedents: `src/usb/xhci.c` (DMA reserve, diag, request-flag, poll-mode),
  `src/plat/rpi4/net_genet.c` (IRQ binding, premap rule),
  `src/boot/boot_device_map.c` (watermark), `src/boot/boot_display_init.c`
  (gpu_fb_flush), `docs/DESIGN_USB_HID.md` (design-doc conventions)
