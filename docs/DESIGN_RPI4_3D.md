# DESIGN: Hardware-Accelerated 3D on Raspberry Pi 4 (V3D 4.2 / VideoCore VI) for AIOS

Status: research / planning only (no code yet). Target: BCM2711, V3D revision 4.2,
seL4 AArch64, AIOS single root task. Date: 2026-06-05. (Produced by a research agent;
see `rpi4-hdmi-phaseb-mailbox` memory for the 2D framebuffer we already drive.)

## 0. TL;DR

- The Pi 4's 3D engine is **V3D 4.2**, a tile-based deferred renderer architecturally
  DIFFERENT from the VideoCore IV ("vc4") in the Pi 1-3. **Almost every bare-metal 3D
  tutorial online targets vc4 and its registers/control-lists do NOT apply unchanged to
  the Pi 4.** Headline differences: V3D 4.x has a real **IOMMU/MMU** (GPU memory is normal
  CPU RAM mapped through a GPU page table, NOT mailbox-locked GPU memory), a **split
  register map** (HUB + per-core CTL + MMU blocks) instead of vc4's flat map, and added
  CSD (compute) and TFU (texture) units.
- **V3D can be driven bare-metal directly from the ARM** by mapping its MMIO and poking
  registers (proven: phire/hackdriver, macoy.me PiV3D, Random06457). No per-job firmware
  involvement once powered+clocked. The firmware's only 3D role is bring-up (power domain
  + clock).
- **There is no official register/programming spec for V3D 3.x/4.x.** Broadcom only
  published the **VideoCore IV** guide (vc4). For the Pi 4 the de-facto spec is **Mesa's
  `src/broadcom`** (CLE packet XML + the v3d Gallium driver) plus **Linux
  `drivers/gpu/drm/v3d`** (register header, MMU, job submit).
- **Recommendation: Option A - a minimal custom V3D register-level driver in the AIOS root
  task.** Mesa port (B) is enormous; firmware exposes no usable 3D path (C); CPU rasterizer
  (D) is the pragmatic fallback / stepping stone.
- Effort for a first shaded triangle via V3D: **~3-6 focused weeks** (dominated by
  control-list/shader-record bring-up + MMU debugging). A spinning Gouraud cube: **~2-4
  weeks** more. CPU-rasterizer baseline: ~1-2 days.

## 1. Architecture: V3D 4.2 on BCM2711

**Where the 3D core sits.** BCM2711 = 4x Cortex-A72 (where AIOS runs) + the **VPU**
("VideoCore", runs `start4.elf` firmware: clocks, power, mailbox, 2D/HVS/HDMI) + the
**V3D** block (the 3D/compute engine, rev 4.2: 2 slices x 4 QPUs = 8 QPUs, 2 TMUs, up to
500 MHz). V3D is independent silicon with its own MMIO + IOMMU; the ARM talks to it
directly over AXI and does NOT go through the firmware for 3D work. Firmware only turns
V3D on (power + clock).

**Tile-based binner/renderer model.** V3D is a tile-based renderer (TBR). The framebuffer
is divided into tiles (~64x64 px at 32bpp). Drawing is two passes:
1. **Binning pass** (bin control list on CT0): coordinate-shader part of vertex processing
   runs; the Primitive Tile Binner computes which tiles each primitive touches and writes a
   per-tile sub-list to memory.
2. **Rendering pass** (render control list on CT1): iterates every tile; loads/clears the
   tile, branches to that tile's binner sub-list to rasterize+shade, stores the tile back.
A minimal "clear + one triangle" needs BOTH a bin CL and a render CL.

**GPU memory + the V3D MMU - the big Pi-1-3-vs-Pi-4 change.**
- Pi 1-3 (vc4): GPU had NO MMU. Buffers were allocated+locked via firmware mailbox (tag
  `0x3000c` MEM_LOCK) returning a GPU bus address; CLs/buffers lived in that contiguous
  locked region. (This is the model in nearly all old bare-metal demos.)
- Pi 4 (V3D 4.2): V3D has a real **IOMMU**. GPU buffers are ordinary CPU RAM, NOT locked,
  NOT necessarily contiguous. You build a **GPU page table** in RAM mapping a 32-bit GPU VA
  space onto CPU physical pages, point the MMU at it, and the GPU issues GPU-virtual
  addresses inside control lists. PTE format (Linux `v3d_mmu.c`): `pte = (phys >> 12) |
  flags`, flags = `V3D_PTE_VALID (bit 28)`, `V3D_PTE_WRITEABLE (bit 29)` (+ big/super page
  bits 30/31 on newer silicon). After writing PTEs: flush `V3D_MMUC_CONTROL` (FLUSH|ENABLE)
  + `V3D_MMU_CTL` (TLB_CLEAR). Faults surface in `V3D_MMU_VIO_ADDR`.
