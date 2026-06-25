# HANDOVER -- session 17 (2026-06-24): THE PIVOT + a userspace kernel on Linux

The big one. AIOS pivoted off seL4/RPi4 bare-metal onto a **gVisor-style userspace kernel on Linux**,
and in one session went from a wedged seL4 board all the way to a host-portable userspace kernel
running real Unix utilities (cat/wc/tail) and managing guest memory on the actual RPi4. seL4 work is
preserved on `main` as the record/fallback. Detailed live state: memory
[[project_pivot_linux_userspace_kernel]]. Design: docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md.

## 0. How the session started (seL4 lead #3, then the pivot)
- Began finishing the seL4 xHCI-MSI keyboard lead. **ROOT CAUSE FOUND + FIXED:** the MSI target
  0xfffffffc overlapped the [0,4GB) RC_BAR2 inbound DMA window, so the VL805's MSI write was swallowed
  as DMA and never reached the RC. Fix = GT_4GB target (hi=0xf), verified vs Linux v6.6 pcie-brcmstb.c.
  `/proc/xhci.msiarm` proved the GT_4GB programming CORRECT on HW (EN=1, addr=0xf_fffffffc) -- but the
  verdict stayed inconclusive (the hub never delivered a keystroke). Commits 484e091 + e4edd13 + c92af3e
  on main. Detail: [[project_usb_kbd_dma_stall]].
- That pile-up (MSI + hub-input + the ~32.4s stall, all to replicate what Linux gives free) is exactly
  what triggered Bryan's decision to **replace seL4 with Linux**.

