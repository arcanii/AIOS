# HANDOVER -- session 25 (2026-06-27): SYSTEM LAYER increment 2 -- AIOS is a system you log into AS A USER

Continues the userspace-kernel work (the 2026-06-24 pivot: AIOS as a gVisor-style userspace kernel on
Linux; verified seL4-on-x86-64 is the destination; verification is the soul). Session 24
(docs/HANDOVER_20260627_session24.md) delivered the **system layer increment 1**: AIOS boots into
init -> login -> a password-checked session -> shell -> logout -> respawn. **Session 25 delivers the
core of increment 2: a real IDENTITY model -- login now switches to the authenticated user, passwords
are crypt()-hashed, and the logged-in shell is fuller.** v0.5.23 -> **v0.5.28**, ABI 48 -> **54**.
ALL HW-validated on the real RPi4 (gcc 14.2, kernel 6.12.47): `sh gate.sh` -> **linux=0 seccomp=0**.

## What shipped (6 commits on `main`; Bryan pushes)

1. **`6fc786a` -- inc 2 part 1: PROCESS IDENTITY (v0.5.24, ABI 48 -> 54).** Six syscalls
   GETUID/GETEUID/GETGID/GETEGID/SETUID/SETGID (0x102F..0x1034). `proc_t` gains real/effective/saved
   uid+gid, inherited across fork, preserved across exec; the launched (init) guest is seeded AIOS root
   (uid 0). Identity is the kernel's OWN model -- decoupled from the host user aios-uk runs as, exactly
   like fs confinement is kernel policy. setuid/setgid follow POSIX privilege (euid 0 sets all three,
   else only real/saved -> EPERM), so login can drop from uid 0 to the user IRREVERSIBLY. libaios: real
   getuid/geteuid/getgid/getegid + new setuid/setgid (were fixed-0 stubs). Proof: guest/prog_id.c +
   sbase `whoami` runs UNMODIFIED.

