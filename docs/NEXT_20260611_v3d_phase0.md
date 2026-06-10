# NEXT: V3D Phase 0 — power, IDENT, IRQ (prompt seed)

Paste-able kickoff for a fresh session:

> On branch `design/rpi4-v3d-driver`, implement V3D Phase 0 exactly per
> `docs/NEXT_20260611_v3d_phase0.md`. Design authority is
> `docs/DESIGN_V3D_IMPLEMENTATION.md` (do NOT build from `DESIGN_RPI4_3D.md` —
> it is partially superseded; see its banner). Land the code, get BOTH builds
> green, get the QEMU plumbing test passing, then stop and hand me the kernel8
> swap instructions. After I boot the Pi, drive the HW probes over netconsole.

## Mission

Prove the V3D power story on real hardware: `/proc/v3d.power` flips
`V3D_HUB_IDENT0` from the dead-bus poison `0xDEADBEEF` to `0x04443356` and
decodes a V3D 4.2 with 8 QPUs. No MMU, no control lists, no rendering — that
is Phases 1–3.

**In scope**: DTB parse + fallback, device-map claims, `v3d_init()` (claims,
IRQ bind, tag-buffer pin — zero V3D MMIO at boot), `v3d_ensure_on()` power
sequence, `/proc/v3d` + `/proc/v3d.power[.N]` + register peek/poke verbs,
optional clock verb, CMake wiring, QEMU not-present path, QEMU test script.

**Out of scope**: `v3d_mmu.c`, `v3d_cl.c`, `v3d_shaders.h`, DISP_V3D_* IPC
labels, fbshow flags, the 8MB pool (`v3d_mem_reserve` may land as a stub or be
deferred to Phase 1 — your call; Phase 0 needs only the 1-page tag buffer).

## Read first (in order)

1. `docs/DESIGN_V3D_IMPLEMENTATION.md` — sections 0–3 (architecture, files),
   7 (/proc verbs + threading regimes), 8 Phase 0, 9 (validation), 10 (risks).
2. `src/boot/boot_device_map.c` — the claim table + ascending watermark.
3. `src/boot/boot_dtb.c` — `parse_genet` (:199-214) and the PCIe fixed-constant
   fallback (:267-289); clone these patterns.
4. `src/usb/xhci.c` — diag verb impl + `LOG_MODULE` conventions; `net_genet.c`
   :703-741 — the IRQ binding recipe.
5. `src/procfs.c` — entry registration; the `path[1]=='3'` disambiguation need
   (`version`/`vka` prefix collisions).

## Verified HW facts (do not re-derive; evidence in the design doc §1)

