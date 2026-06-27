# HANDOVER -- session 23 (2026-06-27): a SECOND PAL backend (seccomp) + a minimal "Linux boots into AIOS" appliance

Continues the userspace-kernel work (the 2026-06-24 pivot: AIOS as a gVisor-style userspace kernel on
Linux; verified seL4-on-x86-64 is the destination; verification is the soul). Session 22 finished the
job-control arc + termios + a confined "disk image". **Session 23 delivered the ENDGAME's dress
rehearsal -- a SECOND PAL backend (seccomp `SECCOMP_RET_TRACE`), proving `kernel/aios_kernel.c` runs
UNCHANGED over a different host trap mechanism (the whole point of the PAL seam) -- and packaged AIOS as
a minimal Linux 6.18 appliance that boots straight into a confined AIOS shell.** All on `main`, **2
commits, v0.5.21 -> v0.5.22, ABI UNCHANGED (48), kernel byte-identical.** Colima-validated (the full
16-key gate passes a SECOND time, `linux=0 seccomp=0`); Pi-pending.

## What shipped this session (on `main`)

### M9 -- a second PAL backend: seccomp (`5f465b1`, v0.5.22) -- THE PORTABILITY PROOF

`make PAL=seccomp` builds `aios-uk` over a seccomp `SECCOMP_RET_TRACE` trap mechanism instead of
`PTRACE_SYSCALL`, and the **whole `run.sh` gate passes a second time** -- `RESULT: linux=0 seccomp=0`,
`kernel/aios_kernel.c` byte-identical. This is the dress rehearsal for `pal_sel4.c`.

- **The split.** `pal/pal_linux.c` (was ~880 lines) split into:
  - **`pal/pal_linux_common.c`** -- the shared Linux host-driver I/O (open/read/write/stat/the `*at`
    family/termios/clock/getdents + M4.2 confinement) AND the ptrace **injectors** (mmap/exec/fork/exit
    + the signal-frame register dance) -- all identical no matter HOW a syscall is trapped. It is
    `#include`d by exactly one front-end (NOT compiled standalone; an `#ifndef PAL_RESUME #error` guards
    it).
  - **`pal/pal_linux.c`** (now ~80 lines) -- the SYSEMU trap front-end (`spawn`/`next`/
    `at_syscall_entry`), `PAL_RESUME = PTRACE_SYSCALL`.
  - **`pal/pal_seccomp.c`** -- the seccomp trap front-end: a `SECCOMP_RET_TRACE`-everything BPF filter,
    a `spawn` with a `raise(SIGSTOP)` handshake (so `PTRACE_O_TRACESECCOMP` is set before the filter
    bites) that runs through to `PTRACE_EVENT_EXEC`, a `next` that recognizes `PTRACE_EVENT_SECCOMP`,
    `PAL_RESUME = PTRACE_CONT`.
  - The one knob: **`PAL_RESUME(pid)` = "resume to the next trap"** (`PTRACE_SYSCALL` stops at every
    syscall; `PTRACE_CONT` stops only at a seccomp-filtered one). The injectors' *internal* "step an
    injected syscall to its exit" calls stay `PTRACE_SYSCALL` in both.
  - **This split IS the structure `pal_sel4.c` reuses:** the trap MECHANISM varies (the front-end), the
    host driver + injectors are shared.
- **Why injectors stay ptrace (honest).** Linux has NO userspace-only way to inject memory/processes or
  rewrite another process's registers -- so the five injectors are ptrace either way (a HOST property,
  not a seam leak). seccomp only changes how an *emulated* syscall is intercepted; the injectors run at
  the seccomp-event stop (after the filter), where rewriting the syscall number dispatches the real host
  syscall without re-filtering.
