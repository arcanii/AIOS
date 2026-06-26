# HANDOVER -- session 20 (2026-06-26): real sbase runs UNMODIFIED on the AIOS kernel

Continues the userspace-kernel work (the 2026-06-24 pivot: AIOS as a gVisor-style userspace kernel
on Linux; verified seL4-on-x86-64 is the destination; verification is the soul). This session
finished the libc gaps sbase needed and then **vendored real suckless sbase and compiled 8 of its
utilities UNMODIFIED** against AIOS's libc -- the headline proof of the retarget. All on `main`
(0.5.x). Live state: memory [[project_pivot_linux_userspace_kernel]]. Design:
docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md. README: uk/README.md.

## What shipped this session (all on `main`, 7 milestone commits + a version bump)

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

**AIOS ABI now (29 syscalls):** WRITE/READ/OPEN/CLOSE/EXIT/MMAP/FSTAT/LSEEK/EXEC/FORK/WAIT/PIPE/DUP2/
STAT/LSTAT/GETCWD/CHDIR/UNLINK/MKDIR/RMDIR/RENAME/GETPID/GETDENTS/OPENAT/FSTATAT/UNLINKAT/FACCESSAT/
READLINK. **Working vendored sbase utilities:** true, false, echo, cat, wc, mkdir, rm, ls (+ `ls -l`).

## Dev loop (unchanged, plus one gotcha)

- `uk/run.sh` (colima aarch64 `gcc:13` container, `--cap-add=SYS_PTRACE`; rc=0 gates the suite).
  libc-program class is `-nostdinc -isystem $(cc -print-file-name=include) -Ilib -Ilib/include`;
  the vendored-sbase class adds `-Ivendor/sbase -w`.
- HW: `scp -r uk pi@192.168.0.8:~/ && ssh pi@192.168.0.8 'cd ~/uk && make && ./aios-uk <prog>'`
  (pi/aios, Linux 6.12). **The Pi was still OFFLINE this session** -- everything is colima-verified,
  Pi-pending (re-confirm when it is back; these are pure path/metadata/userspace ops, so colima is a
  strong signal).
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

## NEXT -> dash (= fully operational), then enforce the boundary

1. **dash** -- vendor (suckless/MIT, like sbase) + compile unmodified, replacing `prog_sh`. This is
   the big one: dash leans hard on **signals** (`signal`/`sigaction`/`kill`/`SIGINT`/`SIGCHLD`) and
   **job control** (process groups, `tcsetpgrp`, `setpgid`, waitpid status macros incl. WIFSIGNALED)
   -- none of which the ABI/kernel model yet. Expect a real signals milestone first (an AIOS signal
   delivery path through the PAL/ptrace), then dash. `getcwd`/`getpwnam`/`fnmatch`/`glob` may also
   surface. Lessons from s15 (zsh job-control gap, [[project_zsh_pty]]) are relevant background.
2. Likely smaller libc gaps dash/more-sbase want along the way: `sysconf`, `fnmatch`/`glob`,
   `setjmp`/`longjmp` (dash uses them), `realpath`, a real `time()` (a CLOCK syscall), more sbase
   utils (head/tail/cp/mv/sort -- cp/mv reuse the recurse + the *at family already built).
3. Then **M4** enforce the boundary (seccomp/namespaces so a guest CANNOT bypass the kernel) ·
   **M5** sched_ext (custom RPi kernel lacks CONFIG_SCHED_CLASS_EXT) · **M6** `pal_sel4.c` (the
   replant seam). The seL4 stall + lead-#3 keyboard stay MOOTED by leaving the platform (seL4 tree
   preserved on main/origin as record/fallback).

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
DONE through **v0.5.1**: M0..M3c, the FULL PROCESS MODEL (M3d), the libc retarget (M3e: shadow
headers, FILE* stdio, errno, sys/stat + path ops, getopt/qsort/bsearch, directory streams), and --
the milestone -- **vendored suckless sbase whose true/false/echo/cat/wc/mkdir/rm/ls compile
UNMODIFIED** against AIOS's libc and run on the kernel (M3f/M3g/M3h: incl. the `*at` family for
`rm -r`, readlink + a real time/strftime layer + a printf rewrite for `ls -l`). **29-syscall ABI.**
sbase is vendored under uk/vendor/sbase and is NEVER patched -- missing libc features are added to
libaios. Validated on colima (the RPi4 has been offline since s19 -- Pi-pending, re-confirm when back).

DEV LOOP: `uk/run.sh` (colima aarch64 container, --cap-add=SYS_PTRACE; rc=0 gates the suite). The
libc-program class is `-nostdinc -isystem $(cc -print-file-name=include) -Ilib -Ilib/include`; the
vendored-sbase class adds `-Ivendor/sbase -w`. HW via `scp -r uk pi@192.168.0.8:~/ && ssh pi@... 'cd
~/uk && make && ./aios-uk <prog>'` (pi/aios). GOTCHA: colima's virtiofs mount lags after a host-side
edit -- the first docker build can fail with "No rule to make target 'aios-uk'"; `sync` + a read-probe
in a throwaway container (or just retry once) before the real build. A ptrace hang is SILENT ->
in-container `timeout N` + fprintf(stderr).

PRIMARY TASK -> **dash = fully operational** (vendor suckless dash, MIT/BSD; compile UNMODIFIED to
replace prog_sh). dash needs what the ABI/kernel do not model yet: **signals** (signal/sigaction/
kill, SIGINT/SIGCHLD) and **job control** (setpgid/tcsetpgrp/process groups, WIFSIGNALED-style wait
status). Expect to build a real AIOS **signals** milestone FIRST (delivery through the PAL/ptrace),
then dash; smaller gaps (sysconf, fnmatch/glob, setjmp/longjmp, realpath, a real time()/CLOCK
syscall) will surface. Keep kernel/aios_kernel.c host-agnostic + the PAL seam minimal (the future
verified boundary); never patch vendored sources -- grow libaios. Commit per milestone on `main`;
validate colima (+ Pi when reachable); Bryan pushes.

THEN: M4 enforce the boundary (seccomp/namespaces); M5 sched_ext; M6 pal_sel4.c (the replant seam).
The seL4 stall + lead-#3 keyboard are MOOTED by leaving the platform (seL4 tree preserved on
main/origin as record/fallback).
