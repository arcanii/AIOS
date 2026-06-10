# NEXT 2026-06-11 — HW REGRESSION: USB keyboard dies on first key press (v0.4.191)

Handover after a long session that completed the multi-review "sweep" (Tiers 0–2,
shipped as v0.4.187 → v0.4.191, all QEMU-verified) and then flashed v0.4.191 to
the real RPi4 — where a **hardware regression** surfaced. Read this with the
memories: `project_usb_hid`, `feedback_hdmi_console_cacheable`,
`project_v04187_tier0_review`, `project_identity_privesc`, `project_netconsole`.

---

## PRIORITY 1 — USB keyboard dies on the first key press (HW regression)

**Symptom (real RPi4, v0.4.191, standalone USB-kbd + HDMI):**
- Boots fine, HDMI login prompt shows, USB keyboard enumerates (Num-lock LED lit).
- **On the FIRST key press the keyboard dies and the LED goes out.** The HDMI
  console then wedges; shortly after the whole system hangs (no net either).
- Earlier in the same boot, `date` "froze everything then recovered" — same path
  (console output render), recovered that once, did not on the keystroke echo.

**This is the display_server → tty_server → USB-driver cascade** (the exact shape
of the backlogged HDMI scroll-freeze, OPEN ITEM 1 in
`docs/NEXT_20260610_usb_followups_status.md`): a keystroke is echoed to the HDMI
console; if rendering/scrolling that char wedges `display_server`, `tty_server`
blocks on its `DISP_CONSOLE` Call, the USB driver blocks on `SER_KEY_PUSH`, the
driver thread stalls → the lock LED it manages drops, and the keyboard is dead.

**KEY FACT — it is a REGRESSION in a TIGHT window (v0.4.188–191).** v0.4.187 was
HW-confirmed GOOD *this session*: the user typed `cat /usr/src/tcc/elf.h` etc. at
the HDMI console, **3534 clean scrolls, no freeze**, across a cold power-cycle
(see the v0.4.187 close-out in `docs/NEXT_20260610_usb_followups_status.md`).
So whatever broke the keyboard/console landed in one of:
- **v0.4.188** fsync + **periodic flusher** — adds a NEW always-on root thread
  (`src/servers/flush_server.c`, pinned core 0, prio 200) that `seL4_Call`s
  `FS_SYNC` every 30s → `blk_cache_flush()` → real eMMC writes. **Prime suspect**:
  a new core-0 thread doing eMMC IO could perturb the SMP/core-0 server scheduling
  or the block layer that the console-output path indirectly shares. NOTE the
  `date` freeze (date may append to /var/log) is consistent with an fs/flush stall.
- **v0.4.189** salted KDF — auth-only; unlikely to touch the keyboard path.
- **v0.4.190** privesc fix — **getty restructure** (now forks a child that calls
  `drop_identity` before exec) + `PIPE_SET_IDENTITY` root-gate + exec uid clamp.
  Second suspect: the getty/login + console/tty fd setup changed. The
  `PIPE_SET_IDENTITY` gate (`active_procs[ci].uid==0`) — verify nothing in the
  tty/display/USB boot path calls it from a non-root context and now gets denied.
- **v0.4.191** thread_server join rework — changed `aios_thread_t` (hence
  `active_proc_t`) layout. Unlikely but: confirm nothing depends on the struct
  offsets. Lowest suspicion.

**RECOMMENDED FIRST STEPS (next session):**
1. **Confirm + localize cheaply.** The flusher (v0.4.188) is the cheapest to test:
   comment out `flush_server_init()` in `src/boot/boot_services.c`, rebuild
   `kernel8.img` (`ninja -C build-rpi4` → `python3 scripts/mkkernel8.py`),
   flash-free swap onto the FAT (`cp disk/kernel8.img /Volumes/AIOSBOOT/...`),
   reboot, test the keyboard. If fixed → it is the flusher (then make it gentler:
   longer period, or skip flushing when nothing is dirty / when the console is
   active, or run it off core 0). NOTE: getty is a DISK app, so a kernel8 swap
   does NOT revert v0.4.190's getty change — to test getty you need a disk rebuild.
2. **If not the flusher,** suspect the getty restructure (v0.4.190). Reason about
   the login/console fd + identity path; the QEMU console works, so it is
   HW-specific timing/cascade. The cacheable-FB scroll path (`fb_console.c`
   scroll_up/flush, `display_vc.c` cacheable map) is the thing that actually
   wedges — instrument `/proc/fbcon` (already shows scroll/flush phase) and read
   it over SSH right after a freeze.