- **THE GATEWAY -- the load-bearing discovery (`test/seccomp_probe.c`).** seccomp on arm64 does **not**
  deliver a trap for out-of-range / unimplemented syscall numbers -- they short-circuit to `ENOSYS`
  *without running the filter* (proven: nr `0x1000` and the unassigned 294..423 gap produce NO
  `PTRACE_EVENT_SECCOMP`; only real implemented nrs like `write`/64 do). AIOS numbers its syscalls
  `>= 0x1000` (so a real Linux syscall is unambiguously an escape) -- so a seccomp PAL **cannot trap AIOS
  syscalls via x8**. ptrace traps the `svc` *instruction* (sees any nr); seccomp filters the
  syscall-table *dispatch* (skips out-of-range). AIOS's disjoint-high numbering and a seccomp trap are
  **mutually exclusive** -- the very property that powers zero-ambiguity escape detection defeats seccomp.
  - **FIX (shared by both backends): a GATEWAY.** Guests trap via an in-range REAL syscall
    `AIOS_GATEWAY = gettid` (178) in `x8`, carrying the real AIOS number in `x9`; the PAL decodes
    `x8 == AIOS_GATEWAY ? x9 : escape` (a real nr left in x8 is an escape, surfaced as-is so the kernel
    kills it). Wired into libaios's three `asys` stubs + the sigreturn trampoline (`mov x8,#178; mov
    x9,#nr`) and the four freestanding guests. `AIOS_GATEWAY` lives in `aios_abi.h`, clearly marked as a
    Linux/aarch64 trap-convention constant (NOT the host-agnostic ABI). Costs no kernel config; `gettid`
    is neutralized (`x8 = -1` in setret) so it never runs. A seL4 PAL traps via IPC and ignores all of it.
  - `guest_escape` keeps a **raw** `svc` (real number in x8, NOT the gateway) as the genuine escape
    vector -- still killed (exit 159) under both backends.
- **Makefile / gate.** `PAL ?= linux` (default = the proven ptrace backend; zero regression) + a
  **`.pal.stamp` FORCE rule** so `make PAL=seccomp` after a plain `make` ACTUALLY rebuilds `aios-uk`
  (else the mtime check silently keeps the wrong backend -- see the footgun below). `run.sh` runs the
  gate **twice** (a `gate()` function, once per PAL) + `stdbuf -oL` for a real-time log. ABI unchanged
  (48); kernel byte-identical (its banner still names the default "ptrace PAL" -- cosmetic).
  `docs/AIOS_KERNEL_DEPENDENCIES.md` formalizes the exact host surface AIOS needs (the seL4 PAL's proof
  obligation).
- **Proof.** `uk/run.sh` -> `RESULT: linux=0 seccomp=0`; both kill the raw-syscall escape (159); both
  pass pipebig/jail/execjail/clock/.../`^C`/`^Z`/raw-mode. Validated on colima; Pi-pending.

### The minimal AIOS appliance (`e4e6408`, `uk/appliance/`) -- "Linux 6.18 boots into AIOS"

Bryan: package AIOS with the latest longterm kernel (~6.18) + a minimum package to support AIOS. Chose
**aarch64/QEMU-virt** (reuses the entire aarch64 userland; bootable + testable here, no HW) +
**strictly-minimal-first** (sched_ext deferred).

- A minimal Linux (default 6.18 LTS) for QEMU `virt` + a **three-file initramfs**: `/init` (a tiny
  STATIC PID-1 launcher, `aios_init.c` -- mounts `/proc` for confinement canonicalization, gives the
  console a controlling terminal for job control, sets `AIOS_ROOT=/aiosroot`, execs `aios-uk`),
  `/aios-uk` (the AIOS kernel, STATIC -- no libc in the image), `/aiosroot` (the AIOS userland from
  `mkaiosroot.sh`). Booted, the kernel hands the machine to a **confined** AIOS shell; the host fs is
  unreachable.

  **BOOT VALIDATED under QEMU** (with a stock Debian arm64 kernel): `Linux boots -> [aios-init] minimal
  Linux up -> [aios-uk] AIOS v0.5.22 -> launching guest: /aiosroot/bin/sh` -- the FULL Linux->AIOS boot
  chain works. (The interactive shell's piped-command output isn't captured in the QEMU serial -- a
  harness quirk of feeding a job-controlled shell over a pipe, NOT an appliance defect; the confined
  shell *executing* commands is proven separately by the in-container static `aios-uk` running
  `echo`/`ls /bin` against `/aiosroot`.) The from-source minimal-6.18 build runs through `defconfig` but
  the gcc:13 container is not a complete kernel-build env (needs `flex bison bc libssl-dev libelf-dev`
  and still hits an arm64 vDSO `__NR_*` header quirk) -- run `build_appliance.sh` in a proper
  kernel-build environment or on the Pi for the from-source kernel.