## 1. THE PIVOT (Bryan's decision + the architecture)
- **Decision:** Linux = interim substrate (mature drivers, no stall, get operational); **verified seL4
  on x86-64 = the destination** (verification is the soul). Consistent with EVAL_20260623 (seL4's
  verification isn't realized on RPi4 anyway).
- **Two scoping answers pinned the architecture:** (1) **full gVisor-style transparent syscall
  interception** -- AIOS programs compile for the **AIOS ABI**, never see the host; their syscalls are
  trapped + serviced by the AIOS kernel. (2) **verification is the soul** -> Linux is a pragmatic
  interim; keep a strict, minimal **PAL** so we can replant onto seL4. Programs only ever see AIOS's
  ABI, so swapping the host underneath is invisible.
- The trap mechanism is a PAL primitive with per-host backends: **Linux = ptrace; seL4 = a VMM/fault
  handler** (deferred behind the PAL).

## 2. M0 -- Linux on the Pi (DONE)
- Bryan flashed `2025-12-04-raspios-trixie-arm64.img` (balenaEtcher) onto the 32GB SD (the old AIOS-seL4
  card -- fine, all in git). I did the headless prep on /Volumes/bootfs: empty `ssh` + `userconf.txt`.
- Pi boots **Linux 6.12.47 (Debian 13 Trixie, aarch64)** at **192.168.0.8** (= raspberrypi.local),
  **login pi / aios**, **keyless SSH installed** (the Mac's id_ed25519). HARDWARE VINDICATION: 4 cores,
  eth0/GENET, **the USB keyboard enumerates** through the VL805 hub (the exact path that wedged seL4),
  Razer mouse, V3D (/dev/dri/card0+renderD128). gcc+make preinstalled, passwordless sudo.
- CAVEAT: **sched_ext NOT compiled into the stock RPi kernel** (M5 needs a custom kernel w/
  CONFIG_SCHED_CLASS_EXT; 6.12 has the code, just not enabled).

## 3. The userspace kernel -- M1..M3c.3 (DONE, all validated colima + native RPi4)
Tree is `uk/` on the `userspace-kernel` branch. The kernel (`kernel/aios_kernel.c`) includes ONLY
`aios_abi.h` + `pal.h` -- host-agnostic through every milestone. The ONLY host-aware file is
`pal/pal_linux.c`.
- **M1 (fdd43ca) first light:** ptrace traps an AIOS-ABI program's syscalls; the kernel services WRITE/
  EXIT. AIOS nr 0x1000 isn't a Linux syscall -> the output provably came from AIOS.
- **M2 (b9f8549) VFS:** kernel-owned fd table (AIOS fd -> opaque pal_file_t); OPEN/READ/WRITE/CLOSE to
  real host storage via the PAL. A program creates a REAL host file, reads it back.
- **M3a (c69ac48) loader+argv:** pal_spawn_guest passes argv; a real `cat` reads its filename from argv.
- **M3b (de5f481) libaios:** a minimal C runtime on the AIOS ABI (lib/libaios.{c,h}) -- _start, printf,
  malloc, string/ctype. Ordinary C (main/printf/malloc/argv) compiles + runs.
- **M3c.1 (a99eb07):** libaios grows (strcmp/strchr/atoi/isspace... + POSIX aliases); a real `wc` reads
  files OR stdin -- so host pipes flow in (`ps aux | aios-uk prog_wc`).
- **M3c.2 (7c76b7e) real memory:** driver refactored **PTRACE_SYSEMU -> PTRACE_SYSCALL**; AIOS_SYS_MMAP
  REWRITES the guest's own svc in place into a Linux mmap (number via NT_ARM_SYSTEM_CALL), so the kernel
  grows guest memory on demand. malloc is now mmap-backed (prog_bigalloc verifies 4MB). **KEY BUG:** must
  save+restore the guest's regs around the rewrite (mmap clobbers x0..x5 but the guest's syscall wrapper
  expects all-but-x0 preserved -> SIGSEGV-looped until fixed).
- **M3c.3 (7efc055) fstat+lseek:** the first STRUCTURED syscall (fills a struct aios_stat in guest mem);
  a real `tail -n N` fstat+lseeks to a large file's trailing window.

**Current AIOS syscall surface:** WRITE/READ/OPEN/CLOSE/EXIT/MMAP/FSTAT/LSEEK. Real utilities run on
real HW: cat, wc (+stdin pipes), tail, mmap stress.

## 4. Dev environment (working)
- Mac is darwin -> can't ptrace-Linux. Dev runs in **colima** (brew installed; `colima start --arch
  aarch64`, kernel 6.8, PTRACE_SYSEMU/SYSCALL OK). Build+run via **`uk/run.sh`** (an aarch64 `gcc:13`
  container, `--cap-add=SYS_PTRACE`).
- Real-HW validation: `scp -r uk pi@192.168.0.8:~/ && ssh pi@192.168.0.8 'cd ~/uk && make && ./aios-uk
  <prog>'`. (gcc/make on the Pi; tracing our own child needs no sudo/cap.)
- **DEBUG TIP (painfully learned):** a ptrace bug (SIGSEGV-loop / stuck waitpid) hangs SILENTLY. Run a
  focused container test with in-container **`timeout N`** so a hang shows as rc=124; instrument
  `pal_linux.c` with `fprintf(stderr,...)`.

## 5. State / housekeeping
- **Branch `userspace-kernel`** (LOCAL -- Bryan pushes): 8 commits, fdd43ca M1 .. 7efc055 M3c.3.
- **`main`** (LOCAL -- Bryan pushes): the pivot design doc (298c418) + the seL4 lead-#3 closure
  (484e091/e4edd13/c92af3e). seL4 tree intact as the record/fallback.
- colima is left running (`colima stop` to reclaim). Pi healthy at 192.168.0.8 with a `~/uk` working copy.
- Every uk/ commit is source-only (build artifacts gitignored) + validated on colima AND the Pi.

## 6. NEXT -> operational (the process model is the frontier)
"Operational" = a working **dash**. The one subsystem deliberately NOT started: the **process model**
(exec/fork/wait/pipe).
- **fork = a MULTI-PROCESS-KERNEL refactor:** g_guest -> a process table; main loop -> waitpid(-1) +
  per-PID dispatch; PTRACE_O_TRACEFORK to auto-trace children; per-process fd tables; fork/exec via
  injection (like mmap). The largest single piece left.
- **exec alone is tractable** (in-place execve injection of the guest's own svc -- but execve is special:
  no normal return, an exec-event stop to consume).
- **In parallel:** grow libaios into the AIOS-ABI RETARGET of the seL4 **libaios_posix** (on main; the
  durable reuse). Ideally shadow standard headers (string.h/stdio.h/...) under lib/include with -nostdinc
  so real `sbase`/`dash` sources compile UNMODIFIED. Then recompile sbase, then dash.
- Later: **M4** enforce the boundary (seccomp/namespaces so a program CANNOT bypass the kernel -- today an
  AIOS program's stray real-Linux syscall just ENOSYSes, nothing forbids it). **M5** sched_ext (custom RPi
  kernel). **M6** pal_sel4.c -- the replant seam (prove the PAL on a second host).

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue building AIOS as a **gVisor-style userspace kernel on Linux** (the 2026-06-24 PIVOT off
seL4/RPi4 -- Linux is the interim substrate, verified seL4-on-x86-64 is the destination; verification is
the soul; programs see only the AIOS ABI, the host sits behind a narrow PAL). READ FIRST: memory
[[project_pivot_linux_userspace_kernel]] + docs/HANDOVER_20260624_session17.md +
docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md + uk/README.md.

WORKING BRANCH = `userspace-kernel` (8 commits M1..M3c.3, LOCAL -- Bryan pushes). The `uk/` tree: a
host-agnostic kernel (kernel/aios_kernel.c includes ONLY aios_abi.h + pal.h) over the ONLY host-aware file
(pal/pal_linux.c, a PTRACE_SYSCALL driver) + libaios (lib/libaios.{c,h}, a C runtime on the AIOS ABI).
DONE: M0 Linux on the Pi; M1 first-light; M2 VFS; M3a argv; M3b libaios; M3c.1 wc+pipes; M3c.2 real
mmap-backed malloc via in-place syscall injection (PTRACE_SYSCALL + NT_ARM_SYSTEM_CALL; SAVE/RESTORE the
guest regs around the rewrite -- that bug SIGSEGV-looped); M3c.3 fstat+lseek + a real tail. AIOS syscalls:
WRITE/READ/OPEN/CLOSE/EXIT/MMAP/FSTAT/LSEEK. Real utilities run (cat/wc/tail/bigalloc), validated colima +
native RPi4.

DEV LOOP: `uk/run.sh` (colima aarch64 container, --cap-add=SYS_PTRACE) for fast iteration; HW-validate via
`scp -r uk pi@192.168.0.8:~/ && ssh pi@192.168.0.8 'cd ~/uk && make && ./aios-uk <prog>'` (login pi/aios,
keyless SSH already installed; Linux 6.12). DEBUG: a ptrace hang is SILENT -- use in-container `timeout N`
(hang -> rc=124) + fprintf(stderr) in pal_linux.c.

PRIMARY TASK -> the PROCESS MODEL (toward operational = dash): start with **exec** (in-place execve
injection -- handle that execve has no normal return + an exec-event stop), then **fork** (the big one: a
multi-process kernel -- g_guest -> a process table, main loop -> waitpid(-1) + per-PID dispatch,
PTRACE_O_TRACEFORK to auto-trace children, per-process fd tables, fork via injection) + **wait** + **pipe**.
IN PARALLEL grow libaios into the AIOS-ABI retarget of the seL4 **libaios_posix** (on main; shadow standard
headers under lib/include w/ -nostdinc so real sources compile unmodified) -> recompile **sbase** -> **dash**
= operational. Keep the PAL seam minimal (it's the future verified boundary). Commit per milestone on the
`userspace-kernel` branch; validate colima + Pi each step; Bryan pushes.

THEN: M4 enforce the boundary (seccomp/namespaces); M5 sched_ext (stock RPi kernel lacks
CONFIG_SCHED_CLASS_EXT -> custom kernel); M6 pal_sel4.c (the replant seam). The seL4 stall + the lead-#3
keyboard are MOOTED by leaving the platform (closed-but-understood: GT_4GB MSI fix proven correct, verdict
inconclusive; seL4 tree preserved on main as record/fallback).
