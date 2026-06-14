# NEXT: session seed -- V3D Phase 2, the GPU kick (first visible pixels)

Paste the brief below into a fresh session. Then read `HANDOVER.md` (top), the
memory index (`MEMORY.md`, auto-loaded), the named memories, and
`docs/DESIGN_V3D_IMPLEMENTATION.md` section 8 (Phase 2), 5 (cache contract),
6 (IPC), 10 (risks).

---

## Paste-this brief

AIOS (research microkernel OS on seL4, repo `~/Desktop/github_repos/AIOS`, branch
`main`, at **v0.4.248**; Bryan pushes via GitHub Desktop -- commit only when asked,
never amend / force-push, no apostrophes in C comments). Develop + verify on QEMU;
deploy to the real Pi FLASH-FREE over the network (`scripts/pi_flash.py` for the
kernel, `scripts/pi_filexfer.py` for disk apps). The Pi runs **build 2301 /
v0.4.248** at **192.168.0.8** (netconsole 2323, sshd 2222 pw root), healthy.

**Goal this session: finish V3D Phase 2 -- the GPU kick -> the first VISIBLE
GPU-rendered pixels (a solid orange clear on the HDMI monitor).** Everything up to
the actual GPU submission is DONE and HW-verified; the CL bytes are byte-exact
gated, so this is about submission / MMU-on-store / cache coherency, NOT control-
list correctness.

### What is already DONE + HW-verified (build on it, do not redo)

- **Phase 0 (power/IDENT/IRQ)** + **Phase 1 (MMU + deliberate fault)** -- HW-verified.
  VIO_ADDR decode fixed v0.4.245 (`raw << (va_width-32)`, va_width=35 from
  MMU_DEBUG_INFO). See [[project_v3d_phase0]].
- **Phase 2 foundation** (see [[project_v3d_phase2]], commits 29ecea6 + 309464d):
  - `src/gpu/v3d_cl.{c,h}` -- bin + render CLEAR control-list emitters
    (`v3d_build_bin_cl`, `v3d_build_render_cl`), pure portable C (compile host +
    kernel). BYTE-EXACT against the Random06457-derived golden fixture
    (`tests/fixtures/v3d_clear_{bin,render}.golden`) via `scripts/v3d_clcheck.py`
    (the only CL-correctness gate -- QEMU has no V3D model). Run it after ANY CL
    change.
  - `src/gpu/v3d_regs.h` -- Phase 2 registers already transcribed from Linux:
    CT0QTS 0x15c, CT0RA/CT1RA, CT0LC/PC, PTB BPCA/BPCS/BPOA 0x308/BPOS 0x30c,
    CTL SLCACTL 0x024, L2TCACTL 0x030 (+ L2TFLSTA/END, FLM bits), plus the
    existing CT0/CT1 CS/CA/QBA/QEA/QMA/QMS, BFC 0x134, RFC 0x138.
  - `src/gpu/v3d.c` -- a BO bump allocator (`v3d_bo_alloc`/`v3d_bo_reset_frame`,
    GPU VA = `V3D_VA_DATA + (off - V3D_SCRATCH_OFF)`); the live framebuffer mapped
    into GPU VA `V3D_VA_FB = 0x10000000` inside `v3d_mmu_init` (reads `gpu_fb_pa`
    live -- firmware-chosen, ~0x3e8c7000 on the Pi); and a NO-KICK dry-run verb
    `/proc/v3d.cl` that builds the CLs + maps the FB and dumps the bytes.
  - **HW-verified via `/proc/v3d.cl`**: in-kernel bin_cl bytes == golden_bin
    exactly, FB mapped (pa 0x3e8c7000, 768 pages), BO allocator works.

### What REMAINS -- the kick (this session)

Per `DESIGN_V3D_IMPLEMENTATION.md` section 8 Phase 2 + [[project_v3d_phase2]].
DECISIONS already made (do not relitigate): run the clear on the **display_server
thread** (the single FB writer), NOT the fs thread; trigger via a request-flag
verb **`/proc/v3d.test`** (kernel-resident) + keep `fbshow --gpu-clear` as the
user-facing verb (disk app -> push). Clear color = orange **0xFFFF8000** (BGR,
R/B-asymmetric so a channel-order bug fails the pixel probe instead of hiding).

1. **Step 7 -- console-suspend / cache-ownership protocol.** `fb_console_set_suspend(1)`
   gates console render/scroll/flush; on first V3D op `gpu_fb_flush_all()`
   (clean all FB lines to PoC) then suspend, so no dirty CPU line writes back over
   GPU pixels; every legacy CPU draw op clears the ownership flag; release path
   un-suspends + `fb_console_clear()`. Files: `src/boot/fb_console.c`,
   `src/boot/boot_display_init.c`, `src/servers/display_server.c`.
2. **Step 8 -- `v3d_submit_frame`** (in `v3d.c`): ensure powered + `v3d_mmu_init`;
   build bin+render CLs into BO buffers (emitters exist); `v3d_invalidate_gpu_caches`
   (SLCACTL all-invalidate + L2TCACTL flush); **bin bracket** = PTB_BPOS=0, CT0QTS
   (tile-state base+enable), CT0QMA/QMS, CT0QBA -> CT0QEA (write QEA = kick), wait
   BFC increment (100 ms deadline), OUTOMEM (core INT bit2) -> supply BPOA/BPOS
   refill + W1C, abort on a second OUTOMEM; **render bracket** = CT1QBA -> CT1QEA
   (kick), wait RFC increment (250 ms); `v3d_dump_and_reset` on timeout
   (CT*CA-minus-QBA, CT*RA, both INT blocks, VIO_*, scratch first-words). EVERY
   wait deadline-bounded on `cntpct` / `v3d_wait` -- NEVER iteration counts
   (the eMMC 32.6s scar, see [[feedback_emmc_completion_timeout]]).
