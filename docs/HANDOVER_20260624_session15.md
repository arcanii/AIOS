# HANDOVER -- session 15 (2026-06-24)

PRIMARY TASK COMPLETE + HW-VALIDATED: the daily-driver console (PTY-backed SSH) is done
end-to-end and confirmed on the real board (.8). All work committed on main (Bryan pushes).
Staying on seL4 (stall mitigated, not cured -- [[feedback_stall_open_concern]]).

## HW VALIDATION (full SD reflash via balenaEtcher) -- 6/6
Flashed `disk/sdcard-rpi4.img` (mksdcard.py: build-rpi4 kernel + the coherent new-libaios
disk_ext2.img). `scripts/ssh_pty_hw_check.py` 6/6 on REAL output: isatty(0) true, interactive
command output, whoami=root + uname=aarch64 (EXTERNAL cmds inherit the PTY via the y-token on
silicon), Ctrl-C survives + discards the partial line. Passed despite a ~32.4s stall +
watchdog-recovery during the run (the relay grace window makes a mid-session stall survivable).

### HW-only teardown race -- FOUND + FIXED (commit 9e15440)
QEMU was a clean 7/7 but HW dropped all command output. A scripted client (`ssh -tt host <<EOF`)
closes stdin right after the commands -> ssh_read_packet_nb returns -1 -> the relay did done=1,
tearing down BEFORE the slow-HW shell (~800ms forks) produced output (QEMU's fast path always won
the race). FIX: the relay ends when the SHELL exits (kill(child,0)==ESRCH), NOT when the client
closes its INPUT side; on socket-EOF/CHANNEL_EOF it keeps draining until the shell exits, bounded
by RELAY_EOF_GRACE_TICKS; the final drain drains-until-quiescent (not break-on-first-empty). Also
fixed false-positive tests (raw -tt echoes the typed command, so a literal marker matched the ECHO
not the OUTPUT -> use printf '%s' concatenation; whoami->root / uname->aarch64 are output-only).
DIAGNOSIS METHOD (reusable): a redirect side-effect read back over netconsole proved the
input+exec path worked and isolated the loss to the output-on-teardown; holding stdin open
(trailing sleep) made output appear -> confirmed the trigger. Deploy lesson: a coherent PTY change
spans kernel + ALL userspace (every binary links libaios) -> full SD reflash is the clean path;
pi_flash --build is kernel-only; a killed netconsole deploy WEDGES netconsole (power-cycle needed).
vi/less are NOT in the board's sbase build (add them to demo a full-screen app).

## What shipped (steps 2b + 3 of docs/DESIGN_PTY_SSH.md)
sshd is a REAL TERMINAL: `isatty(0)` is TRUE over SSH, line editing / Ctrl-C / external-cmd
inheritance all work on HW. Detail + design in memory [[project_pty_console]].

## What shipped (steps 2b + 3 of docs/DESIGN_PTY_SSH.md)
sshd is now a REAL TERMINAL: `isatty(0)` is TRUE over SSH, line editing / Ctrl-C / vi / less
all work. Detail + design in memory [[project_pty_console]].

- **Step 2b** (51f7a2b) -- bind a shell's fd 0/1/2 to a PTY instance, NO cap-mint:
  - tty_server SLAVE ops (instance id in MR0): `TTY_PTY_SLAVE_READ/WRITE/POLL/IOCTL` (85-88)
    + `TTY_IOCTL_GET_WINSZ` (10). Serial console keeps the original TTY_* ops byte-identical.
  - `aios_fd_t.tty_inst` + global `aios_tty_inst` + `aios_set_tty_inst()`. read/readv(fd0),
    write (aios_stdio_write chokepoint), ioctl (TCGETS/TCSETS/TIOCGWINSZ), poll/select route
    by the instance. ONLCR moved into tty_server PTY output (raw-mode vi/less not mangled).
  - The PTY survives exec via a `y<inst>:` cwd token (mirrors `t<slot>:`), packed in PIPE_EXEC
    MR0 bits [47:32] -> shell + all its children inherit the PTY.
  - VINTR on a cooked+ISIG PTY raises SIGINT (`kill(0,SIGINT)` -> pipe_server fg_pid).
    tty_server got pipe_ep as a 3rd boot cap (argv[1]=pipe_ep, argv[2]=disp).
- **Step 3** (70584d0) -- sshd uses the PTY. `channel_relay_pty` = pure byte shuttle (echo /
  Ctrl-C / LF->CRLF all in tty_server now). window-change -> TTY_PTY_WINSZ + SIGWINCH; shell
  exit via `kill(child,0)==ESRCH`; PTY leak fixed (`ssh_channel_pty_release`). Legacy
  cooked-pipe path KEPT as fallback for pty_inst==0 (`ssh -T` / SFTP). SSH_SHELL_PATH=/bin/dash.

## Gates (run every PTY-touching change; serial MUST stay green)
- `python3 scripts/serial_console_qemu_test.py` -- serial login + cooked echo + exec (the
  `echo TTYWO''RKS` quote trick proves ECHO vs EXEC). Instance 0 safety net. **5/5.**
- `python3 scripts/ssh_pty_qemu_test.py` -- `ssh -tt`: isatty TRUE, echo, whoami, uname,
  Ctrl-C discards partial line + survives. **7/7.**
- `python3 scripts/smp_qemu_test.py` -- **4/5 baseline** (5th = netconsole-storm flakiness).
- Build: `python3 scripts/build_apps.py [--no-tcc]` (ninja -> sbase -> dash -> apps -> sshd ->
  libaios -> disk). ~22s with --no-tcc. sshd + posix changes both rebuilt by it.

