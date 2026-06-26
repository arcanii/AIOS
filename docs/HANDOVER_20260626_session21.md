# HANDOVER -- session 21 (2026-06-26): the boundary COMPLETED + POSIX process attributes + more utils

Continues the userspace-kernel work (the 2026-06-24 pivot: AIOS as a gVisor-style userspace kernel on
Linux; verified seL4-on-x86-64 is the destination; verification is the soul). Session 20 reached
OPERATIONAL (sbase + dash unmodified), enforced the syscall-bypass boundary (M4), and delivered
signals + interactive ^C (M5). **Session 21 COMPLETED the confinement story (M4.2 fs + M4.3 exec),
filled out the libc/util surface (head/tail/cp/mv/ln/chmod/sort + a real clock + a file-metadata
layer), and made the process attributes POSIX-correct (per-process cwd + per-process umask).** All on
`main`, **8 commits, v0.5.5 -> v0.5.12, ABI 34 -> 41**. Colima-verified AND HW-validated natively on
the real RPi4 (Linux 6.12.47/aarch64, gcc 14.2). Live demo: `uk/run.sh` (the gate runs prog_pipebig +
prog_jail + prog_execjail + prog_clock + prog_pcwd + prog_umask, all must exit 0). Live state: memory
[[project_pivot_linux_userspace_kernel]]. Design: docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md.
README: uk/README.md.

## What shipped this session (all on `main`)

- **M4.2 fs confinement `9b518a4` (v0.5.5).** The OTHER half of the boundary: when the PAL is launched
  with `AIOS_ROOT` set, every guest file path resolves INSIDE that root via
  **openat2(RESOLVE_IN_ROOT)** -- absolute paths, `..`, and symlinks (absolute or `..`) are all
  clamped to the root by the host kernel. UNPRIVILEGED (no chroot/CAP -> runs as user `pi`), PURELY a
  PAL policy: the kernel + ABI are UNCHANGED (zero new syscalls). Default (`AIOS_ROOT` unset) = whole
  host, byte-identical. Establishes the root once (pal_fs_init_once), FAILS CLOSED. Covers open/openat,
  stat/lstat/fstatat (O_PATH handle + fstat), unlink/rmdir/mkdir/rename/readlink (confined parent +
  single non-walking leaf), faccessat, chdir/getcwd (a PAL-side logical cwd -- LATER moved to the
  kernel, see per-process cwd). Proof: **guest/prog_jail.c**. De-risked first with a standalone openat2
  probe (Docker 29 default seccomp permits it).