| Item | Value |
|---|---|
| V3D hub MMIO | 0xFEC00000, 0x4000 (IDENT0 at +0x08) |
| V3D core0 MMIO | 0xFEC04000, 0x4000 (claim hub+core as ONE 8-page region) |
| RPiVid ASB | 0xFEC11000, 1 page (V3D_S_CTRL +0x08, V3D_M_CTRL +0x0C) |
| PM_GRAFX | `dev_pm_vaddr` + 0x10C (PM block 0xFE100000 ALREADY mapped) |
| PM password | 0x5A000000 OR'd into EVERY PM and ASB write |
| Power sequence | PM_GRAFX \|= V3DRSTN(BIT 6) → ≥2us delay (cntpct_el0) → ASB **master** +0x0C clear REQ_STOP(BIT 0), poll ACK(BIT 1) clear ≤10ms → ASB **slave** +0x08 same. Order: reset, delay, master, slave |
| IDENT0 magic | 0x04443356 powered; 0xDEADBEEF = not powered (reads are SAFE pre-power; the hazard is wrong/misordered WRITES) |
| IDENT1 decode | TVER bits 3:0 = 4, REV bits 7:4 = 2, NCORES bits 11:8 = 1; core CTL_IDENT1: QPUs = NSLC[7:4] × QUPS[11:8] = 8 |
| IRQ | GIC SPI 74 → seL4 IRQ **106**; bind at init, leave everything MASKED, counter must stay 0 (seL4_Poll the ntfn in diag to sample it) |
| Clock (optional) | mailbox SET_CLOCK_RATE 0x00038002 {5, 500000000, 0}; tag buffer pinned at **0x3A002000** (after display 0x3A000000 and PCIe 0x3A001000 in that untyped's forward-only watermark); failure = warning, not fatal |
| Mailbox power tags | DO NOT USE — 0x00028001 has no V3D ID; ENABLE_QPU 0x00030012 does nothing on Pi 4 |

## Implementation checklist (ordered)

1. `include/aios/hw_info.h` + `src/boot/boot_dtb.c`: `has_v3d/v3d_paddr/v3d_irq`;
   `parse_v3d()` (compatible `brcm,2711-v3d`, VC-bus +0x80000000 fixup, irq+32)
   **plus fixed-constant fallback** (0xFEC00000 / 106) — the firmware runtime DTB
   strips nodes and downstream trees move v3d under `v3dbus`.
2. `src/boot/boot_device_map.c`: `reqs[8]` → `reqs[12]`, fix the stale `n <= 6`
   comment at :89, add `v3d` (8 pages @ 0xFEC00000) + `v3dasb` (1 page @
   0xFEC11000) gated on `has_v3d`. Both sit above eMMC 0xFE340000 — watermark
   safe, but go through the sorted table. `include/aios/device_map.h` externs.
3. New `src/gpu/v3d_regs.h` (Phase 0 subset: hub IDENT0–3, hub INT block
   0x50–0x64, core CTL_IDENT0–2, core INT block; transcribe from Linux
   `v3d_regs.h`, document the FRDONE=BIT(0)/FLDONE=BIT(1) trap at the defines).
4. New `src/gpu/v3d.h` + `src/gpu/v3d.c`: `v3d_init()` (gate on `has_v3d`;
   claims already mapped by prealloc — bail-if-not-premapped, GENET rule; bind
   IRQ 106 via the net_genet recipe; pin tag buffer via
   `sel4platsupport_alloc_frame_at`; NO V3D register access at boot),
   `v3d_ensure_on()` (idempotent; the exact sequence above; micro-poke split for
   `.power.N`; abort with `v3d_ok=0` on IDENT mismatch — never proceed against a
   dead bus), `v3d_diag_cmd()`. `LOG_MODULE "v3d"`, all waits through one
   `v3d_wait(cond_fn, ms)` helper on `cntpct_el0`/`mono_deadline_ms` — no raw
   loops, no iteration counts (eMMC 32.6s lesson).
5. `src/aios_root.c`: call `v3d_init()` after the xhci_init block (~:375),
   before `boot_start_services` (:406).
6. `src/procfs.c`: `/proc/v3d` (summary: IDENT0 flagged DEAD/OK, IDENT decode,
   both INT_STS raw, irq counter, `v3d_ok`), `/proc/v3d.power` (read-before →
   sequence → read-after + ASB ACK timings, prints PASS/FAIL),
   `/proc/v3d.power.N` (step N only, for SError bisection), `/proc/v3d.r.<off>`
   / `.c.<off>` / `.w.<off>.<val>`, `/proc/v3d.clock[.N]`. Phase 0 = bring-up
   regime: verbs run direct on the fs thread (nothing else touches V3D; none
   touch the FB). Match on `path[1]=='3'`.
7. `projects/aios/CMakeLists.txt`: add `src/gpu/v3d.c` to the `aios_root` list
   (always compiled, xhci.c precedent at :173); `#ifdef PLAT_RPI4` only around
   the PM/ASB/mailbox specifics. On QEMU `has_v3d=0` → init no-ops →
   `/proc/v3d` prints `v3d: not present`.
8. `scripts/v3d_qemu_test.py` (house script style: python, no apostrophes in
   comments, OK/FAIL output): boot build-04 → `cat /proc/v3d` shows not-present
   → `cat /proc/v3d.power` refuses gracefully → `fbshow --cube` still works →
   exit nonzero on any FAIL.

## Verification

**QEMU (you, before any HW)**: `ninja -C build-04 && ninja -C build-rpi4` both
green; `scripts/v3d_qemu_test.py` passes; existing suites unaffected.

**HW (Bryan boots, then you drive over netconsole port 2323 — gently: one held
connection per command, ~4s settle between)**:
1. Bryan: `ninja -C build-rpi4 && python3 scripts/mkkernel8.py`, copy
   `kernel8.img` to AIOSBOOT, boot the Pi.
2. You: `cat /proc/v3d` → expect `DEAD (0xDEADBEEF)`, irq=0, v3d_ok=0.
3. You: `cat /proc/v3d.power` → expect
   `ident0 0xDEADBEEF -> 0x04443356 ver=4.2 cores=1 qpus=8 mmu=1 PASS` with ASB
   acks < 1ms.
4. You: `cat /proc/v3d` again → OK state; rerun `.power` (idempotency); `reboot`
   over netconsole, repeat 2–3 once.
5. Regression: ping 0% loss, netconsole echo, one `ssh -tt` session, USB
   keyboard types on HDMI, `fbshow --cube` runs, `date` sane, `reboot` works.

## Failure playbook

- IDENT stays 0xDEADBEEF after `.power` → dump PM_GRAFX/ASB readbacks + ACK
  timings (build these into the verb output from the start).
- Kernel halt (SError) on `.power.N` → step N identifies the faulting access;
  Bryan power-cycles; bisect with N-1. Never put the sequence in the boot path.
- IDENT reads 0x0 → mapping bug; check the boot log for the `v3d` prealloc line.
- ASB ACK timeout → report `asb_timeout=1` with elapsed; do not retry-loop.
- Anything haunted across kernel8 swaps in boot/display handover → full reflash
  (balenaEtcher, never dd) before deep-debugging — the scroll-freeze ghost.

## Exit criteria + commit

IDENT magic verified on real HW, idempotent + reboot-stable, ASB acks < 1ms,
IRQ counter 0, QEMU test + full regression green, both builds green. Commit as
the next `v0.4.19x` version bump on this branch; update HANDOVER's pending
item 6 to "Phase 0 DONE, next Phase 1 (MMU + fault probe)" and write the next
NEXT doc pointing at design §8 Phase 1.