3. **Fallback to a working Pi anytime:** reflash a **known-good** image. v0.4.187
   was HW-good — rebuild it: `git stash` (none), `git checkout <v0.4.187 commit>`
   (the commit before v0.4.188 `48f9457`), `python3 scripts/build_apps.py`,
   `ninja -C build-rpi4`, `python3 scripts/mksdcard.py --mem 4096 --output
   disk/sdcard-rpi4.img`, flash with balenaEtcher, then `git checkout main`.

---

## PRIORITY 2 — netconsole back-to-back connections wedge the WHOLE system

Separate, real, reproducible: hammering netconsole with rapid connections (each
connection = one `dash -c` per the v2 design) did not just wedge netconsole — it
**hung the entire box** (net + HDMI + keyboard all dead, 0/10 ping). The
`project_netconsole` memory already says "drive netconsole GENTLY (one held conn,
~4s settle)" — but the failure mode is a FULL system hang, not a netconsole-only
stall, so it is worse than documented. Likely the per-connection fork storm
hitting an allocator/SMP race (root threads are pinned to core 0 precisely for
"SMP allocator-race hardening" — see boot_services `start_server_thread`).

**Rule for HW diagnosis going forward:** do NOT rapid-fire netconsole. Use **SSH**
(a held session, robust — port 2222) or a **USB-serial console** (GPIO14/15,
115200 8N1). If you must use netconsole, ONE connection, ONE compound command,
≥4s between connections. (This session wedged the Pi by ignoring this.)

A proper fix for netconsole (bounded accept rate / serialize per-connection forks
/ a read deadline) is worth a task — it is currently a self-inflicted DoS.

---

## What shipped this session (the "sweep") — ALL committed locally, UNPUSHED

All QEMU-verified; **none were HW-stable** beyond v0.4.187 (see Priority 1).
| ver | commit | what | test |
|---|---|---|---|
| v0.4.187 | 4bc3204 | Tier 0: THIRD_PARTY_LICENSES, CONTRIBUTING, CHANGELOG, .clang-format, compile_commands, SSH backoff + pre-auth hardening | ssh_qemu_test, ssh_backoff_qemu_test |
| v0.4.188 | 48f9457 | fsync/fdatasync/sync + periodic flusher (FS_SYNC) | sync_qemu_test 5/5 |
| v0.4.189 | 4ea45b9 | salted password KDF (`$a1$`, 12000× SHA3) | auth_kdf_qemu_test |
| v0.4.190 | 34490d6 | local privesc fix (PIPE_SET_IDENTITY gate + getty/sshd drop-in-child + exec clamp) | identity_qemu_test 6/6, ssh_nonroot 2/2 |
| v0.4.191 | b9ae56a | thread_server non-blocking join (deferred reply, fault event loop) + 2 review-found bug fixes | thread_qemu_test 5/5 |

Plus docs: `57c700f`, `14c6723` (scroll-freeze close-out). **Nothing is pushed.**

## Remaining sweep work (after the HW regression is resolved)
- **1d — dependency pinning** (the wrap-up right before sharing the repo): pin
  seL4/musl/sbase/dash/zsh/tcc/mbedtls commits, idempotent `.patch` files, DEPS.md.
- Backlogged: **1c CI** (self-hosted, not GitHub-hosted), **HDMI scroll-perf**
  (HW panning — and note it may be entangled with Priority 1 above).
- Minor follow-ups: `open(O_CREAT)` returns a usable fd even when the FS create is
  denied; `auth/net/disp/crypto` endpoints copied UNBADGED to children; **libaios
  stdio is NOT thread-safe** (concurrent printf from a worker + main corrupts
  stdout — surfaced by test_threads); `pthread_detach` is a stub.

## Build / deploy quick ref
- Everyday: `python3 scripts/build_apps.py` (ninja build-04 + sbase + dash + tcc +
  disk_ext2.img). RPi4 kernel: `ninja -C build-rpi4`.
- Flash-free kernel/root-task update: `python3 scripts/mkkernel8.py` →
  `cp disk/kernel8.img /Volumes/AIOSBOOT/kernel8.img` → eject → reboot.
- Full SD image: `python3 scripts/mksdcard.py --mem 4096 --output
  disk/sdcard-rpi4.img` → balenaEtcher.
- ALWAYS build BOTH build-04 + build-rpi4 after shared-code changes.

## Test scripts added this session
`scripts/{sync,auth_kdf,identity,ssh_nonroot,ssh_backoff,thread}_qemu_test.py`,
`scripts/gen_etc_passwd.py`, probe apps `src/apps/{idtest,test_join,test_threads}.c`.