## Known non-bugs (do NOT chase)
- `tty` command / `ttyname()` say "not a tty" -- AIOS has no /dev/pts node. `isatty()` IS true
  (that's what matters). A "/dev/pts/<inst>" ttyname shim is a small optional follow-up.
- `ssh -T` (scripted short non-PTY session) flaky/times-out in QEMU SLIRP -- VERIFIED
  pre-existing (the pre-PTY sshd times out identically). The streaming PTY session is robust.

## PART C -- post-console tracks (zsh / PTY hardening / SHM-ring): assessed, see below

### zsh over the PTY -- BACKLOGGED on Phase-3 job control ([[project_zsh_pty]], commit d340200)
zsh BUILDS (build_zsh.py -> /bin/zsh, 1.5MB, zsh 5.9) and interactive **ZLE works over the PTY**
(a backspace line-edit produced the corrected command). `zsh -c` externals work. BUT **zsh -i
does NOT run external commands** (`whoami` produces no output AND no file side-effect -- the
command never executes; builtins like `print` do run). That is DESIGN_ZSH **Phase 3 (job
control / process groups)**: zsh -i's interactive child-process setup needs real pgids +
tcsetpgrp, which AIOS stubs. `unsetopt monitor` lead TRIED + FAILED (blocker is deeper than the
MONITOR option). So **dash stays the SSH shell**; zsh is on the disk for `exec zsh` ZLE
experimentation. Also: tests now send CR (\r), not LF -- real-terminal Enter is CR and zsh ZLE
binds accept-line to CR (d340200). zsh is NOT in build_apps.py (needs the external zsh source).

### PTY hardening -- DEFERRED (low value now)
- **SIGWINCH live-resize**: plumbing is in + correct (window-change -> TTY_PTY_WINSZ; TIOCGWINSZ
  reads it; kill(0,SIGWINCH) to fg). But NO CONSUMER -- no full-screen apps (vi/less not in
  sbase) and no `stty`. Revisit when a full-screen app lands.
- **Concurrent multi-session**: sshd is a serial accept loop (`listen(lfd,1)`). True concurrency
  needs fork-per-connection + (security!) **DRBG reseed-after-fork** (g_drbg is seeded ONCE at
  startup -> forked children would share RNG state -> identical ECDH keys) + WNOHANG reaping
  (wait4 ignores options today) + concurrency testing + more process load vs the stall. Marginal
  value for a single user; deferred.

### SHM-ring large-file I/O -- DESIGNED, the NEXT focus (Tier-1 #2)
Bottleneck: file writes pack data into MRs, capped ~800 bytes/FS_PWRITE (fetch_pwrite,
aios_posix.c) -> a tcc-output binary is thousands of round-trips. Reuse the pipe SHM-ring model
([[project_shm_ring]]) for FS I/O: PIPE_MSYNC in pipe_server ALREADY maps client pages + calls
vfs_pwrite for mmap write-back, so add a sibling **PIPE_PWRITE_BULK** (client buffer vaddr +
offset + len + path -> map + vfs_pwrite, 4KB+/call), client large-write fast path in
posix_file.c, **default-OFF** like the pipe ring. CAUTION: this is a cache-coherency change --
[[feedback_pipe_shm_cache]] QEMU CANNOT catch cacheable-mismatch bugs; needs an HW soak (the
pipe ring took dedicated sessions). Plan: code + QEMU correctness (byte-exact large file) ->
default-OFF commit -> flash + HW soak (cross-core coherency) -> tcc-linker speedup validation.

---

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue enriching AIOS (STAYING ON seL4 -- prewarm calmed the ~32.4s stall; it STILL fires
~1/boot, watchdog-recovered, MAJOR OPEN CONCERN). READ FIRST: docs/HANDOVER_20260624_session15.md,
then memory [[project_pty_console]] + [[project_shm_ring]] + [[feedback_pipe_shm_cache]] +
[[feedback_stall_open_concern]].

DONE + HW-VALIDATED this session: the PTY-backed SSH daily-driver console (isatty true over SSH,
line editing, Ctrl-C, external cmds inherit the PTY). 6/6 on the board via a full SD reflash
(disk/sdcard-rpi4.img, balenaEtcher). A HW-only teardown race was found + fixed (9e15440). zsh
BACKLOGGED (zsh -i can't run externals -- Phase-3 job control; [[project_zsh_pty]]). PTY
hardening DEFERRED (no consumer / marginal). Commits 51f7a2b 70584d0 9e15440 d340200 + handovers.

PRIMARY TASK -- Tier-1 #2 **SHM-ring large-file I/O** (unblocks tcc linker / on-device
self-hosting; pairs with s14 demand-paged mmap). Design in this handover (PART C) +
[[project_shm_ring]]. Add PIPE_PWRITE_BULK (reuse PIPE_MSYNC's client-page-map + vfs_pwrite) +
the client large-write fast path in posix_file.c, **default-OFF**. GATE: serial regression 5/5,
ssh_pty 7/7, smp 4/5, byte-exact large-file write in QEMU. Then flash + **HW soak the cache
coherency** ([[feedback_pipe_shm_cache]] -- QEMU can't catch it). Then validate the tcc-linker
speedup. Commit per step on main; Bryan pushes.

ALTERNATIVES if Bryan prefers: zsh Phase-3 job control (real pgids in pipe_server + tcsetpgrp +
SIGTSTP/SIGCONT + WNOHANG -- unblocks zsh as the daily-driver shell); seam-extraction refactor;
V3D texturing; ext3 journaling. Toolchain target = musl + tcc.
