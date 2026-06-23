# HANDOVER -- session 14 (2026-06-23/24)

Two threads this session: (A) confirm the stall mitigation + decide the strategic direction, then
(B) start enriching AIOS. All work committed on main (Bryan pushes). Board on build 2901 (prewarm
ON) at 192.168.0.8; the enrichment work below is QEMU-validated, NOT yet flashed.

## A. Stall + strategy (done, recorded)
- **PREWARM stall mitigation CONFIRMED (directional)** + default-ON. Matched reflash A/B/A: ON 0
  wedges/40 cycles, OFF 4 wedges (both types). NOT statistically decisive (OFF rate ~10% + bursty vs
  s13's 43%). Stall stays a MAJOR OPEN CONCERN ([[feedback_stall_open_concern]]). Detail:
  `docs/s14_results.md`, [[project_stall_session14]]. Commit a59a547.
- **DECISION: stay on seL4 for months** (Linux was plan B if the stall couldn't be calmed; prewarm
  calmed it). Driver-reuse review (`docs/DR_20260623_linux_driver_reuse_on_sel4.md`): practical Linux
  driver reuse on seL4 = x86 + driver-VM, no aarch64 shim shortcut. SMP eval
  (`docs/EVAL_20260623_arm_smp_situation.md`): default single-core-pinned is STABLE; BKL caps
  throughput (SMP buys resilience not throughput); confine gate already ran (split by wedge type).
  BL-2 (`docs/BL2_20260623_sel4_coupling_derisk.md`): AIOS userspace is thinly seL4-coupled (clean
  POSIX shim) -> a Linux port is bounded IF ever needed. BL-1/BL-2 backlogged in `BACKLOG.md`.
- **Fix #4** (5f0a765): warn when distribution places work on timer-masked (non-preemptible)
  secondaries (`spawn_util.c`).
- **Console rule recorded** ([[feedback_console_ssh_vs_netconsole]]): netconsole = deploy/recovery
  PLUMBING (bounded output, command-per-conn, NOT a terminal); SSH = the interactive/dev console.
  Don't misuse netconsole for streaming/long output; don't port OpenSSH (improve the existing sshd).

## B. Enrichment roadmap (started)
Roadmap survey: `wz7f0tzrk` synthesis -- keystone is on-device self-hosting (mmap + SHM-ring), the
toolchain is **musl + tcc** (NOT glibc/gcc). Tiers 1-2 + ext3 chosen (staying on seL4).

- **Tier-1 #1 DONE -- demand-paged anonymous mmap** (fc08a74): lifted the old 4MB eager cap; anon
  mmap is now demand-paged (reserve VA, zero-fill on fault) up to 256MB, reusing the file-backed
  fault path. New `PIPE_MMAP_ANON_LAZY` (93). QEMU-validated: `mmaptest` PASS (16MB, zero-fill,
  per-page R/W, munmap, reuse, concurrent); SMP gate 4/5. `mmaptest`/`posix_verify` added to /bin via
  build_apps.py.

- **DAILY-DRIVER CONSOLE (the PTY keystone) -- IN PROGRESS.** sshd today spawns the shell on cooked
  PIPES, no PTY -> isatty=false, raw-mode editors (zsh ZLE, vi, less) wedge (`ssh_channel.c:506`).
  tty_server already has the line discipline but was single-instance. Design:
  `docs/DESIGN_PTY_SSH.md`. Progress:
  - **Step 1 DONE** (c430357): tty_server multi-instance -- state in `tty_inst_t[]`; instance 0 =
    serial console, byte-identical (QEMU serial regression PASS each step: login, cooked echo, exec).
  - **Step 2a DONE** (baa4742): the tty_server PTY MECHANISM -- per-instance `master_out` ring +
    `is_pty` output routing (`inst_out`); master ops `TTY_PTY_ALLOC/INPUT/MASTER_READ/WINSZ/FREE`
    (labels in `include/aios/tty.h`, instance id in MR0). Additive, serial-safe.
  - **Step 2b TODO (the hard part)**: bind a shell's fd 0/1/2 to a PTY instance. Addressing decision
    is OPEN -- two options: (i) BADGED tty cap (badge = instance id) minted by the root/pipe_server in
    the spawn path (idiomatic, but cap-mint plumbing), or (ii) the libc uses instance-id-in-MR slave
    ops (add `TTY_PTY_SLAVE_READ/WRITE/POLL/IOCTL`, instance from a per-process value passed at spawn;
    NO cap-mint). Recommend (ii) for a first cut. Add `tty_inst` to `aios_fd_t`; route in
    `posix_file.c`/`posix_misc.c`/`posix_compat.c`; pass the instance id to the forked shell.
  - **Step 3 TODO (payoff)**: `ssh_channel.c` -- on `pty-req` call `TTY_PTY_ALLOC`; `spawn_shell`
    binds the child fd 0/1/2 to the PTY instance (not pipes); relay loop = SSH data ->
    `TTY_PTY_INPUT`, `TTY_PTY_MASTER_READ` -> SSH; `window-change` -> `TTY_PTY_WINSZ` -> SIGWINCH.
    Drop the hand-rolled Ctrl-C (ISIG handles it). Then point `SSH_SHELL_PATH` at zsh (Tier-2).

### Roadmap queue (not started)
Tier-1 #2 SHM-ring large-file I/O (unblocks tcc linker; pairs with mmap for self-hosting); Tier-2
seam-extraction refactor (msg_marshal.h/posix_ipc.c/split fork.c|cow.c -- shrinks the seL4 seam);
Tier-2 zsh (Phase 1 build flag, Phase 2 select/poll -- converges with PTY step 3); Tier-2 V3D
texturing + graphical console; Tier-2 keyboard LED software-only close-out; ext3 journaling
(crash-resilience insurance vs the stall freezes). See the roadmap synthesis + `BACKLOG.md`.

## Method / constraints (read before HW or testing)
- **Console**: drive interactive/long-output tests over SSH or a `-serial tcp:...,server` socket; keep
  netconsole tests to BOUNDED single-command output ([[feedback_console_ssh_vs_netconsole]]).
- **Serial console regression test** (the PTY safety net): boot QEMU with `-serial tcp:127.0.0.1:PORT,
  server,nowait`, connect, expect `AIOS login:`, send `root\n`/`root\n`, then `echo TTYWORKS\n` and
  verify echo + exec. PASS = instance 0 unchanged.
- **Build/gate**: `ninja -C build-04` (QEMU) + `python3 scripts/smp_qemu_test.py` (4/5 baseline). Apps
  to /bin: add to `scripts/build_apps.py` then `python3 scripts/build_apps.py --no-tcc --no-sbase
  --no-dash` (builds apps + rebuilds disk_ext2.img).
- **Board**: build 2901 (prewarm ON), 192.168.0.8. The enrichment work is userspace/tty + a posix
  change -- NOT flashed. Flash via `pi_flash.py --build` when a coherent set is ready (the stall is
  mitigated, not cured -- be gentle, watchdog+hwdog on).
- Commit on main; Bryan pushes. seL4 kernel edits -> regen `deps/patches/seL4-kernel.patch`.

---

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue enriching AIOS (we are STAYING ON seL4 for months -- the prewarm mitigation calmed the
~32.4s stall enough; Linux is plan B, backlogged). READ FIRST: docs/HANDOVER_20260624_session14.md,
docs/DESIGN_PTY_SSH.md, then memory [[feedback_console_ssh_vs_netconsole]] +
[[project_stall_session14]] + [[feedback_stall_open_concern]].

PRIMARY TASK -- finish the daily-driver console (PTY-backed SSH). Steps 1 + 2a are DONE + committed
(c430357 multi-instance tty_server; baa4742 the tty_server PTY mechanism -- master_out ring + is_pty
routing + TTY_PTY_ALLOC/INPUT/MASTER_READ/WINSZ/FREE). DO step 2b then step 3:
- 2b: bind a shell's fd 0/1/2 to a PTY instance. RECOMMENDED approach (no cap-mint): add
  TTY_PTY_SLAVE_READ/WRITE/POLL/IOCTL (instance id in MR0) that the libc uses when a fd has tty_inst>0;
  add `tty_inst` to aios_fd_t (default 0 = serial); route in posix_file.c/posix_misc.c/posix_compat.c;
  pass the PTY instance id to the forked shell (spawn arg/env). The serial console keeps the original
  TTY_* ops (instance 0) -- it must NEVER regress.
- 3: ssh_channel.c -- on pty-req TTY_PTY_ALLOC; spawn_shell binds child fd 0/1/2 to the PTY (not
  pipes); relay SSH<->PTY via TTY_PTY_INPUT / TTY_PTY_MASTER_READ; window-change -> TTY_PTY_WINSZ ->
  SIGWINCH; drop the hand-rolled Ctrl-C. Then point SSH_SHELL_PATH at zsh (Tier-2 dep).

GATE EVERY STEP: serial-console regression (the `-serial tcp` login+echo+exec test) MUST stay green,
+ smp 4/5. END-TO-END test: SSH in, confirm isatty=true, Ctrl-C, and vi/less render (over SSH, NOT
netconsole). Commit per step on main; Bryan pushes.

THEN (or instead, Bryan's call) the roadmap queue: Tier-1 #2 SHM-ring large-file I/O, Tier-2
seam-refactor / zsh / V3D texturing / keyboard-LED close-out, ext3 journaling. The toolchain target is
musl + tcc (NOT glibc/gcc). Invest in portable userspace + seam-shrinking refactors; the stall stays a
MAJOR OPEN CONCERN.
