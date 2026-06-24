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

## Next (per the design doc)

M3 dash/sbase as AIOS programs (operational — the big ABI jump: argv/env/auxv loading, brk/mmap,
stat, exec/fork/wait…) · M4 enforce the boundary (seccomp/namespaces so a program *cannot* bypass
the kernel) · M5 `sched_ext` · M6 the seL4/x86-64 replant seam.
