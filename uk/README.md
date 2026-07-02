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

## Demo (`demo.sh`) — show AIOS running on the Pi (or colima)

`uk/demo.sh` is a short, narrated, timeout-guarded tour: dash + sbase running on the AIOS kernel, the
**boundary** (a raw-syscall escape is killed), **confinement** (the guest sees only the AIOS root; the
host fs is invisible), and the **portability proof** (the same `6*7` under both the ptrace and seccomp
backends). On the RPi4:

```sh
ssh pi@raspberrypi.local 'cd ~/uk && make all && sh demo.sh'
```

For a **live interactive** AIOS shell with full job control (`^C`/`^Z`/`fg`/`bg`), run a real terminal
into it (note the `-t`). Use the **confined** shell so bare command names resolve to the AIOS coreutils
(an AIOS shell can only run AIOS-ABI programs, never host binaries — `uname`/`date` etc. are real Linux
executables and the boundary will, correctly, kill them):

```sh
ssh -t pi@raspberrypi.local 'cd ~/uk && sh mkaiosroot.sh /tmp/r >/dev/null 2>&1 && \
    AIOS_ROOT=/tmp/r PATH=/bin ./aios-uk /tmp/r/bin/sh'
```

Then (these are the vendored sbase utils that were built, plus dash builtins):
```
ls -l ;  cat /etc/passwd ;  grep root /etc/passwd ;  wc -l /etc/passwd ;  sort ;  echo $((6*7))
cat            # reads stdin -> blocks; press ^Z to suspend ([1]+ Stopped), `fg` to resume, ^C to kill
cat /etc/hostname   # a HOST file, outside the root -> DENIED (the jail)
```

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
  `./prog_args one two | ./prog_wc | ./prog_wc` works. (A write to a pipe with no readers left raises
  **`SIGPIPE`** on the writer — default-terminating it, so `producer | head` dies quietly; a guest that
  ignores/catches `SIGPIPE` gets `EPIPE` instead. The *kernel* keeps ignoring host `SIGPIPE`, since it
  does the pipe writes on guests' behalf.)

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

**AIOS ABI now (48 syscalls):** … READLINK/FCNTL/SIGACTION/SIGRETURN/KILL/ISATTY/CLOCK_GETTIME/
FCHMODAT/FCHOWNAT/SYMLINKAT/LINKAT/UTIMENSAT/UMASK/SETPGID/GETPGID/TCSETPGRP/TCGETPGRP/SIGPROCMASK/
TCGETATTR/TCSETATTR.

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

## M7 — job control, increment 3 (part 1): a real `sigprocmask` ✅

A real per-process **signal mask** — the masking dash `JOBS=1` needs for its `sigblockall`/
`sigclearmask` critical sections. One new ABI syscall (`SIGPROCMASK`, ABI now 46) over a `proc_t`
bitmask of blocked signals (inherited across fork). A signal raised while blocked stays **pending**
(the kernel's delivery path leaves it) and is delivered the moment it's unblocked; `SIGKILL`/`SIGSTOP`
are never blockable. (The pending slot is single — one masked signal at a time, a documented
simplification.) Proof: `guest/prog_sigmask.c` (block `SIGUSR1` → raise → handler does *not* run;
unblock → it's delivered) in the gate. Validated on colima and the RPi4. M5's `^C` is still untouched.

## M7 — job control, increment 3 (part 2): terminal-signal routing ✅

The interactive payoff. Before this, M5's `^C` worked by the **host pty broadcasting** `SIGINT` to the
kernel's whole process group (the kernel + every guest). Now the guests are moved **off** the kernel's
host process group (`setpgid` in the spawn child), so the host pty delivers `^C`/`^Z` only to the
**kernel** — which catches `SIGINT`/`SIGTSTP` (a `SIGTSTP` handler is also what stops the *kernel*
being suspended by `^Z`), and the PAL surfaces a caught terminal signal as a new `pal_guest_next`
event. The kernel then forwards it to **only the foreground process group** (`g_fg_pgrp`).

The forwarding goes entirely through the kernel's **own** pending-signal path — never a host `kill`
of a tracee, which would hit a `ptrace` hazard (a guest stopped at a not-yet-serviced syscall queues
the signal, which the `setret`/run-to-exit machinery then eats). A RUNNING guest takes the signal at
its next syscall; a guest parked in a blocked syscall has that syscall return `EINTR` with the signal
delivered. The "special" syscalls (`read`/`write`/`wait`) bypass the normal return path, so they
gained an entry-time pending-signal check. No new ABI.

So **`^C` now kills the foreground job and the shell survives** — proven *interactively on a pty* by
`test/ctrlc_job_pty.c` (a foreground `./prog_loop`, `^C`, dash returns to its prompt), alongside the
existing `ctrlc_pty` (`^C` at the prompt); both are in the gate. Validated on colima and the RPi4.
dash is still `JOBS=0` (so the foreground group is everything); rebuilding dash `JOBS=1` for `^Z`/
`fg`/`bg` is the last part.

## M7 — job control, increment 3 (part 3): dash `JOBS=1` — full job control ✅

dash is rebuilt **`JOBS=1`**, so all the kernel job-control machinery is now driven by a real shell —
no new ABI. dash `setpgid`s each job into its own process group and `tcsetpgrp`s the foreground, so
**`^C` reaches only the foreground job** (not the shell), **`^Z` suspends it** (`SIGTSTP` → the
`PS_STOPPED` state, reported to dash via `WUNTRACED` → `[1]+ Stopped`), and **`fg`/`bg` resume it**.
Three small build-side pieces: a shadow `<termios.h>` (jobs.c includes it but calls no
line-discipline functions); the `fg`/`bg` builtins **regenerated** into `builtins.{def,c,h}` from
`builtins.def.in` with `JOBS=1` (dash's own `mkbuiltins` — they had been stripped for the `JOBS=0`
build); and `strsignal` extended so `SIGTSTP` reads "Stopped". Proof: `test/ctrlz_pty.c` (`^Z`
suspend → `fg` resume → `^C` kill) joins `ctrlc_pty` and `ctrlc_job_pty` in the gate. Validated on
colima and the RPi4. **The job-control arc (increments 1–3) is complete.**

## M8 — a termios line-discipline layer ✅

`tcgetattr`/`tcsetattr` (two new ABI syscalls, ABI now 48) proxy to the host terminal, so a program
can switch the tty to **raw mode** (`cfmakeraw` clears `ICANON`/`ECHO`/`ISIG`) for char-at-a-time,
unechoed input — the foundation for full-screen interactive apps and custom line editors. Because the
kernel already reads the pty on the guest's behalf, once the guest puts the *host* pty in raw mode the
kernel's reads simply start returning one keypress at a time. `struct aios_termios` + a full shadow
`<termios.h>` whose flag values match the host (so the PAL translation is a field copy; a future seL4
PAL would remap); `cfmakeraw` and the `cf*`-speed helpers are inline in the header. Proof:
`guest/prog_rawkey.c` driven by `test/rawkey_pty.c` — it sends a single byte with **no newline** and
sees `rawkey got: Z` unechoed (canonical mode would block waiting for Enter). Validated on colima and
the RPi4.

## M9 — a SECOND PAL backend: seccomp (the portability proof) ✅

The endgame's whole premise is that `kernel/aios_kernel.c` is host-agnostic — it runs unchanged over a
different host trap mechanism, with only the PAL swapped. M9 **proves it**: `make PAL=seccomp` builds
`aios-uk` over a **seccomp `SECCOMP_RET_TRACE`** trap mechanism instead of `PTRACE_SYSCALL`, and the
**entire 16-key gate passes a second time, byte-for-byte the same kernel**.

- **The split.** `pal/pal_linux.c` was refactored into a shared **`pal/pal_linux_common.c`** (the
  Linux host-driver + the ptrace *injectors*: mmap/exec/fork/exit + the signal-frame dance — all
  identical no matter how a syscall is trapped) and a thin **trap front-end**. The one knob the
  injectors need is `PAL_RESUME(pid)` = "resume to the next trap" — `PTRACE_SYSCALL` (SYSEMU: stop at
  every syscall) for `pal_linux.c`, `PTRACE_CONT` (stop only at a seccomp-filtered syscall) for
  `pal_seccomp.c`. This *is* the structure `pal_sel4.c` will reuse: the trap mechanism varies, the
  host driver is shared.
- **Why injectors stay ptrace (honest).** Linux has **no** userspace-only way to inject
  memory/processes or rewrite another process's registers, so the five injectors are necessarily
  ptrace either way — a property of the host, not a leak in the seam. seccomp only changes how an
  *emulated* syscall is intercepted; the injectors run at the seccomp-event stop (after the filter),
  where rewriting the syscall number dispatches the real host syscall without re-filtering.
- **The GATEWAY (the load-bearing discovery).** AIOS numbers its syscalls `≥ 0x1000` so a real Linux
  syscall is unambiguously an escape. But **seccomp on arm64 does not deliver a trap for out-of-range
  syscall numbers** — they short-circuit to `ENOSYS` *without running the filter* (proven by
  `test/seccomp_probe.c`). ptrace traps the `svc` *instruction* and so sees them; seccomp filters the
  syscall-table *dispatch* and does not. So AIOS guests now trap through an in-range real **gateway**
  syscall (`AIOS_GATEWAY` = `gettid`/178) in `x8`, carrying the real AIOS number in `x9`; the PAL
  decodes `x8 == AIOS_GATEWAY ? x9 : escape`. The gateway is wired into libaios's three syscall stubs
  + the sigreturn trampoline and the four freestanding guests; it costs no kernel config and is
  neutralized (`x8 = -1`) so `gettid` never actually runs. `guest_escape` keeps a **raw** `svc`
  (`x8` = a real number, not the gateway) as the real escape vector — still killed under both backends.
- **Proof.** `uk/run.sh` runs the whole gate **twice** — `RESULT: linux=0 seccomp=0`. Both kill the
  raw-syscall escape (exit 159); both pass pipebig/jail/execjail/clock/…/`^C`/`^Z`/raw-mode. The Makefile
  gained `PAL ?= linux` (default = the proven ptrace backend; zero regression) + a `.pal.stamp` so
  switching `PAL=` actually rebuilds. The kernel banner still reads "ptrace PAL" (the kernel is
  byte-identical and names its default backend — a cosmetic). See
  `docs/AIOS_KERNEL_DEPENDENCIES.md` for the full host-feature manifest this exercise formalized.

## Networking — sockets behind the boundary ✅

AIOS speaks TCP. Networking is **host-passthrough behind the AIOS boundary**, exactly like the VFS: a
socket is an ordinary AIOS fd backed by a real host socket, so `read`/`write`/`close` work on it for
free. The AIOS domain/type/protocol values, the `sockaddr` layout, and the `SOL_SOCKET`/`SO_*` option
values all match the host, so the PAL forwards the address/option bytes straight through (a future seL4
PAL remaps them and talks to a network server).

- **Increment 1 — a TCP client.** `SOCKET`/`CONNECT` (ABI 55→57) + `htons`/`inet_addr` and the shadow
  `<sys/socket.h>`/`<netinet/in.h>`/`<arpa/inet.h>`. `guest/prog_net.c` connects out and round-trips a
  message; it has fetched `http://example.com` over the real internet from the Pi.
- **Increment 2 — a TCP server.** `BIND`/`LISTEN`/`ACCEPT` + `SETSOCKOPT`/`GETSOCKNAME` (ABI 57→62), so
  an AIOS program can **listen**. `ACCEPT` mirrors `SOCKET` — it returns a **new AIOS fd** backed by the
  accepted host socket. `getsockname` lets a server bind an ephemeral port (`:0`) and learn which one it
  got. `guest/prog_netserver.c` is a real AIOS echo server: it sets `SO_REUSEADDR`, binds
  `127.0.0.1:0`, announces its port, accepts a connection, and echoes — and a host client
  (`test/net_server.c`) connects to it and round-trips a message. Wired into the gate (`netsrv`), green
  under **both** PAL backends.

**Honest limits (the next increments):** socket I/O is **blocking** — a serviced `read`/`connect`/
`accept` blocks the single-threaded kernel (fine for one guest; non-blocking + park/wake, integrating
socket readiness into the kernel's event loop like the pipe park/wake, is next). IPv4 only, no DNS
resolver yet (connect takes a dotted quad). And there is **no network-access confinement** yet — which
hosts/ports a guest may reach, the analogue of the `AIOS_ROOT` filesystem confinement, is a later PAL
policy step.

## Building an AIOS root image (the "disk image")

AIOS is a userspace kernel — it runs as a process on the host Linux — so there is no bootable AIOS
*kernel* image (Linux boots; `aios-uk` runs on top). The meaningful analog is an **AIOS root
filesystem**: the self-contained userland (the AIOS-ABI shell + coreutils + config) that the AIOS
kernel *serves and confines*. Launched with `AIOS_ROOT` set, every guest path resolves inside that
tree (`openat2 RESOLVE_IN_ROOT`) and a guest can exec only binaries inside it — so the shell and
utilities see only the image, never the host.

`mkaiosroot.sh` builds one from the compiled binaries: `dash` as `/bin/sh`, the sbase utilities at
their standard names in `/bin`, and a `/etc/passwd`+`/etc/group`. It also writes `aiosroot.tar` (a
shippable form; an ext4 image works too — the host mounts it and points `AIOS_ROOT` at the
mountpoint, since AIOS confines via a directory fd while the host owns the actual filesystem). Run it
as a confined AIOS system:

```sh
make all && sh mkaiosroot.sh ./aiosroot
AIOS_ROOT="$PWD/aiosroot" PATH=/bin ./aios-uk "$PWD/aiosroot/bin/sh"
```

The shell, `ls -l /bin` (names from the image's own `/etc/passwd`), pipelines, and `grep` all run
entirely in-image; the host filesystem (`/etc/hostname`, …) is unreachable. **Note:** the *init*
binary is the trusted entry and is loaded by its **host** path (so name the image's shell by its real
path); everything the shell resolves after that is confined to `AIOS_ROOT`. Verified on colima.

## The minimal AIOS appliance (`appliance/`)

Linux is the *substrate*, AIOS is the kernel on top — so the deliverable "Linux that exists only to
host AIOS" is a **minimal kernel + a three-file initramfs**. `appliance/build_appliance.sh` builds a
minimal Linux (default 6.18) for QEMU `virt` aarch64 + an initramfs holding only `/init` (a tiny
static PID-1 launcher, `aios_init.c`), `/aios-uk` (the AIOS kernel, static), and `/aiosroot` (the AIOS
userland from `mkaiosroot.sh`); `appliance/run_qemu.sh` boots it (TCG, no KVM) straight into a confined
AIOS shell. The exact kernel features AIOS needs — and nothing more — are in
`docs/AIOS_KERNEL_DEPENDENCIES.md` + `appliance/aios.config`; that manifest *is* the eventual seL4
PAL's proof obligation, stated precisely.

## Next (per the design doc)

**sched_ext** — AIOS authors its own scheduling policy as a `sched_ext` BPF program (the 6.18 appliance
can carry `CONFIG_SCHED_CLASS_EXT`; `appliance/aios.config` keeps it off for the strict-minimal base).
Then the endgame: the **seL4/x86-64 replant seam** (`pal_sel4.c`) — a third PAL backend that proves
`kernel/aios_kernel.c` runs unchanged on a verified base. M9 (the seccomp second backend) is the dress
rehearsal: the trap-mechanism/host-driver split + `PAL_RESUME` seam are exactly what `pal_sel4.c` reuses.
