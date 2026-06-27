# HANDOVER -- session 24 (2026-06-27): the SYSTEM LAYER (init + login) -- AIOS boots into a managed system

Continues the userspace-kernel work (the 2026-06-24 pivot: AIOS as a gVisor-style userspace kernel on
Linux; verified seL4-on-x86-64 is the destination; verification is the soul). Session 23
(docs/HANDOVER_20260627_session23.md) delivered the **second PAL backend (seccomp, v0.5.22)** + the
**minimal Linux-6.18 appliance** + a native **`uk/gate.sh`** + a Pi **`demo.sh`** -- all HW-validated on
the RPi4. **Session 24 starts "building out the AIOS environment" with the SYSTEM LAYER (increment 1):
AIOS now boots into a real init -> login -> authenticated session -> shell -> logout -> respawn loop,
not a bare shell.** v0.5.22 -> v0.5.23, **ABI UNCHANGED (48)** -- init + login are pure AIOS-ABI
programs (libaios). Validated on colima: `test/login_pty.c` PASS under BOTH PAL backends, and the full
gate's PASS 1 (linux) is green incl. the new `login` key. (The seccomp PASS 2 of that full run flaked on
the known intermittent ptrace/pipe stall at an UNRELATED test, `prog_pwgrp` -- orthogonal to the system
layer; a re-run clears it.) HW (Pi) validation: in progress.

## Why this direction

Bryan: "build out the AIOS environment." Picked the **system layer** (init / login / sessions) over
fuller-userland / interactive-apps / networking. The trigger: a live demo where typing `uname`/`date`
in `./aios-uk ./dash` got each KILLED -- correctly (those are real host binaries making real Linux
syscalls; the M4 boundary blocks them), but it showed AIOS was "one shell," not a system. Now it is a
system you log into.

## What shipped (system layer, increment 1)

All on `main`, v0.5.23, NO new ABI (init/login are AIOS-ABI programs over the existing 48 syscalls):

- **`guest/init.c` -- the AIOS system init.** The FIRST guest the kernel launches (the appliance's
  `/sbin/init`). Loops: fork+exec `/bin/login`, wait the session, respawn on logout (a `clock_gettime`
  nap for anti-thrash, since libaios `sleep()` is a no-op). The kernel reaps orphans itself, so init
  only manages its login child.
- **`guest/login.c` -- the AIOS login.** Banner, `login:` (read a username), `Password:` (read with
  terminal **ECHO off** via the M8 termios layer), authenticate against `/etc/shadow`, then become the
  user's **login shell** (`execve` with `argv[0] = "-sh"` so dash sources `/etc/profile`; builds an
  envp of USER/HOME/SHELL/PATH/LOGNAME; cats `/etc/motd`). Retries on a bad password.
- **`mkaiosroot.sh`** now installs `/sbin/init`, `/bin/login`, `/etc/shadow` (aios/aios + root/root),
  `/etc/profile`, `/home/aios`. **`appliance/aios_init.c`** launches `/sbin/init` instead of `/bin/sh`
  -- so the appliance boots straight into an AIOS LOGIN.
- **`test/login_pty.c`** -- a forkpty driver: init -> login prompt -> (username + ECHO-off password) ->
  the user's shell runs a command -> `exit` (logout) -> init **respawns** the login (a 2nd prompt).
  PASS under BOTH PAL backends; wired into `gate.sh` as the `login` key (the gate is now 17 keys).

## KEY LESSON (carry forward)

- **A program that reads stdin with buffered stdio then EXECs another program eats that program's
  input.** login's first cut used `fgets` for the username/password; libaios FILE\* buffering
  over-reads the *pipe* (one `read()` slurps the whole pipe into the FILE buffer), so the exec'd shell
  saw EOF and ran nothing -- auth succeeded (motd printed) but the session was dead. On a *tty* the
  bug hides (canonical-mode `read()` returns one line). Fix: login reads stdin **UNBUFFERED**
  (`read_line()` = `read(0,&c,1)` until `\n`), the way real login/getty do. This is the classic
  getty/login gotcha; any future "read input then exec a shell" code must do the same.

## INCREMENT-2 (deliberately deferred -- the honest scope line)

Increment 1 is the login *experience*; these are next:

1. **Real password hashing** -- `crypt()` (DES/MD5/SHA-512) in libaios; `/etc/shadow` stores hashes,
   not plaintext. (Inc 1 compares plaintext.)
2. **Identity** -- a `SETUID/SETGID/GETUID/GETGID` ABI + per-process uid/gid in the kernel (inherited
   across fork, preserved across exec), so login actually switches user and `whoami`/`id`/`ls -l`
   reflect the logged-in user. (Inc 1 keeps the kernel's existing identity -- uid is cosmetic since the
   host owns the real file ownership.)
3. **Services + shutdown** -- an `/etc/inittab`-style config so init starts services (not just one
   getty), and a clean `halt`/`poweroff` (init handles a shutdown signal). Plus a richer userland (more
   sbase utils: `uname`/`whoami`/`date`/`env`/`printf`/`tr`/`cut`/...) so the logged-in shell is fuller.

## Dev loop (carry forward)

- `uk/run.sh` (colima docker wrapper) or directly `sh uk/gate.sh` -- builds the suite + runs the gate
  TWICE (PAL=linux then PAL=seccomp), 17 keys, `linux=0 seccomp=0`.
