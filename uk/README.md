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
**/ head / tail / cp / mv** (cp/mv copy content + structure; `-p` metadata is degraded and
symlinks/special files report `ENOSYS` — there are no mode/owner/time/symlink syscalls yet).

- **M3f** vendored sbase; `true/false/echo/cat` (then `wc` + the libutf rune layer, then `mkdir` +
  `umask`/`parsemode`). **M3g** the **`*at` family** (`openat`/`fstatat`/`unlinkat`/`faccessat` +
  `fdopendir`/`dirfd`) unlocks sbase's `recurse` → `rm -r` over real directory trees. **M3h** `ls` +
  `ls -l`: `readlink`, a real (UTC) `gmtime`/`localtime`/`strftime` time layer, numeric uid/gid (no
  passwd db yet), and a printf REWRITE with flags/width/precision so the columns align.

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

**AIOS ABI now (34 syscalls):** … READLINK/FCNTL/SIGACTION/SIGRETURN/KILL/ISATTY.

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

## Next (per the design doc)

A real `time()`/CLOCK syscall; per-process cwd/umask. `sort` (needs `strtod` + the full libutf rune
layer) and `grep` (needs a real `<regex.h>`); a real file-metadata layer (chmod/chown/symlink/utime
syscalls) so `cp -p` and `ln` work. Then **sched_ext** · the seL4/x86-64 replant seam (`pal_sel4.c`).