2. **`9af9cae` -- inc 2 part 2: login SWITCHES USER (v0.5.25, no new ABI).** On a successful auth,
   login (init's child, uid 0) setgid's then setuid's to the authenticated user before becoming their
   shell, so the WHOLE session runs AS that user (whoami/id/$LOGNAME reflect them). libaios grew
   getlogin(); sbase `logname` joins `whoami`. Proof: test/login_pty.c now asserts the session sees
   `whoami` == aios (not root).

3. **`b61e03c` -- inc 2 part 3: real crypt() SHA-512 hashing (v0.5.26, no new ABI).** /etc/shadow stores
   SHA-512 ("$6$") crypt HASHES; login recomputes crypt(typed_pw, stored_hash) and compares. libaios
   gained a from-scratch SHA-512 (FIPS 180-4) + the SHA-512-crypt scheme (Drepper's spec) -- hashes
   BYTE-IDENTICAL to host glibc / `openssl passwd -6` (verified). Pure libaios, no host call. A non-'$'
   secret is still legacy plaintext (transitional). Proof: guest/prog_crypt.c (reproduces the host
   reference vectors exactly; wrong pw fails; verify round-trip holds).

4. **`4a08c7f` -- gate robustness.** Timeout-guard the four inline dash-PIPE demo lines (head|tail,
   grep|wc, ls|head, echo|wc): they are illustrative output, not pass/fail keys, and hit the documented
   intermittent ptrace/pipe stall under the seccomp backend. A `timeout 30` lets the gate finish past a
   stall (it bit ~3x this session before this fix). No change to what is gated.

5. **`11815da` -- inc 2 part 4: more utils uname/env/printenv/pwd/tty/date (v0.5.27, no new ABI).**
   Headline: **uname reports AIOS's OWN identity** (`uname -srm` -> "AIOS 0.5.28 aarch64", NOT "Linux") --
   a guest sees the AIOS kernel, never the substrate. libaios grew: uname() over a new shadow
   <sys/utsname.h> (AIOS constants + /etc/hostname); putenv/setenv/unsetenv (env); mktime (UTC inverse
   of gmtime) + strftime extended (%F %T %R %D %Z(UTC) %A %B %I %p %u %w %C %n %t) so date prints +
   computes time matching the host EXACTLY; a read-only clock_settime (EPERM -- the AIOS clock has no
   set syscall); ttyname (/dev/console on a tty). date is UTC-only (AIOS has no timezone -- honest).

6. **`41bb2b2` -- inc 2 part 5: sbase tr + cut (v0.5.28, no new ABI, no new libaios code).** Pure
   Makefile wiring against the libutf rune chain sort/grep already use: tr (translate/-d/-s incl. POSIX
   [:class:] sets -> the full is*rune classifiers + to{lower,upper}rune + ef{get,put}rune/utflen); cut
   (-b/-c/-f -d -s + memmem).

**Vendored, running UNMODIFIED now:** dash + sbase true/false/echo/cat/wc/mkdir/rm/ls/head/tail/cp/mv/
ln/chmod/sort/grep + **whoami/logname/uname/env/printenv/pwd/tty/date/tr/cut**.

## KEY LESSONS (carry forward)

- **AIOS identity is the kernel's OWN model, decoupled from the host.** uid/gid are AIOS-internal
  (seeded root for init; setuid drops privilege) -- the host still owns real file OWNERSHIP, so this is
  identity, NOT yet uid-based file-access control (the honest scope line). uname() likewise reports
  AIOS, not the host's "Linux" -- the same principle as fs confinement: AIOS presents its own world.
- **crypt() byte-identity is the proof.** A from-scratch SHA-512 + SHA-512-crypt reproduces
  `openssl passwd -6` exactly -- so /etc/shadow is portable and the algorithm is independently
  verifiable (the soul). The base-64 permutation + 5000-round stretch follow glibc's sha512-crypt.c.
- **A version bump must rebuild what embeds the version.** libaios.c now includes aios_version.h (for
  uname's release string), so include/aios_version.h was added to SBASE_DEPS -- else uname showed a
  stale release on an incremental build (a clean gate build was always fine).
- **The intermittent seccomp dash-pipe stall is real on colima** (kernel 6.8) and bit PASS 2 ~3x this
  session. The fix is the timeout-guard (commit 4 above); the Pi (6.12) completes the full gate.
- **DEV-LOOP gotcha (cost real time): `$PWD` drift.** `git` calls left the shell at the repo ROOT, so
  `-v "$PWD":/uk` mounted the wrong dir ("No rule to make target aios-uk"). ALWAYS use the absolute
  `.../AIOS/uk` path for docker `-v`, never `$PWD`.
- **DEV-LOOP gotcha: colima only bind-mounts under $HOME.** A docker `-v /private/tmp/...:/out` for a
  gate log went VM-local (the host never saw it). Write the gate log INTO the mounted `uk/` tree
  (uk/scratch_gate.log, gitignored-by-hand: rm before commit) so the host can poll it live. And run the
  gate as a DETACHED container (`docker run -d ... > /uk/scratch_gate.log`) so it survives turn
  interruptions; read the in-tree log, or `docker cp` from the (no --rm) container after it exits.
- macOS has **no `setsid`** (a Linux util) -- a `nohup setsid` detach on the Mac fails instantly; it
  works on the Pi.

## INCREMENT-2 REMAINING (the honest scope line)

The CORE of increment 2 (identity + login-switch + crypt + the util batch) is DONE + HW-validated.
What is left:

1. **seq + printf (the util)** -- both need **float printf (`%f`/`%g`/`%e`)** in libaios (the printf
   core has flags/width/precision but no floating point yet). seq also defaults to a float format. Once
   libaios printf grows %f/%g, both build (the Makefile rules + utf/estrtod wiring are already scoped:
   seq = eprintf+estrtod+strtonum+fshut; printf = the utf chain + estrtod/unescape).
2. **/etc/inittab services + clean shutdown.** init currently hardcodes one getty (respawn /bin/login).
   Next: init reads an /etc/inittab-style config to start services, and a clean `halt`/`poweroff`/
   `reboot`. NEEDS A DESIGN CHOICE: AIOS shutdown = the AIOS kernel (aios-uk) exits cleanly. Likeliest
   shape: a new ABI syscall (REBOOT/SHUTDOWN) the kernel handles by stopping its run loop with a
   poweroff/reboot exit code; the appliance's PID-1 (/init, aios_init.c) maps that code to a real Linux
   poweroff/reboot. (Or: init exiting => aios_kernel_run returns => aios-uk exits.) A small halt util or
   an init signal triggers it. Worth a quick check with Bryan before building.

Also still pending from the broader roadmap (unchanged): ls -l OWNERSHIP reflecting an AIOS uid (file
ownership is host-backed today -- a bigger metadata-overlay change, NOT done); the endgame (sched_ext /
pal_sel4.c / the from-source 6.18 kernel build).

## Dev loop (carry forward)

- colima: `UK=.../AIOS/uk; docker run -d --platform linux/arm64 --cap-add=SYS_PTRACE -v "$UK":/uk -w
  /uk gcc:13 sh -c 'stdbuf -oL sh gate.sh > /uk/scratch_gate.log 2>&1'` then poll `$UK/scratch_gate.log`
  for `RESULT:` (detached survives turn interruptions; rm the log before committing). Or `sh uk/run.sh`
  (foreground, but a turn interruption kills it).
- HW: rsync uk/ to the Pi (artifact-excludes + `/scratch_gate.log` + `/gate_hw.log` + `/.pal.stamp`),
  then `ssh pi 'cd ~/uk && rm -f gate_hw.log && nohup setsid sh -c "sh gate.sh > gate_hw.log 2>&1" &'`;
  poll `gate_hw.log` for `RESULT: linux=0 seccomp=0` via fresh ssh.
- GOTCHAS (carried): the GATEWAY for seccomp; `.pal.stamp` forces a rebuild on a PAL switch; `stdbuf
  -oL`; gcc 14 (Pi) stricter than gcc 13; NO apostrophes in a `sh -c '...'` body; a ptrace hang is
  SILENT (timeout + fprintf).

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue building AIOS as a **gVisor-style userspace kernel on Linux** (the 2026-06-24 pivot off
seL4/RPi4 -- Linux is the interim substrate, verified seL4-on-x86-64 is the destination; verification is
the soul; programs see only the AIOS ABI, the host sits behind a narrow PAL). READ FIRST: memory
[[project_pivot_linux_userspace_kernel]] + docs/HANDOVER_20260627_session25.md +
docs/HANDOVER_20260627_session24.md + docs/AIOS_KERNEL_DEPENDENCIES.md + uk/README.md.

WORKING BRANCH = **`main`** (commit per milestone on main, Bryan pushes). The `uk/` tree: a host-agnostic
kernel (kernel/aios_kernel.c includes ONLY aios_abi.h + aios_version.h + pal.h) over a SHARED Linux
host-driver core (pal/pal_linux_common.c) + TWO trap front-ends (pal/pal_linux.c = PTRACE_SYSCALL,
pal/pal_seccomp.c = seccomp RET_TRACE; `make PAL=linux|seccomp`) + libaios + shadow standard headers
(lib/include, -nostdinc).

DONE through **v0.5.28, 54-syscall ABI**: OPERATIONAL (vendored dash + 26 sbase utils run UNMODIFIED) +
the boundary COMPLETE + FULL JOB CONTROL + raw termios + a SECOND PAL backend (seccomp via the GATEWAY)
+ a minimal Linux-6.18 appliance + a Pi demo.sh + the SYSTEM LAYER: inc 1 (init -> login -> session ->
logout -> respawn) AND **the core of inc 2 -- a real IDENTITY model (per-process uid/gid, 6 new
syscalls), login SWITCHES USER, crypt() SHA-512 password hashing (byte-identical to openssl passwd -6),
and more utils (uname reports AIOS not Linux / env / printenv / pwd / tty / date / tr / cut)**. ALL
HW-validated on the RPi4 (gcc 14.2, kernel 6.12.47; `sh gate.sh` -> linux=0 seccomp=0). NEVER patch
vendored sources -- grow libaios.

PRIMARY TASK -> finish the SYSTEM LAYER (increment 2) then the endgame -- ASK Bryan which first:
(A) **inc 2 remainder**: float printf (%f/%g/%e) in libaios -> then sbase seq + printf run UNMODIFIED;
AND /etc/inittab services + a clean shutdown (NEEDS a small design choice -- a REBOOT/SHUTDOWN ABI the
kernel handles by exiting its run loop with a poweroff/reboot code that the appliance PID-1 maps to a
real Linux poweroff; confirm the shape with Bryan first). (B) **the endgame**: sched_ext (the 6.18
appliance can carry CONFIG_SCHED_CLASS_EXT) / the seL4/x86-64 replant seam (pal_sel4.c -- a THIRD PAL
backend proving kernel/aios_kernel.c runs UNCHANGED on a verified base; M9's PAL_RESUME/host-driver
split de-risked it) / the from-source minimal-6.18 kernel build + a Pi "boots into AIOS" deploy. Keep
kernel/aios_kernel.c host-agnostic + the PAL seam minimal. Commit per milestone on `main`; validate
colima + the Pi (`raspberrypi.local`, pi/aios) via `sh gate.sh`; Bryan pushes. GOTCHAS: use the ABSOLUTE
.../AIOS/uk path for docker -v (never $PWD -- git drifts it); write the gate log INTO uk/ (colima only
mounts $HOME) + run the gate DETACHED (docker run -d) so a turn interruption doesn't kill it; the
intermittent seccomp dash-pipe stall (timeout-guarded now); the GATEWAY for seccomp; .pal.stamp; gcc 14
strictness; NO apostrophes in a `sh -c '...'` body.