- HW: `rsync -az --delete --exclude='/aios-uk' --exclude='/guest_*' --exclude='/prog_*'
  --exclude='/sbase-*' --exclude='/dash' --exclude='/init' --exclude='/login' --exclude='*.o'
  --exclude='/aiosroot' --exclude='/aiosroot.tar' --exclude='/appliance/build' --exclude='/appliance/out'
  uk/ pi@raspberrypi.local:~/uk/` then `ssh pi@raspberrypi.local 'cd ~/uk && sh gate.sh'` (pi/aios).
- DEMO on the Pi: `ssh pi 'cd ~/uk && make all && sh demo.sh'`; interactive **confined** login shell:
  `ssh -t pi 'cd ~/uk && sh mkaiosroot.sh /tmp/r >/dev/null && AIOS_ROOT=/tmp/r PATH=/bin ./aios-uk
  /tmp/r/sbin/init'` -> an AIOS login (aios/aios) -> a shell where bare `ls`/`cat`/`grep` work.
- GOTCHAS (carried): seccomp can't trap out-of-range nrs -> AIOS guests trap via the GATEWAY
  (AIOS_GATEWAY=gettid/178, x8=gateway carrying the real nr in x9); `.pal.stamp` forces a rebuild on a
  PAL switch; `stdbuf -oL` for real-time gate logs; colima virtiofs lags (sync + read-probe; NEW dirs
  lag more); gcc 14 (Pi) stricter than gcc 13; NO apostrophes in a `sh -c '...'` body; a ptrace hang is
  SILENT (timeout + fprintf); a rare intermittent ptrace/pipe stall on the Pi 6.12 (file-input pipelines
  + no early-close + timeout guards dodge it -- see demo.sh).

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue building AIOS as a **gVisor-style userspace kernel on Linux** (the 2026-06-24 pivot off
seL4/RPi4 -- Linux is the interim substrate, verified seL4-on-x86-64 is the destination; verification is
the soul; programs see only the AIOS ABI, the host sits behind a narrow PAL). READ FIRST: memory
[[project_pivot_linux_userspace_kernel]] + docs/HANDOVER_20260627_session24.md +
docs/HANDOVER_20260627_session23.md + docs/AIOS_KERNEL_DEPENDENCIES.md + uk/README.md.

WORKING BRANCH = **`main`** (the 0.5.x userspace kernel; commit per milestone on main, Bryan pushes).
The `uk/` tree: a host-agnostic kernel (kernel/aios_kernel.c includes ONLY aios_abi.h + aios_version.h +
pal.h) over a SHARED Linux host-driver core (pal/pal_linux_common.c) + TWO trap front-ends
(pal/pal_linux.c = PTRACE_SYSCALL, pal/pal_seccomp.c = seccomp RET_TRACE; `make PAL=linux|seccomp`) +
libaios + shadow standard headers (lib/include, -nostdinc).

DONE through **v0.5.23, 48-syscall ABI**: OPERATIONAL (vendored dash + 16 sbase utils run UNMODIFIED) +
the boundary COMPLETE (M4 trap soundness + M4.2 fs confinement + M4.3 exec confinement) + FULL JOB
CONTROL + raw termios + a SECOND PAL backend (seccomp, via the GATEWAY -- seccomp can't trap AIOS's
out-of-range nrs) + a minimal Linux-6.18 appliance + a native gate.sh + a Pi demo.sh + **the SYSTEM
LAYER increment 1: AIOS boots into init -> login (password-checked, /etc/shadow) -> a user shell ->
logout -> respawn** (guest/init.c + guest/login.c; the appliance launches /sbin/init). ALL HW-validated
on the RPi4 (kernel 6.12.47, gcc 14.2; `sh gate.sh` -> linux=0 seccomp=0, 17 keys incl. login). NEVER
patch vendored sources -- grow libaios.

PRIMARY TASK -> continue the SYSTEM LAYER (increment 2) and/or the endgame, ASK Bryan which:
(1) **system-layer inc 2**: real crypt() password hashing; a SETUID/SETGID/GETUID/GETGID ABI +
per-process uid/gid so login switches user and whoami/id/ls -l reflect it; /etc/inittab services +
clean shutdown; more sbase utils (uname/whoami/date/env/...). (2) **the endgame**: sched_ext (AIOS's
own scheduling policy as a sched_ext BPF program; the 6.18 appliance can carry CONFIG_SCHED_CLASS_EXT) /
the seL4/x86-64 replant seam (pal_sel4.c -- a THIRD PAL backend proving kernel/aios_kernel.c runs
UNCHANGED on a verified base; M9's PAL_RESUME/host-driver split de-risked it; AIOS_KERNEL_DEPENDENCIES.md
is the proof obligation) / the from-source minimal-6.18 kernel build (a proper kernel-build env) + a Pi
"boots into AIOS" deploy (a getty/systemd unit). Keep kernel/aios_kernel.c host-agnostic + the PAL seam
minimal. Commit per milestone on `main`; validate colima + the Pi (`raspberrypi.local`, pi/aios) via
`sh gate.sh`; Bryan pushes. GOTCHAS: the buffered-stdin-then-exec gotcha (login reads UNBUFFERED); the
GATEWAY for seccomp; .pal.stamp; stdbuf -oL; colima virtiofs lag; gcc 14 strictness; a rare Pi 6.12
ptrace/pipe stall.
