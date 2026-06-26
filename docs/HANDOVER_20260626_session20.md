# HANDOVER -- session 20 (2026-06-26): AIOS OPERATIONAL + boundary ENFORCED + signals/^C

Continues the userspace-kernel work (the 2026-06-24 pivot: AIOS as a gVisor-style userspace kernel
on Linux; verified seL4-on-x86-64 is the destination; verification is the soul). A very large session:
finished the libc gaps sbase needed and **vendored real suckless sbase (8 utilities, UNMODIFIED)**;
**vendored real dash (Debian Almquist Shell, UNMODIFIED) -> AIOS is OPERATIONAL**; **M4 enforced the
boundary** (the trap model is SOUND); and **M5 real signal delivery + interactive ^C**. All on `main`,
now **v0.5.4, 34-syscall ABI**. Colima-verified AND HW-validated natively on the real RPi4 (Linux
6.12.47/aarch64, gcc 14.2). Live demo: `uk/test/demo.sh`. Live state: memory
[[project_pivot_linux_userspace_kernel]]. Design: docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md.
README: uk/README.md.

## What shipped this session (all on `main`, ~18 commits)

- **M3e.5 `36b6ee5`** -- `getopt` (POSIX, no permutation) + `qsort` (generic heapsort). Pure libc,
  no new ABI. Proof: prog_getopt.c.
- **M3e.6 `b16ca32`** -- **directory streams**: a new `AIOS_SYS_GETDENTS` (0x1016) + `struct
  aios_dirent` (host-agnostic wire record) + `pal_host_getdents` (getdents64 translated record-by-
  record into equal-size aios records) + `opendir`/`readdir`/`closedir` + shadow `<dirent.h>`.
  Proof: prog_dir.c.
- **M3f `b3e0ed5`** -- **VENDORED sbase** @ `c546c3a` (suckless, MIT) under `vendor/sbase` (as-is
  minus .git; provenance in vendor/README.md). Build class: each util builds as `sbase-<name>` from
  its source + the libutil objects it needs + lib/libaios.c, with `$(LIBC_CFLAGS) -Ivendor/sbase`.
  **sbase is NEVER patched** -- missing libc features go into libaios. First utils:
  `true/false/echo/cat`. Two new shadow headers: `<limits.h>`, `<regex.h>` (DECLARATIONS ONLY --
  util.h includes it for regex_t; echo/cat/wc never call regcomp).
- **M3f.2 `f02ff22`** -- `wc` (incl. the full libutf rune layer) + `bsearch`.
- **M3f.3 `787b080`** -- `mkdir` (+`-p`/`-m`); `umask` (advisory -- see notes) + the full S_I*
  permission bits. Vendored build switched to `-w` (we never lint/patch vendored sources).
- **M3g `3d8f6ae`** -- the **`*at` family**: 4 new syscalls OPENAT/FSTATAT/UNLINKAT/FACCESSAT
  (0x1017..0x101A) + AT_*/?_OK/O_CLOEXEC/O_DIRECTORY + a host-agnostic `PAL_AT_FDCWD` seam +
  `fdopendir`/`dirfd`/`strndup`. Unlocks sbase's `recurse` -> **`rm -r`** over real trees.
- **M3h `0074b5f`** -- **`ls` + `ls -l`**: 1 new syscall READLINK (0x101B); `struct stat` time
  fields are now `struct timespec` (+ scalar `st_*time` macros) and gained real `st_rdev` (reused
  the pad slot, byte-identity preserved); a real UTC `gmtime`/`localtime`/`strftime` time layer;
  `getpwuid`/`getgrgid` return NULL (numeric ids -- no passwd db yet); and a **printf REWRITE**
  (flags `-`/`0`, width incl. `*`, precision incl. `.*`, l/z/h) so `ls -l` columns align. New shadow
  headers `<time.h>`/`<pwd.h>`/`<grp.h>`/`<sys/sysmacros.h>`.