- **M4.3 exec confinement `b1e2408` (v0.5.6).** A guest-issued exec (AIOS_SYS_EXEC) is resolved INSIDE
  the root too: openat2(RESOLVE_IN_ROOT|O_PATH) -> readlink(/proc/self/fd) = a canonical in-root host
  path -> staged into the guest stack -> execve. The INIT program the operator names is the trusted
  entry and is EXEMPT (only a guest's OWN exec routes through pal_guest_exec). KEY FIX: a denied exec
  must `pal_guest_setret(-errno)` to neutralize the AIOS_SYS_EXEC svc, else the bogus 0x1008 runs as a
  non-existent host syscall on resume and clobbers x0 with -ENOSYS. Proof: **guest/prog_execjail.c**.
- **head/tail/cp/mv `f1fa3ee` (v0.5.7).** Four more vendored sbase utils UNMODIFIED. libaios grew
  getline/getdelim, creat, basename/dirname, llabs + a no-op sleep, and (then) honest no-op/ENOSYS
  stubs for cp/mv file-metadata. Two real fixes fell out: a long-standing **fopen-errno bug** (it
  called raw aios_open so every fopen failure said "Success"; now routes through __ret) and an
  **openat2-strict mode mask** (st_mode with S_IFREG bits -> EINVAL; openat2_in_root now masks `& 07777`
  -- this broke confined cp until fixed). New shadow headers <utime.h>, <libgen.h>; SIZE_MAX/SSIZE_MAX.
- **a real CLOCK `f6d29a2` (v0.5.8, ABI -> 35).** AIOS_SYS_CLOCK_GETTIME reads the host clock via the
  PAL (pal_host_clock_gettime -> clock_gettime(2); CLOCK_REALTIME/MONOTONIC). time()/clock_gettime()/
  gettimeofday() are live; time() no longer returns 0, so `ls -l` dates use the real recent-vs-old
  format. Proof: **guest/prog_clock.c** (formatted UTC matches host `date -u` exactly).
- **file-metadata layer `9b3b744` (v0.5.9, ABI -> 40).** 5 confinement-aware *at syscalls FCHMODAT/
  FCHOWNAT/SYMLINKAT/LINKAT/UTIMENSAT + AIOS_AT_SYMLINK_FOLLOW; libaios real wrappers (chmod=fchmodat
  AT_FDCWD; lchown=fchownat NOFOLLOW; symlink/link via the *at forms; **asys5** added). cp -p preserves
  mode + times; **ln / ln -s / chmod** run UNMODIFIED. CONFINEMENT-CRITICAL: chmod/chown/utimensat
  follow the final symlink in the HOST ns, so confined single-target ops resolve via openat2 +
  /proc/self/fd FIRST (a **confined_canon** helper) -- a planted in-root symlink can't redirect a
  metadata change to a host file; prog_jail.c now asserts this. fchmod/fchown/mknod -> ENOSYS.
- **per-process cwd `b252c2d` (v0.5.10, ABI UNCHANGED).** cwd moved from a single PAL-global to
  proc_t.cwd (inherited across fork, preserved across exec). The kernel pre-absolutes EVERY guest path
  against the calling process's cwd (read_abspath for plain ops; read_at for the *at family, cwd-joined
  only when AT_FDCWD). The exec path is the subtle part: **pal_guest_exec's signature changed from a
  guest pointer to a kernel-resolved absolute path** (the kernel cwd-joins it; the PAL stages it via
  stage_str in both modes) -- an already-forked tracee's real cwd can't be changed, so absolute-path-
  staging is the only correct way. The PAL is now CWD-FREE (pal_host_chdir verify-only; pal_host_getcwd
  only seeds init's cwd; g_cwd / PAL path_norm / pal_confine_exec removed). Fixes the shared-cwd bug (a
  subshell's `cd` moved everyone). Proof: **guest/prog_pcwd.c** + dash `cd /tmp; (cd /); pwd` -> `/tmp`.
- **per-process umask `e90db7b` (v0.5.11, ABI -> 41).** AIOS_SYS_UMASK + proc_t.umask (default 022,
  inherited across fork, PRESERVED across exec). The kernel applies it on open(O_CREAT)/openat(O_CREAT)/
  mkdir; the **host umask is neutralized (umask(0) in pal_guest_spawn)** so this single mask governs.
  libaios umask() became a thin syscall (was a no-op local tracker). Proof: **guest/prog_umask.c** +
  dash `umask 077; ./sbase-mkdir` -> a 0700 dir (the mask SURVIVES the dash->mkdir exec).
- **sort `fc8c03b` (v0.5.12, ABI UNCHANGED).** vendored sbase `sort` UNMODIFIED (lexical/-u/-n incl.
  decimals/-r). libaios grew a **real strtod** (aarch64 HW FP, no soft-float runtime in the -nostdlib
  guest); the Makefile rule wires the full libutf rune chain (rune/runetype + the predicate runes +
  **lowerrune.c, which defines `toupperrune`**) + libutil unescape/strlcpy/writeall. GOTCHA: declaring
  strtod in shadow <stdlib.h> collided with dash's `static double strtod` no-op fallback (gcc 14 errors,
  gcc 13 warned); fixed by `#define HAVE_STRTOD 1` in dash's config.h.

## AIOS ABI now (41 syscalls)