3. **Step 9 -- trigger + pixel probe.** `/proc/v3d.test` posts a request flag
   consumed at the top of `display_server_fn`, which runs takeover + `v3d_submit_frame`,
   then a pixel probe: CleanInvalidate the probed FB page, read `pixel[512,384]`,
   compare to 0xFFFF8000 in BGR memory order. Result line `bfc a->b rfc a->b
   bin_us rend_us pixel=... PASS/FAIL` into a stats struct, harvested via
   `cat /proc/v3d`. `fbshow --gpu-clear [RRGGBB]` mirrors `--clear` (push the app).
4. **Step 10 -- QEMU smoke**: extend `scripts/v3d_qemu_test.py` -- `/proc/v3d.test`
   and `fbshow --gpu-clear` must refuse gracefully (has_v3d=0); never assert a
   pixel on QEMU.

Success = **solid orange on the monitor**, RFC advanced, pixel-probe PASS, 0 MMU
faults, repeatable. Then Phase 3 (triangle) -> Phase 4a (spinning cube = the
project deliverable).

## Risks (DESIGN sec 10 -- the ones that bite here)

- **Display wedge (R1):** a GPU hang inside the display_server handler stalls tty
  `seL4_Call`s -> HDMI + USB dead. Mitigate: every wait `v3d_wait`-bounded;
  `dump_and_reset` returns the handler <=350 ms; the net path survives (drive +
  triage over netconsole/ssh while HDMI is dead).
- **Silent scratch redirect (R3):** RFC advances but NO pixels -> stores went to
  the ILLEGAL_ADDR scratch page (wrong RT VA). ALWAYS pair the PASS line with the
  center-pixel probe + a scratch first-words dump; never trust RFC alone.
- **FB cache coherency (R5):** CPU must not dirty the FB while the GPU owns it ->
  the step-7 suspend + flush-all is load-bearing.

## Workflow / deploy

- **Trees:** `build-04` (flag-OFF QEMU), `build-netd` (flag-ON QEMU), `build-rpi4`
  (flag-OFF RPi4), `build-rpi4-netd` (flag-ON RPi4 = the FLASH TARGET). `AIOS_NETD`
  defaults ON; flag-OFF trees need `-DAIOS_NETD=OFF`. Build BOTH a QEMU + an RPi4
  tree after shared-code changes; `v3d.c`/`v3d_cl.c` are always-compiled,
  `#ifdef PLAT_RPI4` only around the MMIO.
- **Deploy:** driver/display_server/procfs changes = a KERNEL FLASH
  (`mkkernel8.py --kernel build-rpi4-netd/images/aios_root-image-arm-bcm2711
  --output disk/kernel8.img` then `pi_flash.py --host 192.168.0.8`; bumps
  version.h). `fbshow` = a disk-app PUSH (`pi_filexfer.py push <local>
  /bin/aios/fbshow 192.168.0.8`). A kick that touches both needs a flash AND a push.
- **netconsole is wedge-prone** (the ~32s TLBI stall + an 8-slot socket cap).
  Drive GENTLY: one held connection, MANY commands per connection, ~45s of NO
  connections between connections; NEVER `nc -z` (it wedges netconsole). A heavy
  command (and a wedge) can give "0 bytes buffered" -- the box is usually alive
  (ping it); rest ~2 min and retry. `scripts/v3d_probe.py` hardcodes a 30s cmd
  timeout -- for a long verb write a probe with a bigger timeout (see the
  `/tmp/ft_hw.py` pattern in the v0.4.248 session). Reuse the robust driver in
  `scripts/gov_cooling.py` / `fatconfig_hw.py` for resilient driving.
- **QEMU cannot model V3D at all** -- the host golden-CL gate + the QEMU graceful-
  refusal are the only pre-HW checks; the visible clear is HW-only.

## Verification

- Host: `python3 scripts/v3d_clcheck.py` (byte-exact, after any CL change);
  `ninja -C build-04 && ninja -C build-rpi4-netd` green;
  `python3 scripts/v3d_qemu_test.py` green (graceful-refusal of the new verbs).
- HW (after flash + push, over netconsole 2323, gently): power -> mmu -> `.cl`
  (sanity) -> `.test` (the kick) -> look at the monitor (orange) + the PASS line +
  `cat /proc/v3d` pixel probe. Regression: ping, ssh -tt, USB keyboard on HDMI,
  `fbshow --cube` (CPU demo) still runs, reboot.

## State to verify (point-in-time)

- `main` at **v0.4.248**, clean tree, on `origin/main`. Key commits: `7547335`
  V3D VIO_ADDR, `29ecea6` V3D Phase 2 foundation, `309464d` V3D Phase 2 in-kernel
  CL+FB, `ba4d7e4`/`c4a31da`/`fdac94b`/`2c9aef0` eMMC-aware FS (multi-block reads,
  read-ahead, discard, truncate leak fix -- all HW-verified, unrelated to V3D).
- Pi at **192.168.0.8**, build **2301 / v0.4.248**, healthy (netconsole 2323,
  sshd 2222 pw root).
- Key memories: `project_v3d_phase2`, `project_v3d_phase0`, `project_v3d_design`,
  `feedback_emmc_completion_timeout` (deadline-bounded waits),
  `feedback_hdmi_console_cacheable`, `project_netconsole` (driving gently).
- The full Phase 2 implementation plan (11 steps, AU-verdict, command recipes)
  lives in the v0.4.247/248 session; the kick is steps 7-10.