- **`1728c76`** -- version bump 0.5.0 -> **0.5.1** + README updated (M3e done; M3f/g/h documented).
- **M3i `d247f1e` -- dash = OPERATIONAL.** Real dash (Debian Almquist Shell 0.5.11, BSD) compiles
  UNMODIFIED + runs as a real shell: builtins, arithmetic w/ precedence, `&&`/`||`, if/test,
  for/while, var expansion, external exec, multi-stage PIPELINES, command substitution, `>` redirect,
  `$?`, -c/stdin/script-file modes -- driving real sbase `echo | wc | cat` through the process model.
  1 new syscall **FCNTL** (0x101C; `F_DUPFD` = lowest free fd >= arg, how dash parks its script fd
  >10). libaios grew: **setjmp/longjmp + sigsetjmp/siglongjmp** (aarch64 asm -- dash's exception
  mechanism), a **signal layer** (dispositions RECORDED, NOT delivered -- no async PAL path yet;
  -c/scripts never fire one), fcntl/dup/execve/vfork/wait3, sysconf/strtoll/getrlimit/times/strsignal/
  gettimeofday/ioctl (stubs), getpwnam/getppid/getuid/.., string fns (strcasecmp/strspn/strcspn/
  strpbrk/strtok/stpncpy), getopt_long; new errno (ENOEXEC/ELOOP/..) + O_EXCL/O_NONBLOCK + AT_EACCESS;
  new shadow headers signal/setjmp/inttypes/alloca/getopt/sys.{param,resource,time,times,ioctl}.
  Vendored dash @ 057cd650 under uk/vendor/dash; config.h + the generated sources (token/syntax/nodes/
  builtins/signames/init) are AIOS build inputs from dash's own generators (provenance vendor/README.md).
- **`17ba723`** -- version bump 0.5.1 -> **0.5.2** + README marks AIOS operational (M3i documented).

**AIOS ABI now (30 syscalls):** WRITE/READ/OPEN/CLOSE/EXIT/MMAP/FSTAT/LSEEK/EXEC/FORK/WAIT/PIPE/DUP2/
STAT/LSTAT/GETCWD/CHDIR/UNLINK/MKDIR/RMDIR/RENAME/GETPID/GETDENTS/OPENAT/FSTATAT/UNLINKAT/FACCESSAT/
READLINK/FCNTL. **Working vendored, UNMODIFIED:** sbase true/false/echo/cat/wc/mkdir/rm/ls (+ `ls -l`)
AND **dash** (the operational shell).

## Dev loop (unchanged, plus one gotcha)

- `uk/run.sh` (colima aarch64 `gcc:13` container, `--cap-add=SYS_PTRACE`; rc=0 gates the suite).
  libc-program class is `-nostdinc -isystem $(cc -print-file-name=include) -Ilib -Ilib/include`;
  the vendored-sbase class adds `-Ivendor/sbase -w`.
- HW: the Pi came back online late in the session and **the ENTIRE session (M3e.5..M3i, incl. dash)
  was VALIDATED natively on the real RPi4** (Linux 6.12.47/aarch64, gcc 14.2) -- Pi-pending CLEARED.
  Deploy with `rsync -az --delete --exclude='/aios-uk' --exclude='/guest_*' --exclude='/prog_*'
  --exclude='/sbase-*' --exclude='/dash' --exclude='*.o' uk/ pi@raspberrypi.local:~/uk/` then
  `ssh pi@raspberrypi.local 'cd ~/uk && make clean && make -j4 all'`. aios-uk traces its own child,
  so NO sudo/cap is needed on the Pi. run.sh's docker wrapper does NOT apply on the Pi (it IS aarch64
  Linux) -- build + run directly.