WRITE/READ/OPEN/CLOSE/EXIT/MMAP/FSTAT/LSEEK/EXEC/FORK/WAIT/PIPE/DUP2/STAT/LSTAT/GETCWD/CHDIR/UNLINK/
MKDIR/RMDIR/RENAME/GETPID/GETDENTS/OPENAT/FSTATAT/UNLINKAT/FACCESSAT/READLINK/FCNTL/SIGACTION/SIGRETURN/
KILL/ISATTY/CLOCK_GETTIME/FCHMODAT/FCHOWNAT/SYMLINKAT/LINKAT/UTIMENSAT/UMASK. **Working vendored,
UNMODIFIED:** sbase true/false/echo/cat/wc/mkdir/rm/ls(+`ls -l`)/head/tail/cp/mv/ln/chmod/sort AND
**dash** (the operational shell).

## Dev loop (unchanged from s20, plus reminders)

- `uk/run.sh` (colima aarch64 `gcc:13` container, `--cap-add=SYS_PTRACE`; rc=0 gates the suite). The
  gate now requires prog_pipebig + prog_jail + prog_execjail + prog_clock + prog_pcwd + prog_umask all
  exit 0. libc-program class: `-nostdinc -isystem $(cc -print-file-name=include) -Ilib -Ilib/include`;
  vendored-sbase adds `-Ivendor/sbase -w`; dash adds `-Ivendor/dash/src -include vendor/dash/config.h
  -DSHELL -DSMALL -DGLOB_BROKEN -Dalloca=__builtin_alloca -w`.
- HW (native, no docker): `rsync -az --delete --exclude='/aios-uk' --exclude='/guest_*'
  --exclude='/prog_*' --exclude='/sbase-*' --exclude='/dash' --exclude='*.o' uk/ pi@raspberrypi.local:~/uk/`
  then `ssh pi@raspberrypi.local 'cd ~/uk && make clean && make -j4 all'` (pi/aios). aios-uk traces its
  own child -> NO sudo/cap on the Pi.
- GOTCHAS (carried + new this session): the Pi's DHCP lease rotates -> use mDNS `raspberrypi.local`.
  ANCHOR the rsync excludes with `/` or you ship stale guest/prog_*.c sources. **gcc 14 (Pi) is
  stricter than colima's gcc 13** -- implicit-decl = error; it also flagged a `/*`-in-comment
  (-Wcomment), a static-after-non-static `strtod` conflict (the dash HAVE_STRTOD fix), and forced the
  forward-decls for read_abspath/openat2_in_root/g_confined. Colima's virtiofs mount LAGS after a host
  edit -- `sync` + a `grep -c <newthing> <file>` probe in a throwaway container (or just retry once)
  before the real build. **NO apostrophes in run.sh's docker `sh -c '...'` body** (a "child's" closed
  the quote -- bit me this session) [[feedback_script_style]]. A ptrace hang is SILENT -> in-container
  `timeout N` + fprintf(stderr).

## Key technical notes (carry forward)

- **The boundary is now COMPLETE + sound:** M4 (no guest syscall reaches the host) + M4.2 (a serviced
  open/stat/... reaches only an AIOS root) + M4.3 (a guest execs only in-root binaries). All proven by
  red-team guests (guest_escape / prog_jail / prog_execjail) wired into the gate. The recurring
  confinement primitive: **openat2(g_root_fd, path, RESOLVE_IN_ROOT)**; for ops that follow a final
  symlink in the host ns (exec, chmod/chown/utimensat) resolve to a canonical in-root host path via
  **/proc/self/fd FIRST** (confined_canon / the exec staging), then operate on that. Create ops
  (symlinkat/linkat) confine the PARENT dir and store the symlink target verbatim.
