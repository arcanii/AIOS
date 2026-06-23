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

## Not yet done / next
- HW reflash the PTY set (`pi_flash.py --build`) -- gentle, stall mitigated not cured. Verify
  isatty/Ctrl-C/vi over real SSH on the board.
- zsh over the PTY: point SSH_SHELL_PATH at interactive zsh once zsh ZLE + select/poll work.
- SIGWINCH live-resize + concurrent multi-session not yet exercised.

## Roadmap queue (Bryan's call -- not started)
Tier-1 #2 SHM-ring large-file I/O (unblocks tcc linker; pairs with the s14 demand-paged mmap
for self-hosting); Tier-2 seam-extraction refactor (msg_marshal.h / posix_ipc.c / split
fork.c|cow.c); Tier-2 zsh (Phase 1 build flag, Phase 2 select/poll -- converges with the PTY);
V3D texturing + graphical console; keyboard-LED close-out; ext3 journaling. Toolchain = musl +
tcc (NOT glibc/gcc). Stall remains a MAJOR OPEN CONCERN.

---

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue enriching AIOS (STAYING ON seL4 for months -- prewarm calmed the ~32.4s stall; Linux
is plan B, backlogged). READ FIRST: docs/HANDOVER_20260624_session15.md, then memory
[[project_pty_console]] + [[feedback_console_ssh_vs_netconsole]] + [[feedback_stall_open_concern]].

The PTY-backed SSH console is DONE + committed (51f7a2b step 2b, 70584d0 step 3): isatty true
over SSH, line editing, Ctrl-C, vi/less work; serial console (tty_server instance 0)
byte-identical. QEMU-validated (ssh_pty 7/7, serial regression 5/5, smp 4/5), NOT yet flashed.

PICK ONE (Bryan's call):
- HW-validate the PTY console: reflash (pi_flash.py --build, gentle), then drive a real SSH
  session over LAN (.8) -- confirm isatty, Ctrl-C, vi/less render on the board.
- zsh over the PTY: get interactive zsh (ZLE + select/poll) working, then point
  SSH_SHELL_PATH at it -- the cherry on the daily-driver console.
- Roadmap queue: Tier-1 #2 SHM-ring large-file I/O; Tier-2 seam-refactor; V3D texturing;
  kbd-LED close-out; ext3 journaling. Toolchain target = musl+tcc.

GATE every PTY/tty/posix change with the serial-console regression (scripts/
serial_console_qemu_test.py, 5/5) + ssh_pty_qemu_test.py (7/7) + smp 4/5. Commit per step on
main; Bryan pushes. The stall stays a MAJOR OPEN CONCERN.
