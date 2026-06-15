# NEXT: session seed -- V3D Phase 4a finish (confirm/commit the cube) + Phase 4b

Paste the brief below into a fresh session. Then read `HANDOVER.md` (top), the memory
index (`MEMORY.md`, auto-loaded), the named memories, and
`docs/DESIGN_V3D_IMPLEMENTATION.md` section 8 (Phase 4a/4b).

---

## Paste-this brief

AIOS (research microkernel OS on seL4, repo `~/Desktop/github_repos/AIOS`, branch
`main`, at **v0.4.252**; Bryan pushes via GitHub Desktop -- commit only when asked,
never amend / force-push, no apostrophes in C comments). Develop + verify on the HOST
(QEMU has NO V3D model); deploy to the real Pi FLASH-FREE over the network
(`scripts/pi_flash.py`). The Pi runs **v0.4.252 / build 2373** at **192.168.0.8**
(netconsole 2323, sshd 2222 pw root). NOTE: the DHCP lease BOUNCES between `.8` and
`.250` per boot -- if `.8` is dark, ARP-sweep the /24 for MAC `dc:a6:32:1c:2e:e1`
(`pi_flash.py` already scans). Drive netconsole GENTLY: one held connection, MANY
commands per connection, ~45-55s of rest between connections, NEVER `nc -z`.

**The V3D hardware-3D project is at the finish line.** Phase 2 (GPU clear) and Phase 3
(rainbow triangle, GL Shader State path) are DONE + HW-verified + committed. Phase 4a
(the spinning cube = THE PROJECT DELIVERABLE) is code-committed and HW-RENDERED, with
ONE thing left.

### Immediate task (finish Phase 4a)

The cube already ran on HW: `/proc/v3d.cube` -> **150 frames, 0 OUTOMEM, 0 MMU faults,
0 resets, status=OK PASS** (~1.1 ms/frame, paced ~60 fps). What is NOT yet confirmed:
**is the cube visually correct (a solid spinning 3-D cube) or INSIDE-OUT** (you see the
far faces through it)? The render is proven; only the backface-cull WINDING is
HW-determined (the Y viewport is flipped, so screen winding is inverted vs object space).

1. Re-spin it and look at the monitor: drive `cat /proc/v3d.power` -> `.mmu` ->
   `cat /proc/v3d.cube.300` (300 frames ~5s) over netconsole and watch the HDMI.
   (Use the `/tmp/v3d_tri.py` driver pattern from the prior session; the `.cube` poll
   is capped at 4s so harvest the PASS line with a follow-up `cat /proc/v3d`.)
2. **If the cube is inside-out**: flip the cull winding -- in `src/gpu/v3d_cl.c`
   `v3d_build_triangle_bin_cl`, the CFG_BITS line emits `p->cull ? 0x05 : 0x07`; change
   the cube value `0x05` -> `0x01` (byte0 bit 2 = clockwise_primitives; do NOT touch
   bit 1, that re-enables back faces). Rebuild `build-rpi4-netd`, `mkkernel8.py`,
   `pi_flash.py`, re-spin, re-confirm. (Equivalently you could swap the triangle vertex
   order in `v3d_cube.c` `CUBE_FACE`, but the CFG_BITS bit is the one-line knob.)
3. **Commit 4a-C** once it looks right: `include/aios/version.h` is already bumped to
   252 (UNCOMMITTED -- the flashed kernel) plus any winding fix. Commit as
   "v3d: v0.4.252 -- Phase 4a spinning cube HW-VERIFIED (the deliverable)".

That closes the project deliverable: **clear -> triangle -> cube, all GPU-rendered on
real V3D silicon, drivable over netconsole.**

### Then (optional follow-ups, design sec 8 + 11)

- **Phase 4b -- double-buffer via mailbox panning** (tear-reduction; also the
  backlogged HDMI scroll-perf fix). Request `virt_h = 2*768` at display init, map both
  halves into GPU VA, render to the back half, `plat_display_pan(back_y)` via
  `TAG_SET_VIRT_OFF` on RFC. Optional -- 4a single-buffer (tearing accepted) is the
  shipped deliverable.