- **The PAL got SMALLER, not bigger, on the seam-sacred front:** confinement (M4.2/M4.3) added zero
  ABI, and per-process cwd REMOVED g_cwd / PAL path_norm / pal_confine_exec from the PAL (cwd is a
  kernel/process concept now). pal_guest_exec's signature is now `(who, const char *abspath, gargv,
  genvp)` -- a kernel string the PAL stages, not a guest pointer.
- **Process attributes are POSIX-correct:** cwd + umask both live in proc_t, inherited across fork AND
  preserved across exec. The kernel pre-absolutes paths against proc_t.cwd and masks create-modes with
  proc_t.umask; the host umask is neutralized so the guest's umask is authoritative.
- **libaios discipline holds:** still freestanding; strtod is the first FP (aarch64 HW FP, no libgcc/
  soft-float needed in -nostdlib). The single libaios printf supports flags/width/precision + %o.
- **Known degradations (honest):** getpwuid/getgrgid -> NULL (ls numeric ids). <regex.h> is decls-only
  (grep needs a real engine). fchmod/fchown/mknod -> ENOSYS (no fd-metadata / device syscalls). dash is
  built JOBS=0 (M5 ^C is line-interrupt, not job control).

## OPEN ISSUE for Bryan to decide (flagged via a task chip)

**dash's generated build inputs are gitignored + untracked.** `vendor/dash/config.h` AND
`vendor/dash/src/{syntax,nodes,builtins,init,signames}.c`, `token.h`, `arith.h` are ignored by
`vendor/dash/.gitignore` (inherited from dash's upstream autoconf .gitignore) and were never committed.
The build works because they live in the working tree + rsync to the Pi (the rsync does NOT exclude
them), but **a fresh `git clone` cannot build dash**, and the s21 `#define HAVE_STRTOD 1` edit to
config.h is on disk, NOT in git. config.h is hand-curated (HAVE_STRSIGNAL/HAVE_STRTOD/fstat64 macros),
so it is not reproducible from a clean checkout. Decide: (A) `git add -f` the 8 files (optionally drop
their .gitignore entries) so clones build; or (B) add a documented/scripted regen step in
vendor/README.md. Either way capture HAVE_STRTOD. (A task chip "Track dash generated build inputs (or
document regen)" was spawned.)

## NEXT (the confinement + process-attribute arcs are done)

1. **grep** -- the last major coreutil; needs a REAL `<regex.h>` engine (regcomp/regexec). shadow
   <regex.h> is declarations-only today. The biggest single remaining util task -- give it a dedicated
   pass (port or write a small POSIX BRE/ERE engine into libaios).
2. **full job control** -- dash is built `JOBS=0`; real job control needs setpgid/tcsetpgrp + a termios
   layer (the M5 ^C is line-interrupt, not job control). s15 zsh lessons [[project_zsh_pty]] are
   background.
3. Smaller: more sbase utils as needed (most "easy" ones are done); a passwd/group db (so ls shows
   names not numeric ids).
4. Then **sched_ext** (the stock RPi kernel lacks CONFIG_SCHED_CLASS_EXT -> needs a custom kernel) and
   the **seL4/x86-64 replant seam** (`pal_sel4.c`) -- the endgame that proves the PAL. The seL4 stall +
   lead-#3 keyboard stay MOOTED by leaving the platform (seL4 tree preserved on main/origin as
   record/fallback).

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue building AIOS as a **gVisor-style userspace kernel on Linux** (the 2026-06-24 pivot off
seL4/RPi4 -- Linux is the interim substrate, verified seL4-on-x86-64 is the destination; verification
is the soul; programs see only the AIOS ABI, the host sits behind a narrow PAL). READ FIRST: memory
[[project_pivot_linux_userspace_kernel]] + docs/HANDOVER_20260626_session21.md +
docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md + uk/README.md.

WORKING BRANCH = **`main`** (the 0.5.x userspace kernel; commit on main, Bryan pushes). The `uk/` tree:
a host-agnostic kernel (kernel/aios_kernel.c includes ONLY aios_abi.h + aios_version.h + pal.h) over
the ONLY host-aware file (pal/pal_linux.c, a multi-process PTRACE_SYSCALL driver) + libaios (a C runtime
on the AIOS ABI) + shadow standard headers (lib/include, used with -nostdinc).

