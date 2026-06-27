# HANDOVER -- session 22 (2026-06-27): grep + passwd DB + the WHOLE job-control arc + termios + a root image

Continues the userspace-kernel work (the 2026-06-24 pivot: AIOS as a gVisor-style userspace kernel on
Linux; verified seL4-on-x86-64 is the destination; verification is the soul). Session 21 completed the
boundary (M4.2 fs + M4.3 exec) and the POSIX process attributes. **Session 22 finished the last major
coreutil (`grep`, on a real regex engine), gave `ls -l` real names (a passwd/group DB), built the
ENTIRE job-control arc end to end (process groups -> stop/continue -> sigprocmask -> terminal-signal
routing -> dash `JOBS=1` with full `^C`/`^Z`/`fg`/`bg`), added a real termios raw-mode layer, packaged
AIOS as a confined "disk image", and fixed SIGPIPE.** All on `main`, **10 commits, v0.5.13 -> v0.5.21,
ABI 41 -> 48**. Colima-verified AND HW-validated natively on the RPi4 (Linux 6.12.47/aarch64, gcc 14.2).
Live demo: `uk/run.sh` (a 16-key gate, see below). Live state: memory
[[project_pivot_linux_userspace_kernel]]. Design: docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md.
README: uk/README.md.

## What shipped this session (all on `main`, oldest first)

- **grep `5b22779` (v0.5.13).** The last major coreutil. Vendored sbase `grep` runs UNMODIFIED on a
  real POSIX **regex engine** added to libaios (regcomp/regexec/regfree/regerror). Design (correctness
  is the soul): parse -> AST -> Thompson NFA program -> **LINEAR NFA simulation** (no catastrophic
  backtracking, guaranteed halt). BRE + ERE: literals, `.`, bracket classes (+POSIX `[:class:]`), `^`/
  `$`, `\<`/`\>`, grouping, `|`, `* + ? {m,n}`, REG_ICASE. Boolean match (grep compiles REG_NOSUB).
  Plus libc gaps: `fmemopen`, `sprintf`, `strcasestr`, shadow `<strings.h>`. Proof: guest/prog_regex.c
  (75-case battery, gated).
- **passwd/group DB `04c104c` (v0.5.14).** getpwuid/getpwnam/getgrgid/getgrnam parse /etc/passwd +
  /etc/group, so `ls -l` shows NAMES (returned NULL before). A missing file -> NULL -> numeric
  fallback (a confined guest unaffected). Proof: guest/prog_pwgrp.c. Strong HW proof: the Pi runs as
  user `pi` (uid 1000) -> `ls -l` of a pi-owned file shows `pi pi`.
