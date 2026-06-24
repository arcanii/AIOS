# DESIGN: AIOS as a portable userspace kernel (Linux now, seL4 later)

**Decision (2026-06-24, Bryan):** stop hand-building a from-scratch OS on seL4/RPi4 and re-base AIOS as
a **gVisor-style userspace kernel** that runs on a commodity host kernel. **Linux is the interim
substrate** (mature drivers, no ~32.4s stall, get operational fast); **a verified seL4 (x86-64) is
the destination** -- verification is the soul of the project, so the architecture is built to
*replant* onto seL4 behind a strict seam.

This is consistent with our own prior analysis (docs/EVAL_20260623_arm_smp_situation.md +
DR_20260623): seL4's verification guarantee is *not realized on RPi4* (proofs are uniprocessor; RPi4
is not a verified platform), and the stall is AIOS-on-seL4-specific. So today we pay all of seL4's
costs (porting every driver, the stall, MSI/DMA/hub firefighting) and collect none of its benefit.
This session's lead-#3 is the microcosm: we proved the brcmstb GT_4GB MSI programming *correct* on HW
(BAR_HI=0xf, VL805 EN=1, addr=0xf_fffffffc) yet still couldn't validate it because the hub wouldn't
deliver a single keystroke. Linux solves all of that for free.

## The model

```
        AIOS programs  (compiled for the AIOS ABI -- host-agnostic, unmodified across hosts)
              │   syscall instruction (trapped; the program thinks AIOS IS the kernel)
   ┌──────────▼───────────────────────────────────────────┐
   │  AIOS userspace kernel   (the stable, portable core)   │   ← future verified TCB
   │  ABI dispatch · VFS · process/exec · pipe/IPC · net ·   │
   │  signals · scheduling policy · the AIOS personality     │
   └──────────┬───────────────────────────────────────────┘
              │  PAL  -- Platform Abstraction Layer (narrow, the verified seam)
   ┌──────────▼─────────────┐               ┌────────────────────────────┐
   │  PAL → Linux  (NOW)     │      or       │  PAL → seL4 (DESTINATION)   │
   │  trap: ptrace/KVM        │               │  trap: VMM / fault handler  │
   │  hw:   Linux drivers     │               │  hw:   device untyped + IRQ │
   │  sched: sched_ext (BPF)  │               │  sched: seL4 sched config   │
   └──────────────────────────┘               └────────────────────────────┘
```

The defining property: **programs only ever see AIOS's ABI.** The host kernel is an implementation
detail of the PAL. Swap Linux→seL4 and the programs (and the entire AIOS kernel above the PAL) do
not change.

## Syscall interception platform

gVisor-style = the guest's syscalls are *trapped*, not cooperatively forwarded. We expose this as a
PAL primitive (`pal_guest_trap_next()` → returns the trapping thread + its syscall args; the AIOS
kernel services it and `pal_guest_return()`s the result). Per-host backends:

- **Linux, milestone 1 = `ptrace(PTRACE_SYSEMU)`.** Simplest, most portable, proven (gVisor's ptrace
  platform, UML). Every guest syscall SIGTRAPs into the AIOS kernel (the tracer), which reads
  registers, dispatches per the AIOS ABI, writes the result, and resumes. Slow (~2 ctx switches/
  syscall) but correctness-first. *This is the foundation -- get it right before optimizing.*
- **Linux, later = `seccomp`+SIGSYS (systrap) or KVM** for throughput. Same PAL contract.
- **seL4 (endgame) = a thin VMM / fault-handler.** seL4 has no ptrace; transparent trapping of an
  unmodified guest world is done via seL4's virtualization (Arm/x86 VM) or a user-level fault
  handler. The AIOS kernel plays the Sentry/VMM role. Same `pal_guest_trap_next()` contract.

## The PAL -- keep it NARROW (it is the future verified boundary)

Every primitive the AIOS kernel needs from the host. Smaller = smaller future proof obligation.

