# V3D Phase 2 golden CL fixtures -- provenance

`v3d_clear_bin.golden` (17 B) and `v3d_clear_render.golden` (165 B) are the
byte-exact binning + render control lists for a 1024x768 GPU clear-to-framebuffer
job, used by `scripts/v3d_clcheck.py` to gate `src/gpu/v3d_cl.c`.

## How they were generated

Compiled the UNMODIFIED CL-encoding headers from
[Random06457/rpi4-gpu-bare-metal-examples](https://github.com/Random06457/rpi4-gpu-bare-metal-examples)
(MIT) host-side (`src/device/v3d/v3d_cl.hpp`, `src/kernel/cl_emitter.hpp`), driving
them to emit the clear packet sequence from `docs/DESIGN_V3D_IMPLEMENTATION.md` sec 8.
Only `v3d_memory.hpp` was replaced with a host shim (deterministic `ARM_TO_BUS`);
all field encodings are theirs. Output is deterministic across runs.

## Scene / synthetic addresses (must match `tools/v3d_host_clgen.c`)

- 1024x768, 32bpp, BGR; 64x64 tiles (16x12=192); 4x4-tile supertiles (4x3=12)
- clear color orange `0xFFFF8000`
- tile-alloc (PTB) base `0x00110000`; framebuffer / RT0 `0x10000000`; stride 4096
- render CL base `0x00210000` (packet 20 self-references the generic sub-list here)

## sha256

```
be004acff16ee7c214c3f9585bf2c7f09e8e8253e31816f0503bc424ee03ca99  v3d_clear_bin.golden
3e34512170007624ed1e07e6acdba94544c1c397e707977a074fbfbf470db29d  v3d_clear_render.golden
```

## Regenerating

`git clone --depth 1 https://github.com/Random06457/rpi4-gpu-bare-metal-examples`,
then the host harness + shim captured this session (see the `project_v3d_phase2`
memory for the exact build recipe). Only needed if the clear packet sequence
changes; the fixtures are the frozen ground truth otherwise.