- **HW gotchas (s20):** (1) the Pi's DHCP lease ROTATED -- it was no longer at `.8`; use the mDNS
  name **`raspberrypi.local`** (keyless SSH still works) and read the IP with `hostname -I`. (2) the
  rsync artifact-excludes MUST be anchored with a leading `/` (`/prog_*`, like .gitignore) -- an
  unanchored `prog_*` also excludes the `guest/prog_*.c` SOURCES and you ship stale sources. (3) **gcc
  14 (the Pi) makes implicit-function-declaration a hard ERROR** where gcc 13 (colima) only warned --
  it caught a real latent bug (tolower used before its definition; fixed `d4183af`). HW gcc is
  stricter; build there before declaring done.
- **GOTCHA (new): colima's virtiofs mount lags after a host-side edit.** The first `docker run`
  right after editing files on the Mac frequently fails with `make: No rule to make target 'aios-uk'`
  or `No such file or directory` (the container sees a stale/partial mount). Fix: `sync` + a tiny
  read probe in a throwaway container (`docker run ... sh -c 'grep -c <newthing> Makefile'`) before
  the real build, or just retry once. Not a code bug.
- A ptrace hang is SILENT -> in-container `timeout N` + `fprintf(stderr)` in pal_linux.c.

## Key technical notes (carry forward)

- **Host-agnostic kernel invariant holds:** kernel/aios_kernel.c still includes only AIOS-owned
  headers; every host primitive is behind pal.h. The `*at` family added a host-agnostic
  `PAL_AT_FDCWD` sentinel so the kernel never names the host's AT_FDCWD (pal_linux's `hostdir()`
  maps it).
- **struct stat <-> struct aios_stat MUST stay byte-identical.** Time fields are now `struct
  timespec`/`struct aios_timespec` (two 8-byte fields on LP64 -- identical layout); `st_rdev` reused
  the old 4-byte pad slot (which had 4 bytes of implicit tail pad, so st_size..st_ctim offsets are
  unchanged). The shadow `<sys/stat.h>` adds `#define st_atime st_atim.tv_sec` etc. for the POSIX
  scalar names.
- **libaios discipline:** it stays freestanding (defines its own FILE/DIR/struct tm/struct
  dirent/passwd/group privately) so it compiles under BOTH the PROG class (no shadow headers) and
  the LIBC class. Those private structs MUST match the shadow-header copies byte-for-byte (same rule
  as stat<->aios_stat).