- **The GPU is still a 32-bit master:** every page mapped for it (incl. the page table)
  must be in **low physical RAM (< 1 GB)** - the same constraint AIOS already respects for
  the mailbox framebuffer.

**The V3D 4.x register interface (concrete map).** Not one flat file (that was vc4). Split
into blocks. BCM2711 device tree `brcm,2711-v3d` exposes two MMIO ranges:

| Range | Bus addr | ARM-phys alias | Size |
|---|---|---|---|
| `hub`   | 0x7ec00000 | **0xFEC00000** | 0x4000 |
| `core0` | 0x7ec04000 | **0xFEC04000** | 0x4000 |

Power domain `GRAFX_V3D` (index 10), reset `RESET_V3D`, interrupt **GIC SPI 74** (level-high).
V3D sits just above the `0xFE______` peripheral window AIOS already maps.

Register offsets (from Linux `drivers/gpu/drm/v3d/v3d_regs.h` - the authoritative header):
- **HUB block** (base 0xFEC00000): `V3D_HUB_IDENT0 0x08`, `IDENT1 0x0c` (NCORES, revision -
  read to confirm "4.2"), `IDENT2 0x10` (MMU present), `IDENT3 0x14`. TFU regs live here.
- **MMU** (in hub region): `V3D_MMUC_CONTROL 0x1000` (cache FLUSH/ENABLE), `V3D_MMU_CTL
  0x1200` (TLB_CLEAR), `V3D_MMU_PT_PA_BASE 0x1204` (**physical base of the GPU page table**,
  value = phys >> 12), `V3D_MMU_VIO_ADDR 0x1234` (faulting GPU VA).
- **CORE/CTL block** (base 0xFEC04000): `V3D_CTL_IDENT0/1/2 0x00/04/08` (TMUs/slices/QPUs);
  interrupts `V3D_CTL_INT_STS 0x50 / INT_CLR 0x58 / INT_MSK_* 0x5c..0x64`; **CLE (Control
  List Executor)** - thread 0 = binner, thread 1 = renderer: `CT0CS 0x100 / CT1CS 0x104`
  (control/status), `CT0QBA 0x160 / CT1QBA 0x164` (queue base GPU-VA), `CT0QEA 0x168 /
  CT1QEA 0x16c` (queue end - **writing QEA kicks the job off**), `CT0QMA/QMS 0x170..`
  (binner overflow mem). CSD (compute) regs ~0x900 are not needed for the first triangle.
