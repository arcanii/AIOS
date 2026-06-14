# NEXT: session seed -- V3D Phase 3 Stage A (the byte-exact triangle emitter)

Paste the brief below into a fresh session. Then read `HANDOVER.md` (top), the memory
index (`MEMORY.md`, auto-loaded) + the named memories `project_v3d_phase3`,
`project_v3d_phase2`, and `docs/DESIGN_V3D_IMPLEMENTATION.md` section 8 (Phase 3).

---

## Paste-this brief

AIOS (research microkernel OS on seL4, repo `~/Desktop/github_repos/AIOS`, branch
`main`). Bryan pushes via GitHub Desktop -- commit only when asked, never amend /
force-push, no apostrophes in C comments. Develop + verify on the HOST (QEMU cannot
model V3D at all). The Pi runs **v0.4.250** at **192.168.0.8** (netconsole 2323, sshd
2222 pw root), healthy.

**Phase 2 (GPU clear -> first orange pixels) is DONE + HW-VERIFIED + pushed (920a5d2).**
**Goal now: V3D Phase 3 Stage A -- the byte-exact host-side triangle CL emitter + golden
gate (NO hardware risk).** Phase 3 = the rainbow triangle via the GL Shader State path
(design sec 8, the "highest-risk" phase). Same front-loaded shape as Phase 2: lock CL
correctness host-side before any Pi flash.

### What is already DONE (build on it, do not redo)

- **A1 foundation** (on disk, UNTRACKED -- commit with the Random06457 MIT NOTICE when
  finalizing): `tests/fixtures/v3d_triangle_{bin,render}.golden`; `src/gpu/v3d_shaders.h`
  (the 3 QPU blobs frag/vtx/coord VERBATIM from gl_rainbow_triangle.cpp + the triangle
  geometry).
- **The full byte-exact reference is preserved + self-contained** at `tools/v3d_ref/`
  (rescued from ephemeral /tmp; also backed up `~/v3d_ref_backup/`):
  `cd tools/v3d_ref && sh build.sh && ./gen` regenerates all 4 goldens byte-exact;
  `python3 decode.py` prints the full packet-by-packet decode (clear + triangle, bin +
  render). Field bit-layouts: `tools/v3d_ref/ref_src/device/v3d/v3d_cl.hpp`. The exact
  triangle scene + GL Shader State Record + Attribute Records (field by field) +
  buffer layout are in `tools/v3d_ref/gen.cpp` `buildTriangle()`.
- **The clear emitter exists**: `src/gpu/v3d_cl.c` has `v3d_build_bin_cl` +
  `v3d_build_render_cl` (clear), byte-exact-gated by `scripts/v3d_clcheck.py` vs
  `tests/fixtures/v3d_clear_*.golden`. The `rb_swap` field is parameterized (kernel=0
  for AIOS BGR FB, host clgen=1 to match the golden) -- the triangle reuses this.

### Stage A tasks (host-only; the golden gate is the only check)

1. **A2 -- triangle emitters** in `src/gpu/v3d_cl.{c,h}`: `v3d_build_triangle_bin_cl`,
   `v3d_build_triangle_render_cl`, a GL Shader State Record (36 B) emitter, an Attribute
   Record (16 B) emitter -- all parameterized by addresses (like the clear takes
   `tile_alloc_va`). Reproduce the decoded golden bytes EXACTLY.
   - **Bin CL = 24 packets**: clear's first 4 (120 w/h, 19, 92, 6) then 107 CLIP_WINDOW,
     96 CFG_BITS(077002), 104 POINT_SIZE(1.0), 105 LINE_WIDTH(1.0), 110 CLIPPER_XY_SCALING
     ((w/2)*256, (h/2)*-256), 111 CLIPPER_Z_SCALE_AND_OFFSET(.5,.5), 109 CLIPPER_Z_MIN_MAX
     (0,1), 108 VIEWPORT_OFFSET(w/2,h/2), 87 COLOR_WRITE_MASKS(0), 86 BLEND_CONSTANT(0),
     97/99/88 ZERO_ALL flat/nonpersp/centroid, 74 TRANSFORM_FEEDBACK(0), 92 OCCLUSION(0),
     91 SAMPLE_STATE(0f00803f), 71 VCM_CACHE_SIZE(44), 64 GL_SHADER_STATE
     (record_addr 32B-aligned | nattr in low 5 bits), 36 VERTEX_ARRAY_PRIMS(mode 4, count
     3, first 0), 4 FLUSH.
   - **FOUR structural deltas vs the clear emitter** (these bit you): (a) render TRMC
     order is Common -> CLEAR_COLORS -> COLOR -> ZS (clear was Common -> COLOR ->
     CLEAR_COLORS -> ZS); (b) the generic tile list lives in a SEPARATE indirect buffer
     referenced by START_ADDRESS_OF_GENERIC_TILE_LIST(20), NOT inline; (c) a SECOND
     STORE_TILE_BUFFER_GENERAL for the Z buffer (D16, UIF_XOR); (d) keep `rb_swap`
     parameterized (kernel 0 / clgen 1). Triangle golden clear color = 0xff101010;
     500x500 -> 8x8 tiles, 1x1-tile supertiles -> 64 SUPERTILE_COORDINATES.
2. **A3 -- the gate**: extend `tools/v3d_host_clgen.c` to replicate `buildTriangle`'s
   buffer layout (compute the synthetic addresses the golden used) + emit via the new
   triangle emitters; extend `scripts/v3d_clcheck.py` to byte-diff vs the triangle
   fixtures. **Exit Stage A = `python3 scripts/v3d_clcheck.py` byte-exact on the triangle
   bin + render CLs.** Then build all 4 trees green (`ninja -C build-04` etc.).

### Then (later sessions)

- **Stage B (kernel, QEMU-graceful)**: new BOs (shaders/record/attrs/default-attrs/
  uniforms/vertex/Z), triangle submit (reuse `v3d_submit_frame` + the post-render L2T
  flush), `DISP_V3D_TRI 119` + `/proc/v3d.tri` + `fbshow --gpu-tri`; v3d_qemu_test
  graceful refusal.
- **Stage C (HW)**: flash (`mkkernel8.py` + `pi_flash.py --host 192.168.0.8`) + push
  fbshow + drive netconsole GENTLY (one held conn, ~45s rest between, never `nc -z`)
  -> rainbow triangle on HDMI; probe center != clear color, 3 corners == clear color.
  Use `/tmp/v3d_kick2.py` pattern from the Phase 2 session as the driver.

### Verification

- Host: `cd tools/v3d_ref && sh build.sh && python3 decode.py` (the reference);
  `python3 scripts/v3d_clcheck.py` (the gate, after A3); all 4 trees green.
- QEMU: `python3 scripts/v3d_qemu_test.py` (after Stage B adds the verbs).

### State to verify (point-in-time)

- `main` at **920a5d2** (Phase 2), pushed (origin/main == local). Phase 3 foundation
  files UNTRACKED: `src/gpu/v3d_shaders.h`, `tests/fixtures/v3d_triangle_*.golden`,
  `tools/v3d_ref/`.
- Key memories: `project_v3d_phase3` (the A2 scope + the 4 deltas), `project_v3d_phase2`,
  `project_v3d_design`. The byte-exact decode is the source of truth -- regenerate it,
  do not work from memory of the packet bytes.
