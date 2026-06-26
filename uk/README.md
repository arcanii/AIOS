# AIOS userspace kernel (`uk/`) — v0.5.x

**0.5.x is a new design line.** It departs from the 0.4.x seL4/RPi4 bare-metal line (preserved on
`main` as the record/fallback) and gets its own major.minor; the running version lives in
`include/aios_version.h` and is printed in the kernel banner. 0.5.0 covers M0..M3e.

The pivot architecture (see `docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md`): AIOS becomes
a **gVisor-style userspace kernel** that runs on a commodity host kernel. AIOS programs compile for
the **AIOS ABI** and never see the host; their syscalls are *trapped* and serviced by the AIOS
kernel. The host sits behind a narrow **PAL** — Linux today (drivers, no stall), verified seL4
(x86-64) later. Programs only ever see AIOS's ABI, so swapping the host underneath is invisible.

## Layout (the seam is the point)

```
include/aios_abi.h   the AIOS ABI -- what programs see (host-agnostic)
include/pal.h        the PAL -- the ONLY host surface the kernel uses (the future verified seam)
kernel/aios_kernel.c the AIOS userspace kernel -- host-agnostic core (includes only the two above)
pal/pal_linux.c      PAL Linux backend -- the ONLY file that knows about Linux (ptrace SYSEMU)
guest/guest_hello.c  a freestanding AIOS-ABI program (raw svc, AIOS syscall numbers, no libc)
```

`kernel/aios_kernel.c` is meant to compile unchanged against a future `pal/pal_sel4.c`. Keep
`pal.h` minimal — every primitive added there is future proof obligation.

## M1 — first light

Proves the whole interception foundation: an AIOS-ABI binary runs, and its `WRITE`/`EXIT` syscalls
are trapped (`PTRACE_SYSEMU`, so Linux never executes them) and serviced by the AIOS kernel, which
reaches the host only through the PAL host gateway.

### Build + run

The Mac host is darwin and cannot ptrace Linux, so build/run in colima's aarch64 Linux VM:

```sh
colima start --arch aarch64        # once; boots the Linux VM
uk/run.sh                          # builds + runs first-light in an aarch64 container
```

Expected output:

```
[aios-uk] AIOS userspace kernel -- M1 first light (Linux/ptrace PAL)
[aios-uk] launching guest: ./guest_hello
hello from an AIOS-ABI program -- serviced by the AIOS userspace kernel, not Linux
[aios-uk] guest exited via AIOS ABI, code=42
guest exit status: 42
```

`aarch64` only for now (matches the RPi4 target and the colima VM). Needs `CAP_SYS_PTRACE`.

## M2 — a VFS behind the ABI ✅

The kernel now owns an fd namespace and services real file I/O. `OPEN`/`READ`/`CLOSE` join
`WRITE`/`EXIT`; the kernel keeps a fd table (AIOS fd → opaque `pal_file_t` backing object) and
reaches storage only through the PAL. `guest/guest_fileio.c` creates a file, writes it, reads it
back, and echoes it — `uk/run.sh` then shows the **real host file** the AIOS program produced
(`/tmp/aios_m2.txt`). Verified in colima and natively on the RPi4.

## M3 — toward operational ✅ (the process model is complete)

Real C programs run on the AIOS ABI, and the kernel is now multi-process:

- **M3a** loader + argv (a real `cat`). **M3b** `libaios`, a minimal C runtime on the ABI. **M3c**
  `wc`/`tail`/`bigalloc`: stdin pipes, mmap-backed `malloc` (in-place syscall injection), `fstat`/
  `lseek`.
- **M3d — the process model.** `exec` (execve injection), `fork`/`wait`/`exit` (the kernel went
  multi-process: a process table, a `waitpid(-1)` event loop over all guests, per-process fd tables
  over a refcounted open-file table), and `pipe`/`dup2` (non-blocking pipe ends + park/wake so the
  single-threaded kernel never wedges). Capstone: **`prog_sh`**, a shell that runs real pipelines —
  `./prog_args one two | ./prog_wc | ./prog_wc` works.

**AIOS ABI today:** WRITE/READ/OPEN/CLOSE/EXIT/MMAP/FSTAT/LSEEK/EXEC/FORK/WAIT/PIPE/DUP2.

## M3e — the libc retarget ✅

Ordinary C compiles `-nostdinc` against AIOS's **shadow standard headers** (`lib/include`:
string/ctype/stdlib/unistd/fcntl/stdio/errno/time/dirent/sys/…) so it picks up AIOS's libc
(implemented by `libaios` on the ABI) instead of the host's:

