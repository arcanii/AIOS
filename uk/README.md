# AIOS userspace kernel (`uk/`)

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

## M3e — the libc retarget (in progress: real C compiles unmodified)

The road to "fully operational = dash" is recompiling real `sbase`/`dash` against AIOS's libc. The
foundation is in:

- **M3e.1** — AIOS **shadow standard headers** under `lib/include` (string/ctype/stdlib/unistd/fcntl/
  stdio/sys/...), compiled with **`-nostdinc -isystem <gcc-include>`** so ordinary C picks up AIOS's
  libc (implemented by libaios on the ABI) instead of the host's. libaios grew a standard-named POSIX
  surface (read/write/open/fork/execv/waitpid/strtol/getenv/…). Proof: `prog_libc.c` — real C, only
  standard headers.
- **M3e.2** — **FILE\* buffered stdio** (stdin/stdout/stderr, fopen/fgets/fprintf/fread/fwrite/…;
  line-buffered stdout, flushed on exit). Proof: `prog_stdio.c`.

`uk/run.sh` runs the whole suite (colima); each milestone is also validated natively on the RPi4.

## Next (per the design doc)

Continue M3e: errno + a real `-errno` path, `sys/stat.h` + path stat/getcwd/chdir/unlink (new ABI
syscalls), getopt, real getpid, signals → then **vendor + compile real `sbase`** unmodified, then
**dash** = fully operational. Then **M4** enforce the boundary (seccomp/namespaces so a program
*cannot* bypass the kernel) · **M5** `sched_ext` · **M6** the seL4/x86-64 replant seam.