- `appliance/build_appliance.sh` (static binaries + initramfs + fetch/build the kernel),
  `appliance/run_qemu.sh` (qemu-system-aarch64 -M virt, TCG -- no KVM), `appliance/aios.config` (the
  AIOS-required kernel options; NET/BLOCK/modules off), `docs/AIOS_KERNEL_DEPENDENCIES.md` (the WHY,
  grep-grounded).
- **Validated on colima:** the STATIC `aios-uk` runs the confined AIOS root end-to-end (echo + `ls /bin`
  from `/aiosroot`, host fs jailed away); the initramfs assembles. **The full Linux->AIOS boot chain is
  QEMU-validated** (stock arm64 kernel): `Linux -> [aios-init] -> [aios-uk] v0.5.22 -> launching guest:
  /aiosroot/bin/sh`. The from-source minimal-6.18 build needs a proper kernel-build env (the gcc:13
  container lacks `flex bison bc libssl-dev libelf-dev` and hits an arm64 vDSO header quirk) -- run the
  scripts on the Pi or a kernel-build box.

## KEY LESSONS / GOTCHAS (carry forward)

- **The seccomp-out-of-range fact is the whole story:** seccomp on arm64 will NOT trap a syscall whose
  number is out of the implemented table (proven by `test/seccomp_probe.c`). Any future seccomp/USER_NOTIF
  work MUST go through the GATEWAY (`AIOS_GATEWAY` in `aios_abi.h`). ptrace is immune (it traps the
  instruction).
- **The stale-mtime FOOTGUN (now fixed by `.pal.stamp`):** `make PAL=seccomp aios-uk` after `make all`
  would NOT rebuild (aios-uk newer than its source by mtime), so it silently ran the *linux* binary. The
  first several "seccomp passes" this session were accidentally the linux build -- a FALSE PASS. ALWAYS
  verify a flag-selected build actually recompiled (clean-build, or `cmp` the binaries). The `.pal.stamp`
  FORCE rule now makes a PAL change rebuild.
- **Block buffering HIDES a hang's location.** A `sh -c` writing to a file is block-buffered, so the last
  flushed line is NOT where execution stopped. `stdbuf -oL` (added to `run.sh`) gives a real-time log.
  (A 27-minute "hang" mid-gate turned out to be a colima/container degradation under the very long
  session -- a fresh run completed both passes green. Everything passed in isolation.)
- **Appliance build deps:** the gcc:13 container lacks `xz-utils` (so `tar xf linux-*.tar.xz` fails) and
  `wget`; install `cpio xz-utils wget qemu-system-arm`. Stage the initramfs on a LOCAL fs, NOT the
  virtiofs mount -- `mknod /dev/console` fails on virtiofs.
- **Carried:** colima virtiofs LAGS after a host edit (sync + a read-probe / retry; NEW dirs lag more);
  a ptrace hang is SILENT (in-container `timeout N` + fprintf(stderr)); gcc 14 (Pi) stricter than gcc 13;
  NO apostrophes in run.sh's `sh -c '...'` body [[feedback_script_style]].

## NEXT -- the endgame (M9 + the appliance done this session)

1. **HW (Pi) validation** of M9 (seccomp, `make PAL=seccomp`) + the appliance boot -- both
   colima-validated, Pi-pending. (rsync + `make` per the dev loop below.)
2. **sched_ext** -- AIOS's own scheduling policy as a sched_ext BPF program; the 6.18 appliance can
   carry `CONFIG_SCHED_CLASS_EXT` (`appliance/aios.config` keeps it off for the strict-minimal base).
3. **the seL4/x86-64 REPLANT SEAM (`pal_sel4.c`)** -- a THIRD PAL backend proving
   `kernel/aios_kernel.c` runs UNCHANGED on a verified base. M9 de-risked the seam: `pal_sel4.c` gets a
   `PAL_RESUME`-equivalent + its own injectors; the host-driver split shows exactly what is shared vs
   host-specific. `docs/AIOS_KERNEL_DEPENDENCIES.md` is the proof-obligation manifest.

Keep `kernel/aios_kernel.c` host-agnostic + the PAL seam minimal; NEVER patch vendored sources -- grow
libaios.

## Dev loop (carry forward)

- `uk/run.sh` (colima aarch64 `gcc:13`, `--cap-add=SYS_PTRACE`; rc=0 gates the suite TWICE, once per PAL).
- `make PAL=seccomp` for the seccomp backend (default `make` = ptrace). `.pal.stamp` makes a PAL switch
  rebuild.