- **M3e.1** shadow headers + a standard-named POSIX surface · **M3e.2** FILE\* buffered stdio ·
  **M3e.3** `errno` (a real negated-`-errno` path + `strerror`/`perror`) · **M3e.4** `sys/stat.h` +
  path ops (stat/lstat/getcwd/chdir/unlink/mkdir/rmdir/rename/getpid) · **M3e.5** `getopt` + `qsort`
  (+`bsearch`) · **M3e.6** **directory streams** (`opendir`/`readdir`/`closedir` over a getdents-
  backed `AIOS_SYS_GETDENTS` + `struct aios_dirent`).

Proofs: `prog_libc/stdio/errno/fs/getopt/dir.c` (real C, shadow headers only).

## M3f / M3g / M3h — real sbase runs UNMODIFIED ✅ (the milestone)

The headline proof of the retarget: genuine third-party POSIX-utility source (**suckless sbase**,
vendored unmodified under `vendor/sbase` — see `vendor/README.md`) compiles UNMODIFIED against
`libaios` and runs on the AIOS kernel. sbase is never patched; missing libc features are added to
`libaios`. Working utilities: **true / false / echo / cat / wc / mkdir / rm / ls** (incl. `ls -l`)
**/ head / tail / cp / mv / ln / chmod / sort / grep** (`cp -p` preserves mode + times; `ln`/`ln -s`
make hard/symlinks — backed by the file-metadata syscalls below; `sort` does lexical/`-u`/`-n`/`-r`,
the numeric compare using a real `strtod`; `grep` is `-EFHcilnvwx`, backed by a real regex engine —
see M3j below).

- **M3f** vendored sbase; `true/false/echo/cat` (then `wc` + the libutf rune layer, then `mkdir` +
  `umask`/`parsemode`). **M3g** the **`*at` family** (`openat`/`fstatat`/`unlinkat`/`faccessat` +
  `fdopendir`/`dirfd`) unlocks sbase's `recurse` → `rm -r` over real directory trees. **M3h** `ls` +
  `ls -l`: `readlink`, a real (UTC) `gmtime`/`localtime`/`strftime` time layer, uid/gid → **names**
  via the passwd/group DB below, and a printf REWRITE with flags/width/precision so the columns align.

## M3i — dash: AIOS is operational ✅