- **Known degradations (honest, documented -- NOT hacks):** `umask` is tracked in libc but the
  kernel does not yet enforce a per-guest umask on create (the host's still applies; common modes
  come out right). `getpwuid`/`getgrgid` return NULL (no /etc/passwd parsing) -> ls prints numeric
  ids, which `ls -ln` does too. `time()` returns a fixed 0 "now" (no clock syscall) -> ls's recent-
  vs-old date *heuristic* is degraded, but the actual mtime *dates* are correct (gmtime+strftime on
  the file's mtime). `<regex.h>` is declarations-only (grep will need a real regex).
- **Vendoring rule:** sbase sources are never edited. When a util needs a libc feature we lack, add
  it to libaios (that is how every util here came up: bsearch, umask, the *at family, strndup, time,
  readlink, printf widths). The vendored build uses `-w` so upstream warnings do not clutter output;
  errors still surface, and libaios is linted in the prog_* builds.

## NEXT (dash operational; M4 boundary sound; M5 signals + interactive ^C done)

**M4 (fcf4135, v0.5.3):** the syscall-bypass boundary is enforced -- pal_guest_setret neutralizes
every trapped syscall (`set_syscall_nr(-1)`); the kernel kills any guest that emits a non-AIOS
syscall. Proof: `guest/guest_escape.c`. **M5 (f23faa4 + b22a6fe + 3583af0, v0.5.4):** real signal
DELIVERY -- the kernel runs a guest handler via a PAL register dance (SIGACTION/SIGRETURN/KILL/ISATTY,
ABI->34); dash `trap`/`kill` work AND **interactive dash + ^C** work (^C interrupts the prompt, dash
survives). A `do_read` single-read fix (POSIX: return what is available, do not loop to fill the
buffer -- a terminal blocked forever otherwise) made interactive mode function. Proofs: prog_signal.c,
test/ctrlc_pty.c (a HOST pty driver). All HW-validated on the RPi4 (kernel 6.12). Open directions:

1. **M4.2 -- fs/resource isolation** (the OTHER half of the boundary): restrict the guest's VIEW of
   the host. Today a serviced AIOS `open` -> `pal_host_open` opens ANY host path. Give the guest a
   mount namespace / sandbox root (or openat2 RESOLVE_IN_ROOT in the PAL) so it can only reach an
   AIOS root.
2. **More sbase utils** -- head/tail/cp/mv/sort (cp/mv reuse the *at + recurse already built), grep
   (needs a REAL regex -- shadow <regex.h> is declarations-only today). Each is a small add now.
3. Smaller gaps: a real `time()`/CLOCK syscall (ls date heuristic, dash timing), per-guest umask +
   per-process cwd. Full job control (dash is built JOBS=0; M5 ^C is line-interrupt, not job control --
   would need setpgid/tcsetpgrp + a termios layer for line editing). s15 zsh lessons [[project_zsh_pty]].
4. Then **sched_ext** (custom RPi kernel lacks CONFIG_SCHED_CLASS_EXT) · the **seL4/x86-64 replant
   seam** (`pal_sel4.c`). The seL4 stall + lead-#3 keyboard stay MOOTED by leaving the platform (seL4
   tree preserved on main/origin as record/fallback).

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue building AIOS as a **gVisor-style userspace kernel on Linux** (the 2026-06-24 pivot off
seL4/RPi4 -- Linux is the interim substrate, verified seL4-on-x86-64 is the destination; verification
is the soul; programs see only the AIOS ABI, the host sits behind a narrow PAL). READ FIRST: memory
[[project_pivot_linux_userspace_kernel]] + docs/HANDOVER_20260626_session20.md +
docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md + uk/README.md.

WORKING BRANCH = **`main`** (the 0.5.x userspace kernel; commit on main, Bryan pushes). The `uk/`
tree: a host-agnostic kernel (kernel/aios_kernel.c includes ONLY aios_abi.h + aios_version.h +
pal.h) over the ONLY host-aware file (pal/pal_linux.c, a multi-process PTRACE_SYSCALL driver) +
libaios (a C runtime on the AIOS ABI) + shadow standard headers (lib/include, used with -nostdinc).
DONE through **v0.5.4 -- OPERATIONAL + boundary ENFORCED + signals/^C**: M0..M3c, the FULL PROCESS
MODEL (M3d), the libc retarget (M3e), **vendored suckless sbase -- true/false/echo/cat/wc/mkdir/rm/ls
compile UNMODIFIED**
(M3f/g/h: the `*at` family for `rm -r`, readlink + a UTC time/strftime layer + a printf rewrite for
`ls -l`), and -- the milestone -- **vendored real dash (Debian Almquist Shell, BSD) compiles
UNMODIFIED and runs as a real shell** (M3i): builtins, arithmetic, &&/||, if/test, for/while,
pipelines, command substitution, `>` redirect, $?, -c/stdin/script modes, driving real sbase
echo|wc|cat through the process model. **30-syscall ABI** (added FCNTL). To get dash: setjmp/
sigsetjmp (aarch64 asm), a RECORD-ONLY signal layer (no async delivery yet), fcntl/execve/vfork +
~10 new shadow headers. **M4 -- the boundary is now ENFORCED (v0.5.3):** the PAL neutralizes every
trapped syscall (`set_syscall_nr(-1)`) so a guest-chosen syscall NEVER runs on the host, and the
kernel kills any guest that emits a non-AIOS (real Linux) syscall -- the trap model is SOUND (proof:
guest/guest_escape.c). **M5 -- real SIGNAL DELIVERY + interactive ^C (v0.5.4):** the kernel RUNS a
guest handler (was record-only) via a PAL register dance -- SIGACTION/SIGRETURN/KILL/ISATTY (ABI->34);
dash `trap`/`kill` work AND **interactive dash + ^C** work (^C interrupts the prompt, dash survives);
a do_read single-read fix made interactive mode function (proof: prog_signal.c, test/ctrlc_pty.c). Both
sbase + dash are vendored under uk/vendor/{sbase,dash} and are NEVER patched -- missing libc features
are added to libaios. Validated on colima AND **natively on the real RPi4** (Linux 6.12.47/aarch64,
gcc 14.2 -- Pi-pending CLEARED; gcc 14 caught + we fixed a tolower-before-decl bug colima's gcc 13
missed).