- **JOB CONTROL -- a 4-part arc, ALL DONE.** This is the headline of s22.
  - **inc 1 `b7ef535` (v0.5.15, ABI->45): the process-group FOUNDATION.** proc_t.pgid (inherited fork,
    preserved exec; init its own leader) + a controlling-terminal foreground group (g_fg_pgrp). 4
    syscalls SETPGID/GETPGID/TCSETPGRP/TCGETPGRP (+ getpgrp=getpgid(0), killpg=kill(-pgrp)); KILL with
    pid<=0 signals a process group. State+syscalls only, M5 ^C untouched. Proof: guest/prog_jobctl.c.
  - **inc 2 `f997925` (v0.5.16): STOP/CONTINUE.** PS_STOPPED state -- "stopped" = "don't resume until
    SIGCONT" (the kernel already owns when each guest runs: kreturn plants the result via setret, then
    does NOT resume). SIGSTOP/SIGTSTP default-stop; SIGCONT resumes (KILL special-cases it). wait
    WUNTRACED (stopped child, status (sig<<8)|0x7f) / WCONTINUED (0xffff) / WNOHANG. No new ABI. Proof:
    guest/prog_stop.c.
  - **inc 3 part 1 `c5361c2` (v0.5.17, ABI->46): a real SIGPROCMASK.** proc_t.sig_mask (blocked-signal
    bitmask, inherited fork); a blocked pending signal waits until unblocked. SIGKILL/SIGSTOP never
    blockable. Single pending slot (documented). Proof: guest/prog_sigmask.c.
  - **inc 3 part 2 `5e1e66b` (v0.5.18): TERMINAL-SIGNAL ROUTING.** The interactive payoff. Guests
    moved OFF the kernel's host pgrp (setpgid(0,0) in the spawn child), so the host pty signals only
    the kernel; the kernel catches SIGINT+SIGTSTP (a SIGTSTP handler is also what stops the *kernel*
    being ^Z-suspended), pal_guest_next surfaces it as a new event (3), and forward_terminal_signal()
    delivers to ONLY g_fg_pgrp. **KEY LESSON (a real ptrace hazard, caught by ctrlc_job_pty):** the
    forward MUST go through the kernel's own pending-signal path, NEVER a host kill of a tracee -- a
    tracee stopped at a not-yet-serviced syscall queues the host signal and pal_guest_setret's
    run-to-exit then EATS it. So a RUNNING fg guest takes it at its next syscall, a PARKED one's
    blocked syscall returns EINTR; and the special syscalls read/write/wait got an entry-time
    deliver_pending() check. dash stays JOBS=0 here. Proof: test/ctrlc_job_pty.c (a fg ./prog_loop, ^C,
    dash returns to its prompt).
  - **inc 3 part 3 `92d597c` (v0.5.19): dash JOBS=1 -- FULL job control.** dash setpgid's each job +
    tcsetpgrp's the fg, so ^C hits ONLY the fg job, ^Z suspends (-> "[1]+ Stopped"), fg/bg resume.
    THREE build-side pieces: (a) a shadow `<termios.h>` (jobs.c #includes it, calls no line-discipline
    fns); (b) the fg/bg builtins were MISSING ("fg: not found") -- configure had stripped them, so
    REGENERATE builtins.{def,c,h} from builtins.def.in with JOBS=1 via dash's mkbuiltins (`cd
    vendor/dash/src; cc -E -x c -include ../config.h -o builtins.def builtins.def.in; sh mkbuiltins
    builtins.def`; the fgcmd/bgcmd functions were already in jobs.c); (c) strsignal extended to 31 so
    SIGTSTP -> "Stopped". Proof: test/ctrlz_pty.c (^Z -> prompt -> fg -> ^C).
- **termios raw mode `c94a8f1` (v0.5.20, ABI->48).** TCGETATTR/TCSETATTR proxy to the host tty, so
  cfmakeraw RAW mode (clear ICANON/ECHO/ISIG) gives char-at-a-time, unechoed input. The mechanism is
  free: the kernel already reads the pty on the guest's behalf, so once a guest puts the *host* pty in
  raw mode the kernel's reads return one key at a time. struct aios_termios + a full shadow
  `<termios.h>` (flag values match the host -> the PAL translation is a field copy); cfmakeraw + cf-speed
  helpers are inline in the header. Proof: guest/prog_rawkey.c via test/rawkey_pty.c (one byte, NO
  Enter -> "rawkey got: Z" unechoed).
- **mkaiosroot.sh `a1a0def`.** AIOS is a userspace kernel, so there's no bootable AIOS kernel image;
  the analog is the AIOS ROOT FILESYSTEM -- the AIOS-ABI userland the kernel serves + CONFINES via
  AIOS_ROOT. The script assembles one (dash as /bin/sh, sbase utils at standard names, /etc/passwd +
  group) and tars it. Demonstrated: a fully confined AIOS system runs entirely from the image (shell,
  ls -l with names, pipelines, grep), host fs unreachable. **GOTCHA:** the INIT binary is the trusted
  entry loaded by its HOST path -- `aios-uk /bin/sh` would load the host's /bin/sh and get escape-killed
  (159); name the image's shell by its real path. Build outputs gitignored.
- **SIGPIPE `0ccf6e8` (v0.5.21).** A guest write()ing to a pipe with no readers now gets SIGPIPE (13;
  default terminates -> `producer | head` is quiet, ignored/caught -> -EPIPE) -- before it only got
  EPIPE, so a producer flushing buffered stdout after the reader closed printed a spurious "ls: ferror
  <stdout>: Success". Fix: pipe_no_reader() at the two PAL_EPIPE sites routes through kreturn (which
  applies the disposition). The kernel keeps ignoring HOST SIGPIPE. Proof: guest/prog_sigpipe.c. (A
  spawned-task chip flagged this; done same session.)

## AIOS ABI now (48 syscalls; literal define count is 47, 0x1000-0x102E)

(The docs' running "ABI -> N" tally has run one high since M3i -- `grep -c '^#define AIOS_SYS_'
include/aios_abi.h` is 47. Kept "48" for continuity with the version-header changelog.)

… READLINK/FCNTL/SIGACTION/SIGRETURN/KILL/ISATTY/CLOCK_GETTIME/FCHMODAT/FCHOWNAT/SYMLINKAT/LINKAT/
UTIMENSAT/UMASK/**SETPGID/GETPGID/TCSETPGRP/TCGETPGRP/SIGPROCMASK/TCGETATTR/TCSETATTR**. **Working
vendored, UNMODIFIED:** sbase true/false/echo/cat/wc/mkdir/rm/ls(+`ls -l`)/head/tail/cp/mv/ln/chmod/
sort/grep AND **dash** (now `JOBS=1` -- full interactive job control).

## Dev loop (carry forward)

- `uk/run.sh` (colima aarch64 `gcc:13` container, `--cap-add=SYS_PTRACE`; rc=0 gates the suite). The
  gate now has **16 keys**, all must exit 0: pipebig jail execjail clock pcwd umask regex pwgrp jobctl
  stop sigmask sigpipe **ctrlc ctrlc-job ctrlz rawkey** (the last four are pty tests built on the host
  with `cc ... -lutil` -- forkpty). libc-program class: `-nostdinc -isystem $(cc -print-file-name=include)
  -Ilib -Ilib/include`; dash adds `-include vendor/dash/config.h -DSHELL -DSMALL -DGLOB_BROKEN -w`.
- HW (native, no docker): `rsync -az --delete --exclude='/aios-uk' --exclude='/guest_*' --exclude='/prog_*'
  --exclude='/sbase-*' --exclude='/dash' --exclude='*.o' --exclude='/aiosroot' --exclude='/aiosroot.tar'
  uk/ pi@raspberrypi.local:~/uk/` then `ssh pi@raspberrypi.local 'cd ~/uk && make clean && make -j4 all'`
  (pi/aios). aios-uk traces its own child -> NO sudo/cap on the Pi.
- GOTCHAS (carried): Pi DHCP lease rotates -> use mDNS `raspberrypi.local`; ANCHOR rsync excludes with
  `/`; **gcc 14 (Pi) is stricter than colima's gcc 13** (implicit-decl = error; -Wcomment; static-after-
  non-static); colima virtiofs LAGS after a host edit -- `sync` + a read-probe (or retry) first; **NO
  apostrophes in run.sh's docker `sh -c '...'` body** [[feedback_script_style]]; a ptrace hang is SILENT
  -> in-container `timeout N` + fprintf(stderr). NEW this session: a `*/` inside a libaios comment
  (`cf*/cfmakeraw`) closed the comment early -- the -Wcomment family of gotcha.

## Key technical notes (carry forward)

- **The job-control delivery model is the subtle part.** Guests are OFF the kernel's host pgrp; the
  kernel routes terminal signals to g_fg_pgrp via the guests' own pending-signal path (kreturn /
  deliver_pending), NEVER a host kill of a tracee (the setret/queued-signal hazard). The two pty tests
  ctrlc_job_pty + ctrlz_pty are the regression guard for this; keep them green.
- **dash's generated sources are JOBS-sensitive.** builtins.{def,c,h} were regenerated for JOBS=1; if
  the dash version is ever bumped or JOBS toggled, regenerate them from builtins.def.in (see inc-3-part-3
  above). config.h has JOBS 1 + HAVE_KILLPG 1 + HAVE_STRTOD 1 + HAVE_STRSIGNAL 1 (AIOS-authored).
- **The PAL grew a little but stayed the seam.** New host ops: pal_take_term_signal (terminal-signal
  routing), pal_host_tcgetattr/tcsetattr (termios). kernel/aios_kernel.c still includes ONLY abi +
  version + pal.h (host-agnostic). The termios struct flag values match the host (a field-copy PAL
  translation, like AIOS errno == Linux); a seL4 PAL would remap.
- **AIOS as a system:** mkaiosroot.sh + AIOS_ROOT gives a confined AIOS userland ("disk image"). For a
  Pi that "boots into AIOS", the deploy step is a getty/systemd unit launching aios-uk against an image
  on the console -- a config on top of Raspberry Pi OS, NOT a kernel image (the pivot put Linux under).
- **Known degradations (honest):** regexec is boolean-only (no submatch; \1..\9 rejected -- nothing in
  scope needs them). The sig pending slot is single (one masked signal at a time). <termios.h> is a
  real raw-mode layer but cfmakeraw is the only mode helper; no full line editor vendored yet.

## NEXT -- the endgame (both have external blockers Bryan must clear)

1. **sched_ext** -- AIOS authors its own scheduling policy as a sched_ext BPF program. The STOCK RPi
   kernel lacks CONFIG_SCHED_CLASS_EXT (/sys/kernel/sched_ext absent) -> needs a CUSTOM RPi kernel build
   with it (6.12 has the upstream code, just not enabled). I can write the BPF policy + PAL install hook
   but can't validate without the kernel.
2. **the seL4/x86-64 REPLANT SEAM (`pal_sel4.c`)** -- a second PAL backend proving kernel/aios_kernel.c
   compiles + runs UNCHANGED on a verified base (the whole point of the PAL seam). Needs an seL4/x86-64
   env. Doable-here first step: a PAL-seam audit + a compiling pal_sel4.c skeleton (proves
   host-agnosticism). A validatable alternative not on the roadmap: a **second Linux PAL (seccomp/SIGSYS)**
   -- same kernel, different host mechanism, fully testable here, the strongest portability proof short
   of seL4. (Bryan chose polish over the endgame in s22; the endgame is teed up for whenever.)
3. Smaller polish if wanted: more interactive apps now that raw mode exists; more coreutils.

Keep kernel/aios_kernel.c host-agnostic + the PAL seam minimal; NEVER patch vendored sources -- grow
libaios. The seL4 stall + lead-#3 keyboard stay MOOTED by leaving the platform (seL4 tree preserved on
main/origin as record/fallback).

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue building AIOS as a **gVisor-style userspace kernel on Linux** (the 2026-06-24 pivot off
seL4/RPi4 -- Linux is the interim substrate, verified seL4-on-x86-64 is the destination; verification
is the soul; programs see only the AIOS ABI, the host sits behind a narrow PAL). READ FIRST: memory
[[project_pivot_linux_userspace_kernel]] + docs/HANDOVER_20260627_session22.md +
docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md + uk/README.md.

WORKING BRANCH = **`main`** (the 0.5.x userspace kernel; commit on main, Bryan pushes). The `uk/` tree:
a host-agnostic kernel (kernel/aios_kernel.c includes ONLY aios_abi.h + aios_version.h + pal.h) over
the ONLY host-aware file (pal/pal_linux.c, a multi-process PTRACE_SYSCALL driver) + libaios + shadow
standard headers (lib/include, used with -nostdinc).

DONE through **v0.5.21, 48-syscall ABI -- OPERATIONAL + the boundary COMPLETE + POSIX process
attributes + FULL JOB CONTROL + raw terminal mode**. Real vendored `sbase` (true/false/echo/cat/wc/
mkdir/rm/ls(+`ls -l`)/head/tail/cp/mv/ln/chmod/sort/grep) AND `dash` run UNMODIFIED; **dash is now
`JOBS=1` with full interactive job control** (`^C` hits only the foreground job, `^Z` suspends,
`fg`/`bg` resume -- proven on a pty + the RPi4). `grep` runs on a real LINEAR NFA-simulation regex
engine in libaios; `ls -l` shows real names via a passwd/group DB; **tcgetattr/tcsetattr** give RAW
terminal mode; a broken-pipe write raises **SIGPIPE**. The BOUNDARY is enforced + sound: M4 (no guest
syscall reaches the host) + M4.2 fs confinement (AIOS_ROOT -> openat2 RESOLVE_IN_ROOT) + M4.3 exec
confinement. `mkaiosroot.sh` packages a confined AIOS root image. Vendored sources NEVER patched --
missing libc goes into libaios. Validated on colima AND **natively on the real RPi4** (Linux
6.12.47/aarch64, gcc 14.2).

DEV LOOP: `uk/run.sh` (colima aarch64 container, --cap-add=SYS_PTRACE; rc=0 gates a 16-key suite incl.
4 pty tests built with `cc ... -lutil`). HW: `rsync -az --delete --exclude='/aios-uk' --exclude='/guest_*'
--exclude='/prog_*' --exclude='/sbase-*' --exclude='/dash' --exclude='*.o' --exclude='/aiosroot'
--exclude='/aiosroot.tar' uk/ pi@raspberrypi.local:~/uk/` then `ssh pi@raspberrypi.local 'cd ~/uk &&
make clean && make -j4 all'` (pi/aios). GOTCHAS: Pi DHCP rotates -> `raspberrypi.local`; ANCHOR rsync
excludes with `/`; gcc 14 (Pi) stricter than colima gcc 13; colima virtiofs LAGS (sync + retry); NO
apostrophes in run.sh's docker `sh -c '...'` body [[feedback_script_style]]; watch `*/` inside C
comments; a ptrace hang is SILENT -> in-container `timeout N` + fprintf(stderr).

PRIMARY TASK -> the ENDGAME (both endgame items have external blockers -- ASK Bryan which to pursue, or
do a validatable-here step): (1) **sched_ext** -- AIOS's own scheduling policy as a sched_ext BPF
program; the stock RPi kernel lacks CONFIG_SCHED_CLASS_EXT so it needs a CUSTOM RPi kernel build (Bryan's
hardware task) -- writeable here, not validatable. (2) **the seL4/x86-64 replant seam (`pal_sel4.c`)** --
a second PAL backend proving kernel/aios_kernel.c runs UNCHANGED on a verified base (needs an seL4 env);
the doable-here first step is a PAL-seam audit + a compiling pal_sel4.c skeleton. A validatable
alternative NOT on the roadmap but the strongest portability proof short of seL4: **a second Linux PAL
(seccomp/SIGSYS)** -- same kernel, different host trap mechanism, fully testable on colima + the Pi.
Keep kernel/aios_kernel.c host-agnostic + the PAL seam minimal; NEVER patch vendored sources -- grow
libaios. Commit per milestone on `main`; validate colima + the Pi (`raspberrypi.local`); Bryan pushes.