The destination of the libc retarget. Real **dash** (the Debian Almquist Shell, 0.5.11, BSD —
Debian/Ubuntu's `/bin/sh`) compiles UNMODIFIED against `libaios` and runs on the AIOS kernel as a
real shell: builtins, **arithmetic** with precedence, `&&`/`||` control flow, `if`/`test`,
`for`/`while` loops, variable expansion, external **exec**, multi-stage **pipelines**, **command
substitution**, `>` **redirection**, `$?` status, `-c`/stdin/script-file modes. It drives real sbase
commands (`echo | wc | cat`) through the kernel's process model.

One new syscall (FCNTL — `F_DUPFD` for dash's redirection bookkeeping) plus the libc dash needs,
added to libaios (sbase + dash both stay unmodified): `setjmp`/`sigsetjmp` (aarch64 asm — dash's
exception mechanism), a signal layer (dispositions recorded, not yet delivered — enough for `-c`),
`fcntl`/`dup`/`execve`/`vfork`/`wait3`, `sysconf`/`strtoll`/`getrlimit`/`times`/`strsignal`, more
string fns, and the headers `<signal.h>`/`<setjmp.h>`/`<inttypes.h>`/`<alloca.h>`/`<getopt.h>`/
`<sys/{param,resource,time,times,ioctl}.h>`. Vendored under `vendor/dash` (config.h + the generated
sources are AIOS build inputs; see `vendor/README.md`).

**AIOS ABI now (30 syscalls):** WRITE/READ/OPEN/CLOSE/EXIT/MMAP/FSTAT/LSEEK/EXEC/FORK/WAIT/PIPE/DUP2/
STAT/LSTAT/GETCWD/CHDIR/UNLINK/MKDIR/RMDIR/RENAME/GETPID/GETDENTS/OPENAT/FSTATAT/UNLINKAT/FACCESSAT/
READLINK/FCNTL.

`uk/run.sh` runs the whole suite (colima); milestones through M3e.3 are also validated natively on
the RPi4 (M3e.4 onward is colima-verified, Pi-pending — the Pi went offline mid-s19).

## M3j — grep: a real regex engine ✅

The last major coreutil. Real **grep** compiles UNMODIFIED against `libaios` and runs `-EFHcilnvwx`
through the kernel — driven by a real **POSIX regex engine** added to `libaios`
(`regcomp`/`regexec`/`regfree`/`regerror`; the shadow `<regex.h>` was declarations-only). The
engine parses the pattern to an AST, compiles it to a Thompson **NFA program**, and matches by
**linear NFA simulation** (a Pike-style thread sweep) — so there is **no catastrophic backtracking**
and the matcher is **guaranteed to halt** (a per-step visited stamp dedups program counters, so even
`(a*)*` cannot spin). It supports literals, `.`, bracket expressions (ranges, `[^…]` negation, POSIX
`[:class:]`), `^`/`$` anchors, `\<`/`\>` word boundaries, grouping, `|` alternation, the `* + ?`
quantifiers and `{m,n}` intervals, and `REG_ICASE`, in both BRE and ERE (with the GNU-ish leniencies
grep relies on). It is a boolean matcher: grep compiles `REG_NOSUB` and never reads `pmatch`, and no
other vendored utility needs submatch capture yet. No new ABI — regex is pure `libaios`. The libc
gaps grep needed also landed: `fmemopen` (a read-mode in-memory stream for `-e`/`-f`/literal
patterns), `sprintf`, `strcasestr`, and a shadow `<strings.h>`.

Proofs: `guest/prog_regex.c` — a **75-case** `regcomp`/`regexec` battery (BRE/ERE, classes, anchors,
intervals, word boundaries, icase, the exact patterns `grep -w`/`-x` build, compile-error reporting,
and the catastrophic patterns that prove linearity) — wired into the run.sh gate; plus the live
`sbase grep` demos (`-E`/`-i`/`-v`/`-c`/`-n`/`-w`, an anchored BRE, and a `grep | wc -l` dash
pipeline). Validated on colima.

## M3k — a passwd/group database ✅

`getpwuid`/`getpwnam`/`getgrgid`/`getgrnam` now read **`/etc/passwd`** and **`/etc/group`** (they
returned `NULL` before), so **`ls -l` shows real user/group names** instead of numeric ids. Pure
`libaios` — no new ABI; the lookups use the existing file I/O and return a pointer to static storage
(POSIX). A missing or unreadable file still yields `NULL` → the numeric fallback, so a confined guest
whose root has no `/etc/passwd` is unaffected. Proof: `guest/prog_pwgrp.c` (uid 0 → `root`, name
round-trips, an unassigned uid → `NULL`) in the gate, plus `ls -l` resolving the owner name.
Validated on colima and the RPi4 (`ls -l` → `pi pi` for a uid-1000 file).

## M4 — the boundary is enforced ✅

The gVisor trap model is now **sound**: an AIOS program *cannot* reach the host behind the kernel's
back. Two layers, both in the existing ptrace PAL (no new ABI):

- **Mechanism** — `pal_guest_return` neutralizes the trapped syscall number (sets it to `-1` so the
  host kernel *skips* it) before running it to its exit. The guest's chosen syscall — an AIOS number
  *or* a smuggled real-Linux number — never executes on the host; the kernel plants the result. Only
  the kernel's own deliberate injections (mmap/exec/fork/exit) ever run a real host syscall.
- **Policy** — the kernel treats any trapped syscall number below the AIOS range (i.e. a real Linux
  syscall the guest emitted) as an escape attempt and **kills the guest** with a diagnostic.

`guest/guest_escape.c` is the red-team proof: it issues a raw Linux `write(64)` between two AIOS
writes. Before M4 the Linux write *executed on the host*; now it never runs and the guest is killed
(exit 159). HW-validated on the RPi4 (kernel 6.12). The complementary half — restricting the guest's
*view* of host resources (its fs namespace, so a serviced `open` can only reach an AIOS root) — is
the next hardening step.

## M5 — real signal delivery + interactive ^C ✅

The kernel now actually **runs a guest's signal handler** (dispositions are no longer just recorded).
Three new ABI syscalls — `SIGACTION`, `SIGRETURN`, `KILL` — plus `ISATTY` (ABI now 34). Delivery is a
register dance kept in the PAL: at a syscall exit (synchronous, e.g. `raise`) or a signal-stop
(asynchronous, e.g. terminal `^C`), the kernel saves the guest's regs, jumps it to the handler with
`x0 = signum` and `lr =` a libaios trampoline, and on `SIGRETURN` restores the saved regs so the guest
resumes where it was. The kernel catches `^C` itself (no `SA_RESTART`, EINTR-safe waits) so a blocking
read interrupts and the guest gets its own `^C` via the process group.

Proofs: `prog_signal.c` (a handler runs on `raise`, `SIG_IGN` survives); dash `trap '...' USR1;
kill -USR1 $$` (the trap fires, the script continues); and `test/ctrlc_pty.c` — **interactive dash on
a pty: `^C` interrupts the prompt line and dash survives** (handler runs, line aborts, prompt
returns) then runs the next command. A `do_read` single-read fix (POSIX semantics: return what's
available, don't loop to fill the buffer) was what made interactive mode function. HW-validated on the
RPi4 (kernel 6.12).

**AIOS ABI now (45 syscalls):** … READLINK/FCNTL/SIGACTION/SIGRETURN/KILL/ISATTY/CLOCK_GETTIME/
FCHMODAT/FCHOWNAT/SYMLINKAT/LINKAT/UTIMENSAT/UMASK/SETPGID/GETPGID/TCSETPGRP/TCGETPGRP.

A real clock: `AIOS_SYS_CLOCK_GETTIME` reads the host clock through the PAL (`pal_host_clock_gettime`
→ `clock_gettime(2)`; `AIOS_CLOCK_REALTIME`/`MONOTONIC`), so `time()`/`clock_gettime()`/`gettimeofday()`
are live — `time()` no longer returns a fixed 0. `ls -l` dates now use the real recent-vs-old format,
and `prog_clock.c` confirms the wall clock matches the host and the monotonic clock advances.

A file-metadata layer: five confinement-aware `*at` syscalls (`FCHMODAT`/`FCHOWNAT`/`SYMLINKAT`/
`LINKAT`/`UTIMENSAT`) make `chmod`/`chown`/`symlink`/`link`/`utimes` real, so `cp -p` preserves
mode+times and `ln`/`chmod` run. The confinement-critical detail: `chmod`/`chown`/`utimensat` follow
the final symlink in the *host* namespace, so a confined guest's single-target ops first resolve the
path inside the root (`openat2` + `/proc/self/fd`) — a planted in-root symlink can't redirect a
metadata change to a host file (proven in `prog_jail.c`: `chmod` through an escaping symlink is denied
while the host file's mode is untouched).

A **per-process cwd**: the current directory moved from a single PAL-global to the kernel's process
table. The kernel pre-absolutes every guest path — including the exec path — against the *calling
process's* cwd, so a subshell's `cd` no longer leaks into its siblings or parent (inherited across
fork, preserved across exec). No new ABI; the PAL is now cwd-free (`pal_host_chdir` just verifies the
target is a directory, and `pal_guest_exec` takes a kernel-resolved absolute path it stages into the
guest). `prog_pcwd.c` proves a child's `chdir` leaves the parent's cwd untouched, and real dash shows
`cd /tmp; (cd /); pwd` → `/tmp`.

A **per-process umask** (`AIOS_SYS_UMASK`): a real file-creation mask the kernel tracks per process and
applies on `open(O_CREAT)`/`mkdir`, inherited across fork *and* preserved across exec (the host umask is
neutralized so this single mask governs created modes — it used to be a no-op tracker with the host's
mask applied). `prog_umask.c` shows `0666` under `umask 077` → `0600`, the mask inherited across fork;
real dash shows `umask 077` surviving a `dash`→`mkdir` exec (the new dir is `0700`).

## M4.2 — filesystem confinement (the other half of the boundary) ✅

M4 stopped a guest *bypassing* the kernel; M4.2 stops a guest — even going *through* the kernel —
from reaching host paths **outside an AIOS root**. When the PAL is launched with `AIOS_ROOT` set,
every guest file path is resolved **inside** that root with `openat2(RESOLVE_IN_ROOT)`: absolute
paths, `..` traversal, and symlinks (absolute *or* `..`) are all clamped to the root by the host
kernel. That is the standard **unprivileged** container path-safety primitive — no `chroot` /
`CAP_SYS_CHROOT`, so it runs as plain user `pi` on the Pi.

The shape of the win: confinement is **purely a PAL policy**. The AIOS kernel passes the same path
strings either way; the kernel and the **ABI are unchanged — zero new syscalls**. A future seL4 PAL
gets the same view from an fs cap rooted at the AIOS fs. Default (`AIOS_ROOT` unset) = the whole
host, behaviour unchanged (every prior milestone runs byte-identically).

It covers the **data** boundary — `open`/`stat`/`unlink`/`mkdir`/`rmdir`/`rename`/`chdir`/`getcwd`/
`readlink` + the `*at` family (the kernel-side `cwd` is now a confined logical path). Path ops that
have no single confined form (unlink/mkdir/rename/readlink) open the parent dir confined and act on a
single non-walking leaf.

`guest/prog_jail.c` is the red-team + positive proof (the M4.2 analogue of `guest_escape.c`): run
under `AIOS_ROOT`, it confirms in-root open/stat/create/readdir/chdir/name-ops work while every
escape vector — an absolute host path, a `..` traversal, an absolute symlink, a `..` symlink — is
denied. `run.sh` also shows the *same* real `sbase cat` reading a host secret unconfined, then being
denied that secret once confined, then reading an in-root file fine. Validated on colima and the
RPi4 (kernel 6.12).

### M4.3 — exec confinement

The remaining boundary gap: a guest-issued **exec** (`AIOS_SYS_EXEC`) is now resolved **inside** the
root too, so a guest can launch only binaries in its root. The PAL resolves the guest's exec target
with `openat2(RESOLVE_IN_ROOT)`, turns the resulting `O_PATH` handle into a canonical real host path
via `/proc/self/fd`, and execs *that* (the canonical path is fully resolved and provably under the
root). The **init** program the operator names on the `aios-uk` command line is the trusted entry and
is **exempt** — only a guest's *own* `exec()` is confined (like a kernel loading its init image from a
known place, then jailing everything it spawns). `guest/prog_execjail.c` proves it: in-root binaries
run, while out-of-root host paths (`/bin/sh`, `/etc/passwd`, …) are denied and a `..` escape is
clamped back into the root. Validated on colima and the RPi4.

## M7 — job control, increment 1: the process-group foundation ✅

The first step of full job control: the kernel now tracks **process groups** and a **controlling-
terminal foreground group**. Four new ABI syscalls — `SETPGID`, `GETPGID`, `TCSETPGRP`, `TCGETPGRP`
(ABI now 45) — plus `getpgrp()` = `getpgid(0)` and `killpg()` = `kill(-pgrp)`. Each process carries a
`pgid` (inherited across fork, preserved across exec; init is its own group leader), and `KILL` now
signals a **process group** when given a pid ≤ 0 (the group `-pid`, or the caller's group for 0) —
how `killpg` reaches a whole job. `tcsetpgrp`/`tcgetpgrp` track the terminal's foreground group (with
an `ENOTTY` guard on a non-terminal fd).

This increment lands the *state and the syscalls* only; it deliberately does **not** touch signal
delivery, so M5's interactive `^C` path is unchanged — dash stays `JOBS=0` and the regression check
(`test/ctrlc_pty.c`) still passes. Proof: `guest/prog_jobctl.c` (pgid inheritance across fork,
`setpgid(0,0)` making a new leader, `kill(-pgrp)`/`killpg` delivering to the group, and the
`tcsetpgrp`/`tcgetpgrp` wiring + `ENOTTY`) in the gate. Validated on colima and the RPi4.

## M7 — job control, increment 2: stop / continue ✅

The process-lifecycle half of job control: a process can now be **stopped** and **continued**, and a
parent learns of both through `wait`. No new ABI — it reuses `KILL`/`WAIT`. A `SIGSTOP`/`SIGTSTP`
(default action) moves the process to a new **`PS_STOPPED`** state — the kernel plants the syscall
result and then simply does *not* resume it (the kernel already owns when each guest runs, so
"stopped" is just "don't resume until `SIGCONT`"). `SIGCONT` resumes a stopped process immediately.
`wait(WUNTRACED)` reports a stopped child (status `(sig<<8)|0x7f`, so `WIFSTOPPED`/`WSTOPSIG` decode
it) and `wait(WCONTINUED)` reports a continued one (`0xffff` → `WIFCONTINUED`); `WNOHANG` polls.

Still **no terminal-signal routing** — stop/continue is driven by explicit `kill` here, so M5's `^C`
path is untouched and `ctrlc_pty` still passes. Proof: `guest/prog_stop.c` (`SIGSTOP` → `WIFSTOPPED`,
`SIGCONT` → `WIFCONTINUED`, then terminate + reap) in the gate. Validated on colima and the RPi4.

## Next (per the design doc)

**Job control, increment 3** — the interactive payoff: route terminal signals (`^C`/`^Z`) to *only*
the foreground process group (today every guest shares the kernel's process group, so M5's `^C` works
by the host pty broadcasting to it — that gets reworked: move guests off the kernel's host group and
have the kernel forward terminal signals to `g_fg_pgrp`), add `sigprocmask` for dash's
`sigblockall`/`sigclearmask`, then rebuild dash **`JOBS=1`** so `^Z`/`fg`/`bg` and "`^C` kills only the
foreground job" work interactively. Then **sched_ext** · the seL4/x86-64 replant seam (`pal_sel4.c`).