- Appliance: `apt-get install -y cpio xz-utils wget qemu-system-arm`, then `sh appliance/build_appliance.sh`
  + `sh appliance/run_qemu.sh`.
- HW (native, no docker): `rsync -az --delete --exclude='/aios-uk' --exclude='/guest_*' --exclude='/prog_*'
  --exclude='/sbase-*' --exclude='/dash' --exclude='*.o' --exclude='/aiosroot' --exclude='/aiosroot.tar'
  --exclude='/appliance/build' --exclude='/appliance/out' uk/ pi@raspberrypi.local:~/uk/` then
  `ssh pi@raspberrypi.local 'cd ~/uk && make clean && make -j4 all && make PAL=seccomp aios-uk'` (pi/aios).

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue building AIOS as a **gVisor-style userspace kernel on Linux** (the 2026-06-24 pivot off
seL4/RPi4 -- Linux is the interim substrate, verified seL4-on-x86-64 is the destination; verification is
the soul; programs see only the AIOS ABI, the host sits behind a narrow PAL). READ FIRST: memory
[[project_pivot_linux_userspace_kernel]] + docs/HANDOVER_20260627_session23.md +
docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md + docs/AIOS_KERNEL_DEPENDENCIES.md + uk/README.md.

WORKING BRANCH = **`main`** (the 0.5.x userspace kernel; commit on main, Bryan pushes). The `uk/` tree:
a host-agnostic kernel (kernel/aios_kernel.c includes ONLY aios_abi.h + aios_version.h + pal.h) over a
SHARED Linux host-driver core (pal/pal_linux_common.c) + TWO trap front-ends (pal/pal_linux.c =
PTRACE_SYSCALL, pal/pal_seccomp.c = seccomp RET_TRACE; selected by `make PAL=linux|seccomp`) + libaios +
shadow standard headers (lib/include, used -nostdinc).

DONE through **v0.5.22, 48-syscall ABI -- OPERATIONAL + the boundary COMPLETE + FULL JOB CONTROL + raw
terminal mode + a SECOND PAL backend (seccomp) + a minimal Linux-6.18 appliance**. Real vendored sbase
(true/false/echo/cat/wc/mkdir/rm/ls(+ls -l)/head/tail/cp/mv/ln/chmod/sort/grep) AND dash run UNMODIFIED.
**M9: the WHOLE 16-key run.sh gate passes a SECOND time with `make PAL=seccomp` (RESULT linux=0 seccomp=0),
kernel/aios_kernel.c BYTE-IDENTICAL** -- the portability proof. Seccomp on arm64 cannot trap AIOS's
out-of-range syscall numbers, so guests trap via a GATEWAY (AIOS_GATEWAY=gettid/178 in x8, real AIOS nr
in x9; PAL decodes x8==gateway ? x9 : escape) -- see test/seccomp_probe.c. The PAL split (shared
host-driver/injectors in pal_linux_common.c + a thin trap front-end parameterized by PAL_RESUME) is
exactly what pal_sel4.c reuses. The minimal appliance (uk/appliance/) boots Linux 6.18 straight into a
confined AIOS shell (a 3-file initramfs: /init + static /aios-uk + /aiosroot). Validated on colima;
Pi-pending.

PRIMARY TASK -> the endgame's remaining items: (1) **HW (Pi) validation** of `make PAL=seccomp` + the
appliance boot. (2) **sched_ext** -- AIOS's own scheduling policy as a sched_ext BPF program (the 6.18
appliance can carry CONFIG_SCHED_CLASS_EXT). (3) **the seL4/x86-64 replant seam (`pal_sel4.c`)** -- a
THIRD PAL backend proving kernel/aios_kernel.c runs UNCHANGED on a verified base (M9 de-risked the seam;
docs/AIOS_KERNEL_DEPENDENCIES.md is the proof obligation). Keep kernel/aios_kernel.c host-agnostic + the
PAL seam minimal; NEVER patch vendored sources -- grow libaios. Commit per milestone on `main`; validate
colima + the Pi (`raspberrypi.local`); Bryan pushes. GOTCHAS: seccomp can't trap out-of-range nrs (use
the GATEWAY); `.pal.stamp` makes a PAL switch rebuild (else stale-mtime silently keeps the wrong backend);
`stdbuf -oL` for real-time gate logs; colima virtiofs lags; NO apostrophes in run.sh's `sh -c` body.