DONE through **v0.5.12, 41-syscall ABI -- OPERATIONAL + the boundary COMPLETE + POSIX process
attributes**. Real vendored `sbase` (true/false/echo/cat/wc/mkdir/rm/ls(+`ls -l`)/head/tail/cp/mv/ln/
chmod/sort) AND `dash` run UNMODIFIED (-nostdinc against libaios). The BOUNDARY is enforced + sound:
**M4** (the PAL neutralizes every trapped syscall so a guest syscall NEVER reaches the host; the kernel
kills a guest that emits a non-AIOS syscall) + **M4.2** fs confinement (AIOS_ROOT -> openat2
RESOLVE_IN_ROOT; a serviced open/stat/... reaches ONLY an AIOS root) + **M4.3** exec confinement (a
guest execs only in-root binaries; the operator's INIT is the trusted, exempt entry) -- all
UNPRIVILEGED, mostly zero-ABI PAL policy, proven by guest_escape/prog_jail/prog_execjail (all in the
run.sh gate). **M5** = real signal delivery + interactive ^C. Plus a real **CLOCK_GETTIME**, a
**file-metadata *at layer** (chmod/chown/symlink/link/utimes -> cp -p + ln + chmod), **per-process cwd**
and **per-process umask** (both inherited across fork, preserved across exec -- the PAL is now cwd-free
and pal_guest_exec takes a kernel-resolved absolute path), and a real **strtod** (for sort). Vendored
sources are NEVER patched -- missing libc features go into libaios. Validated on colima AND **natively
on the real RPi4** (Linux 6.12.47/aarch64, gcc 14.2).

DEV LOOP: `uk/run.sh` (colima aarch64 container, --cap-add=SYS_PTRACE; rc=0 gates the suite -- the gate
requires prog_pipebig + prog_jail + prog_execjail + prog_clock + prog_pcwd + prog_umask to exit 0).
HW (native, no docker): `rsync -az --delete --exclude='/aios-uk' --exclude='/guest_*' --exclude='/prog_*'
--exclude='/sbase-*' --exclude='/dash' --exclude='*.o' uk/ pi@raspberrypi.local:~/uk/` then `ssh
pi@raspberrypi.local 'cd ~/uk && make clean && make -j4 all'` (pi/aios). GOTCHAS: the Pi's DHCP lease
rotates -- use the mDNS name `raspberrypi.local`; ANCHOR the rsync excludes with `/`; gcc 14 (Pi) is
stricter than colima's gcc 13 (implicit-decl = error, -Wcomment, static-after-non-static); colima's
virtiofs mount LAGS after a host edit -- `sync` + a read-probe (or retry once) before the first build;
**NO apostrophes in run.sh's docker `sh -c '...'` body** [[feedback_script_style]]; a ptrace hang is
SILENT -> in-container `timeout N` + fprintf(stderr).

PRIMARY TASK -> pick the next milestone (the confinement + process-attribute arcs are DONE): (1) **grep**
-- the last major coreutil; needs a REAL `<regex.h>` engine (regcomp/regexec; shadow <regex.h> is
decls-only). The biggest single remaining util task -- a dedicated pass (port or write a small POSIX
BRE/ERE engine into libaios). (2) **full job control** -- dash built JOBS=0; needs setpgid/tcsetpgrp +
a termios layer (the M5 ^C is line-interrupt, not job control). (3) smaller: a passwd/group db (ls
shows numeric ids today). THEN sched_ext (custom RPi kernel w/ CONFIG_SCHED_CLASS_EXT) and the
**seL4/x86-64 replant seam (`pal_sel4.c`)** -- the endgame that proves the PAL. Keep
kernel/aios_kernel.c host-agnostic + the PAL seam minimal (it got SMALLER this session); NEVER patch
vendored sources -- grow libaios. Commit per milestone on `main`; validate colima + the Pi
(`raspberrypi.local`); Bryan pushes.

OPEN ISSUE (decide early): dash's generated build inputs (vendor/dash/config.h + src/{syntax,nodes,
builtins,init,signames}.c + token.h/arith.h) are gitignored + UNTRACKED, so a fresh `git clone` cannot
build dash and the s21 `#define HAVE_STRTOD 1` config.h edit is on disk but NOT in git. Either `git
add -f` them (and capture HAVE_STRTOD) or add a documented regen step. A task chip was spawned for this.