DEV LOOP: `uk/run.sh` (colima aarch64 container, --cap-add=SYS_PTRACE; rc=0 gates the suite). The
libc-program class is `-nostdinc -isystem $(cc -print-file-name=include) -Ilib -Ilib/include`; the
vendored-sbase class adds `-Ivendor/sbase -w`; dash adds `-Ivendor/dash/src -include
vendor/dash/config.h -DSHELL -DSMALL -DGLOB_BROKEN -Dalloca=__builtin_alloca -w` (the `dash` Makefile
target). HW (native, no docker): `rsync -az --delete --exclude='/aios-uk' --exclude='/guest_*'
--exclude='/prog_*' --exclude='/sbase-*' --exclude='/dash' --exclude='*.o' uk/ pi@raspberrypi.local:~/uk/`
then `ssh pi@raspberrypi.local 'cd ~/uk && make clean && make -j4 all'` (pi/aios; the Pi's DHCP lease
rotates -- use the mDNS name, not a fixed IP; ANCHOR the rsync excludes with `/` or you ship stale
guest/prog_*.c sources; gcc 14 there is stricter than colima's gcc 13 -- e.g. implicit-decl = error).
GOTCHA: colima's virtiofs mount lags after a host-side edit -- the first docker build can fail with
"No rule to make target 'aios-uk'" / "No such file"; `sync` + a read-probe in a throwaway container
(or just retry once) before the real build. A ptrace hang is SILENT -> in-container `timeout N` +
fprintf(stderr).

PRIMARY TASK -> pick the next milestone (dash operational; M4 boundary sound; M5 signals + interactive
^C done): (1) **M4.2 fs/resource isolation** -- the OTHER half of the boundary: restrict the guest's
VIEW of the host (mount namespace / sandbox root / openat2 RESOLVE_IN_ROOT in the PAL) so a serviced
open() can only reach an AIOS root, not arbitrary host paths (today pal_host_open opens ANY host path).
(2) **more sbase utils** -- head/tail/cp/mv/sort (cp/mv reuse the *at + recurse); grep needs a REAL
regex (<regex.h> is decls-only). (3) a real `time()`/CLOCK syscall, per-guest umask/cwd; full job
control (dash built JOBS=0; the M5 ^C is line-interrupt, not job control -- would need setpgid/tcsetpgrp
+ a termios layer). Keep kernel/aios_kernel.c host-agnostic + the PAL seam minimal (the future verified
boundary); NEVER patch vendored sources -- grow libaios. GOTCHA: NO apostrophes in run.sh (the docker
`sh -c '...'` body) [[feedback_script_style]]. Commit per milestone on `main`; validate colima + the Pi
(`raspberrypi.local`, gcc 14 is stricter); Bryan pushes.

THEN: sched_ext (custom RPi kernel w/ CONFIG_SCHED_CLASS_EXT); pal_sel4.c (the replant seam).
The seL4 stall + lead-#3 keyboard are MOOTED by leaving the platform (seL4 tree preserved on
main/origin as record/fallback).