- **Do NOT reuse vc4 offsets on Pi 4** (vc4 had flat `CT0CA 0x110 / BPCA 0x300`, no MMU,
  no CSD - that's what hackdriver/PeterLemon use).

## 2. How 3D jobs are submitted

**Control lists.** A CL is a packed byte stream of packets (1-byte opcode + payload) the
CLE executes in order. Build two:
- **Bin CL** (CT0): Tile Binning Mode Config -> Start Tile Binning -> state (Clip Window,
  Configuration Bits, Viewport) -> NV Shader State (-> shader record) -> the triangle
  (Vertex Array Primitives) -> Flush/Halt.
- **Render CL** (CT1): Tile Rendering Mode Config (FB GPU-VA, clear flags) -> Clear Colors
  -> per-tile {Tile Coordinates -> load/clear -> Branch to Sub-list -> Store} for every
  tile -> final Store/Halt.

The exact packet IDs/layouts for V3D 4.2 are in Mesa's `src/broadcom/cle/v3d_packet_v42.xml`
(`gen_pack_header.py` generates `v3d_packet_v42_pack.h`). **This XML is the closest thing to
a register-level spec for Pi 4 control lists - treat it as the source of truth.**

**Shader records / NV shaders / QPU programs.** Geometry packets reference a **Shader State
Record** (pointers to fragment/coordinate shader QPU code, uniforms, vertex attribute
descriptors). Simplest path = an **NV ("No Vertex") shader** record: supply already-
transformed screen-space vertices (x,y in 12.4 fixed point, z, 1/w, varyings) directly, so
only a tiny **fragment shader** is needed - the standard "hello triangle" trick. The
fragment shader is a **QPU program** (Broadcom 64-bit VLIW SIMD ISA); for a flat triangle
it's a handful of instructions ending in program-end + thread-switch. **The V3D 4.x QPU
encoding differs from vc4's** - vc4 shader binaries are NOT reusable; Mesa
`src/broadcom/qpu` + `broadcom/compiler` are the reference.

**Role of the firmware at init (power + clock).** Needed for bring-up, not per-job. Linux's
v3d driver at probe: enable the **V3D clock** (firmware **clock id 5**), bring up the
**GRAFX_V3D power domain** (**index 10**), deassert reset, set the MMU page table, enable
IRQs. On the Pi 4 the clock+power are owned by the firmware via the mailbox: power tag
`0x00028001` (set power state) / the `raspberrypi-power` domain packets; QPU-enable
convenience tag `0x00030012`; clock via `0x00038002` (set clock rate) to V3D's operating
freq. **Nuance:** on a normal Pi 4 with `vc4-kms-v3d` the firmware already powers+clocks
V3D, so its registers are live. On a clean AIOS bare-metal boot, **do NOT assume** - assert
the V3D power domain + set the clock via mailbox BEFORE touching 0xFEC0_0000, or register
reads return garbage / the bus hangs (the classic "Unable to enable V3D").

## 3. Can V3D be driven bare-metal (no firmware/Linux)? Yes.

Once powered+clocked, V3D is just MMIO + DRAM data structures. Evidence (with the crucial
IV-vs-VI caveat):
- **macoy.me "Bare metal graphics on Raspberry Pi 4" (PiV3D)** - the most relevant: a
  single-header C V3D interface for the Pi 4, 100k tris @1080p/177fps bare-metal,
  reverse-engineered from Mesa. **Pi-4 correct.** https://macoy.me/blog/programming/PiV3D
- **Random06457/rpi4-gpu-bare-metal-examples** - bare-metal Pi 4 rainbow triangle via the
  V3D control-list path. **Pi-4 correct.** https://github.com/Random06457/rpi4-gpu-bare-metal-examples
- **Mesa `src/broadcom`** (CLE packet XML, v3d Gallium driver, QPU assembler, and the
  **software simulator** `v3d_simulator.c`) - **the authoritative Pi-4 reference.** The
  simulator validates control lists/shader records on a workstation before HW.
- **Linux `drivers/gpu/drm/v3d`** - registers (`v3d_regs.h`), MMU (`v3d_mmu.c`), IRQ, job
  submit for exactly this silicon.
- **phire/hackdriver** + **PeterLemon/RaspberryPi** - great for the *model* but **vc4 (Pi
  1-3) register map + no-MMU memory** - do not copy offsets to Pi 4.
- **Broadcom VideoCore IV AG100-R** - the only official 3D doc, **vc4**; read for pipeline
  concepts, treat registers/encodings as vc4-specific.

**Minimal pipeline (Pi 4):** (1) power+clock V3D via mailbox; (2) map hub+core MMIO, read
IDENT to confirm rev 4.2; (3) allocate low-1GB page-aligned RAM for page table, bin CL,
render CL, tile-alloc memory, shader record, QPU shader, vertex array, framebuffer; (4)
build the GPU page table, write `V3D_MMU_PT_PA_BASE`, flush; (5) build bin CL; (6) build
render CL; (7) submit (CT0QBA/QEA, wait bin-done; then CT1QBA/QEA, wait frame-done via
`V3D_CTL_INT_*` or GIC SPI 74); (8) scan out the linear framebuffer over the existing HDMI
path.

## 4. Options for AIOS (with honest effort)

- **Option A - Minimal custom V3D register-level driver (RECOMMENDED).** Mailbox
  power/clock -> map regs via seL4 device-untyped (mind the ascending-paddr watermark AIOS
  hit before) -> allocate low-1GB contiguous RAM (AIOS already does this for the mailbox FB)
  -> build the page table + program the MMU -> hand-build bin+render CLs + a trivial QPU
  fragment shader -> submit + wait on the done IRQ (GIC SPI 74) -> scan out. Needs only
  device-untyped frames, one IRQ cap, and low contiguous memory - maps cleanly onto AIOS's
  driver-in-root-task model. **Pi-4 cache-coherency gotcha (you've been bitten before):**
  clean the dcache for every buffer handed to V3D before writing QEA, and invalidate before
  reading the framebuffer back - same class of bug as the pipe-SHM all-NUL issue
  [[pipe-shm-cache-coherency]]. **Effort: ~3-6 weeks to first triangle.**
- **Option B - Port Mesa's v3d Gallium driver.** Inherit Gallium/NIR/the v3d compiler +
  implement a DRM/v3d uABI shim (SUBMIT_CL, CREATE_BO, MMAP_BO, WAIT_BO...) + a BO manager +
  winsys. Tens of thousands of LOC, Linux/glibc build system, no GL/DRM in AIOS. **Months,
  high risk.** Only worth it if the goal is a full GLES stack.
- **Option C - Ask the firmware (mailbox/dispmanx/OpenVG).** Investigated: **no usable
  bare-metal 3D path.** The GL/VG libs are closed ARM-side blobs that drive V3D themselves;
  Dispmanx is 2D compositing; the only firmware GPU-exec facility is "execute QPU code"
  (compute, not a 3D pipeline). **Dead end.**
- **Option D - Stay software (CPU rasterizer).** Scanline/half-space triangle rasterizer on
  the A72 writing the existing linear framebuffer. No GPU, fully portable/debuggable. Good
  baseline + a way to get the scene/flip plumbing done. **Days; trivial risk.** (This is
  what the v0.4.169 spinning-cube demo is.)

## 5. Recommended path + phased plan (Option A)

Build the CPU rasterizer (D) first (1-2 days) so framebuffer/flip/scene code exists, then:
- **Phase 0 - Power, registers, IRQ (~3-5 days).** Mailbox bring-up (power domain 10, clock
  id 5, deassert reset). Retype seL4 device-untyped for 0xFEC00000 + 0xFEC04000. Read
  IDENT, confirm rev 4.2 / 8 QPUs. Bind GIC SPI 74 to an seL4 notification. Risk: wrong
  power/clock -> junk reads / bus hang.
- **Phase 1 - GPU memory + MMU (~3-6 days).** Allocate low-1GB page-aligned RAM. Build a
  single-level page table (PTE = phys>>12 | VALID|WRITEABLE), map a scratch buffer, write
  PT_PA_BASE, flush. Validate; deliberately fault an unmapped VA and read VIO_ADDR. Risk:
  cache coherency.
- **Phase 2 - A clear (~3-5 days).** Render-only CL (Tile Rendering Mode Config + clear +
  per-tile Store). Submit via CT1, wait frame-done, verify the HDMI FB shows the clear color.
- **Phase 3 - One shaded triangle (~1-2 weeks).** Hand-assemble a minimal QPU fragment
  shader; build the shader record + NV vertex array (3 pre-transformed verts + per-vertex
  color); build the bin CL; submit bin (CT0) then render (CT1). Cross-check CLs/shader vs the
  Mesa simulator first. **Highest-risk phase** (shader-record packing + QPU ISA).
- **Phase 4 - Spinning cube (~2-4 weeks).** Add a depth buffer, a vertex/coordinate shader
  (or transform on the A72), back-face culling, multiple triangles, double-buffered flip
  synced with HDMI, per-frame rotation uniforms.

## 6. Risks, unknowns, references

**Risks:** no official Pi-4 spec (reverse-engineer from Mesa+Linux); the **IV-vs-VI trap**
(~90% of "Pi GPU triangle" material is vc4 and misleads on registers/MMU/memory/QPU);
**A72<->V3D cache coherency** (flush before submit, invalidate before scanout - highest-
probability "works in sim, garbage on HW" bug, a known AIOS failure mode); power/clock not
asserted; seL4 device-untyped paddr watermark; GPU is 32-bit (low-RAM buffers); slow HW
feedback loop (lean on the simulator + an early MMU-fault dumper); QPU shader assembly is
the steepest sub-task.

**Load-bearing references:**
- Mesa `src/broadcom`: https://docs.mesa3d.org/drivers/v3d.html ;
  https://cgit.freedesktop.org/mesa/mesa/tree/src/broadcom/cle/gen_pack_header.py (-> v3d_packet_v42.xml)
- Linux `drm/v3d`: https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/v3d/v3d_regs.h ;
  https://lxr.missinglinkelectronics.com/linux/drivers/gpu/drm/v3d/v3d_mmu.c ;
  https://docs.kernel.org/gpu/v3d.html
- BCM2711 device tree (V3D base/IRQ/power): https://github.com/raspberrypi/linux/blob/rpi-6.6.y/arch/arm/boot/dts/broadcom/bcm2711.dtsi
- macoy.me PiV3D: https://macoy.me/blog/programming/PiV3D ; PiGPU: https://macoy.me/blog/programming/PiGPU
- Random06457 Pi-4 bare-metal: https://github.com/Random06457/rpi4-gpu-bare-metal-examples
- phire/hackdriver (vc4 model): https://github.com/phire/hackdriver
- Broadcom VideoCore IV AG100-R: https://docs.broadcom.com/docs-and-downloads/docs/support/videocore/VideoCoreIV-AG100-R.pdf
- Life of a Triangle (TBR walkthrough): https://jbush001.github.io/2016/02/27/life-of-triangle.html
- Mailbox property interface (power/clock/QPU tags): https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