- Bigger/tunable cube: `CUBE_FOCAL_Q16` in `v3d_cube.c` sets size (currently ~0.65 of
  screen). Per-frame depth (true depth test, on-chip Z) is NOT needed for a convex cube
  but is the path if non-convex geometry is ever wanted.
- Retire the V3D bring-up scaffolding if desired (the `.power`/`.mmu`/`.fault`/`.cl`
  diag verbs can stay -- they are harmless and useful).

### Architecture recap (what is already built)

- `src/gpu/v3d.c` -- the driver: power/IDENT/IRQ, the 8 MB MMU pool + BO bump
  allocator, `v3d_run_cls` (the shared bin+render doorbell/wait/recovery core, reused
  by clear + triangle + cube), `v3d_submit_frame` (clear), `v3d_submit_triangle`,
  `v3d_submit_cube_frame` + `v3d_cube_run`, `v3d_dump_and_reset`, the `/proc/v3d`
  dispatcher + the request-flag wake (kind 1=clear/2=triangle/3=cube; fs thread sets
  `g_v3d_req` + `seL4_Signal`s a notification bound to the display TCB -- non-MCS
  reply-cap-safe). All waits cntpct-deadline-bounded, never iteration counts.
- `src/gpu/v3d_cl.{c,h}` -- the byte-exact CLE emitters (clear + triangle/cube, the
  latter parameterized with `cull` + `skip_z`). Explicit byte writes (no bitfields) so
  host + kernel agree. `v3d_i2f` = FP-free int->float.
- `src/gpu/v3d_cube.{c,h}` -- cube geometry + the FP-free per-frame transform.
- `src/gpu/v3d_shaders.h` -- the 3 Random06457 QPU blobs + triangle geometry.
- `tools/v3d_ref/` -- the SELF-CONTAINED Random06457 (MIT) reference harness;
  `cd tools/v3d_ref && sh build.sh && ./gen ; python3 decode.py` regenerates +
  decodes every golden.
- IPC: `DISP_V3D_CLEAR 118 / TRI 119 / CUBE 120 / RELEASE 121` on `disp_ep`;
  `fbshow --gpu-clear|--gpu-tri|--gpu-cube [N]|--gpu-release` (disk app -- needs a
  `pi_filexfer.py push` if you use it; the `.test/.tri/.cube` /proc verbs are
  kernel-resident and need NO push).

### Verification (run before every commit / HW deploy)

- Host: `python3 scripts/v3d_clcheck.py` (8 CLs byte-exact + cube-transform sanity);
  `ninja -C build-04 && ninja -C build-rpi4-netd` (+ `build-netd`, `build-rpi4`) green;
  `python3 scripts/v3d_qemu_test.py` (15/15 graceful refusal; rebuild the QEMU disk
  with `mkdisk.py` first if `fbshow` changed).
- HW: flash `build-rpi4-netd` (`mkkernel8.py --kernel
  build-rpi4-netd/images/aios_root-image-arm-bcm2711 --output disk/kernel8.img` then
  `pi_flash.py --host 192.168.0.8`), drive netconsole gently. Regression: ping,
  `ssh -tt`, `fbshow --cube` (CPU demo) still runs, `fbshow --gpu-release` restores the
  console, reboot.

### State to verify (point-in-time)

- `main` ahead of `origin/main` (`26ac58b`) by 4 (Phase 3 B/C + Phase 4a A/B) PLUS an
  uncommitted `include/aios/version.h` (252). Bryan pushes.
- Key commits: `920a5d2` Phase 2, `26ac58b`/`8cfc66e`/`15ea47f` Phase 3 A/B/C,
  `89a3769`/`cf1d904` Phase 4a A/B.
- The Pi runs the cube kernel (v0.4.252). The HDMI shows the last cube frame (console
  suspended) until a legacy draw or `fbshow --gpu-release` / reboot.
- Memories: `project_v3d_phase4`, `project_v3d_phase3`, `project_v3d_phase2`,
  `project_v3d_design`, `project_usb_hid` (boot-only USB enum gotcha).