| Group     | Primitives                                                              |
|-----------|------------------------------------------------------------------------|
| memory    | map / unmap / protect pages (guest + kernel-internal)                  |
| execution | spawn guest context · `guest_trap_next` · read/write regs · `return`   |
| time      | monotonic clock · arm/cancel timer                                     |
| device/IO | the host-driver gateway (Linux: file/socket/ioctl fds; seL4: untyped+IRQ) |
| ipc       | AIOS-kernel-internal notification/queue (if the kernel is multi-component) |
| sched     | (Phase 2) install scheduling policy (Linux: sched_ext BPF; seL4: config)   |

Everything host-specific lives below this line. Everything above is portable AIOS.

## ABI + userspace reuse

- The **AIOS ABI is a POSIX-ish subset** -- because the existing personality (dash, sbase, sshd) is
  POSIX and already rides `libaios_posix`. Migration = **retarget `libaios_posix`'s bottom edge**
  from "seL4 IPC" to "AIOS ABI syscall" (a trap instruction on Linux). The programs above are
  unchanged; they recompile. This preserves the shell, coreutils, fs semantics, net design -- the
  actual value -- with minimal rework.
- The current **multiserver split** (pipe/fs/net/exec servers) maps to AIOS-kernel *modules*. For
  first light keep them in-process (monolithic AIOS kernel) for simplicity; the seL4 endgame can
  re-isolate them as separate protection domains (aligns with the microkernel model). Flag as a
  design knob, not a day-1 commitment.

## What transfers · what is retired

- **Transfers (the value):** dash/sbase/sshd, VFS + fs semantics, the net-stack design, the
  pipe/IPC + exec model, all host tooling, the POSIX shim logic.
- **Retired:** the seL4 kernel build, elfloader/boot, every hand-written driver (xHCI/VL805,
  GENET, V3D, eMMC, HDMI), and every stall mitigation (watchdog/prewarm/poll-clamp). The driver
  *knowledge* informs nothing we must maintain -- Linux owns it now.

## Staged roadmap

- **M0 -- Linux substrate.** Boot mainline/RPi-OS Linux on the RPi4 (and a Linux dev box/VM for
  iteration -- the Mac host is darwin, can't ptrace-Linux). Hardware just works. *(Bryan: flash.)*
- **M1 -- first light.** Minimal AIOS kernel (ptrace tracer) + a trivial AIOS-ABI program whose
  `write`/`exit` are serviced by the AIOS kernel. Proves the interception foundation. ~a few hundred
  LOC; de-risks the whole approach.
- **M2 -- one server.** Bring the VFS/fs path up behind the ABI (open/read/write/close to a real
  backing store via the PAL → Linux fds). `cat`, redirection work.
- **M3 -- the shell.** dash + sbase running as AIOS programs through the gate; pipes/exec via the
  AIOS kernel. This is "AIOS is operational."
- **M4 -- enforce the boundary.** Tighten the guest sandbox (seccomp/namespaces) so an AIOS program
  *cannot* reach Linux except through the AIOS kernel -- "only AIOS talks to Linux" by mechanism.
- **M5 -- AIOS owns scheduling.** sched_ext BPF policy authored by AIOS (needs RPi kernel 6.12+).
- **M6 -- the replant seam.** Prove the PAL by standing up a second backend (start the seL4/x86-64
  VMM path); the goal state where AIOS programs run unmodified on a verified base.

## Risks / open questions

- **Identity:** this reframes AIOS from "verified microkernel" to "portable userspace kernel, verified
  base later." Bryan: verification stays the soul → the PAL seam is sacred and stays minimal.
- **Interception perf:** ptrace is slow; acceptable for correctness/first-light, revisit at M5.
- **seL4 transparent trapping (M6):** seL4 has no ptrace -- the endgame relies on seL4 virtualization
  (CAmkES VMM / Arm or x86 VM). Confirm the VM story before over-investing; the PAL keeps it deferrable.
- **Work happens on a NEW branch** (keep `main`/seL4 history intact as the record + fallback).
