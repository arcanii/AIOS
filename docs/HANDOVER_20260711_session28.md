# HANDOVER — session 28 (2026-07-03..11): the seL4 "soul" — from a link-scaffold to a real guest RUNNING on seL4

**HEADLINE:** the endgame of the pivot went from *aspiration* to *running code*. This session took the
verified-seL4 destination through **M6 (the seam scaffold) → the real-port scope + AArch64 ratification →
Phase 0 (the x7 pin) → Phase A (the kernel boots as an seL4 root task) → Phase B (a real AIOS guest RUNS
on seL4 via the fault-EP trap loop) → Phase C.1 (mmap + the resume-past-syscall fix)**. Six commits; the
host-agnostic kernel now *links against*, *boots on*, and *runs guests on* a non-Linux, formally-verified
microkernel — no Linux underneath.

Continues the 2026-06-24 pivot (AIOS as a gVisor-style userspace kernel on Linux; **verified seL4 —
now AArch64 — is the destination**; verification is the soul; programs see only the AIOS ABI, the host
behind a narrow PAL). Prior: docs/HANDOVER_20260703_session27.md. Read: memory
[[project_pivot_linux_userspace_kernel]] + docs/PLAN_20260709_sel4_real_port.md +
docs/DESIGN_20260703_pal_sel4_seam.md + uk/pal/sel4/boot.c.

## Discipline (unchanged, keeps catching real bugs — KEEP DOING IT)
Every milestone = a commit on `main` + validation (Linux line: `sh gate.sh` both backends; seL4 line:
build + boot on qemu-arm-virt) + an adversarial Workflow review (find→verify, read-only Explore agents)
BEFORE commit. Bryan pushes. The reviews caught real latent issues on B and C.1.

## What shipped (6 commits on `main`)

1. **`38ba63c` — M6, the seL4 PAL SEAM-PROVING SCAFFOLD (v0.5.41).** `uk/pal/pal_sel4.c` implements the
   full `pal.h` contract (~48 primitives) as documented stubs, each stating its seL4 proof obligation;
   `make PAL=sel4` compiles + **links** the host-agnostic kernel against a 3rd, NON-Linux PAL WITHOUT
   `pal_linux_common.c` (even links on macOS/arm64; `nm` = 0 ptrace/pal_linux symbols). `gate.sh` 3rd
   pass → `RESULT linux=0 seccomp=0 sel4=0`. Proof-obligation doc: docs/DESIGN_20260703_pal_sel4_seam.md.
2. **`95985ce` — the real-port SCOPE + AArch64 RATIFICATION.** A 6-researcher (3 live-web on current seL4
   facts + 3 repo auditors) → 2-architect → adversarial-verify Workflow → docs/PLAN_20260709_sel4_real_
   port.md. **Revised the pivot's x86-64 destination to AArch64** (Bryan ratified): AArch64 got full FC
   verification incl. fastpath (Apr 2024) + integrity/availability (Apr 2025); x86-64 is now the WEAKEST
   verified config (FC-to-C only) with zero reuse. Also found the seL4 15.0.0 SDK is ALREADY in `deps/`.
3. **`189ec41` — Phase 0: the libaios x7 pin.** seL4/aarch64 reads the syscall nr from **x7**, and the
   UnknownSyscall fault message carries x0–x7 but NOT x8/x9. Pinned `x7:=AIOS nr` at all conforming svc
   sites (libaios 3 asys stubs + sigtramp + the dev guests); guest_escape's raw svc gets a sub-0x1000
   escape sentinel. All AIOS nrs ≥0x1000 → never a valid negative seL4 nr → every guest svc deterministic-
   ally UnknownSyscall-faults. **Inert on Linux** (x7 dead there); colima gate green both backends.
4. **`5461c97` — Phase A: the kernel BOOTS as an seL4 root task.** `settings-uk.cmake`
   (AArch64/non-MCS/seL4-15.0.0/**unicore**/debug+printing/EL2-hyp) + `projects/aios-uk/CMakeLists.txt`
   (rootserver `aios-uk-sel4`, built under `-DAIOS_UK_BUILD=1`; `aios_kernel.c` compiled BYTE-IDENTICAL
   via a per-source `-Dmain=aios_kernel_main`) + `uk/pal/sel4/boot.c` (the REAL seL4 PAL). The unchanged
   kernel boots on qemu-arm-virt + prints its banner via `pal_host_write`→`seL4_DebugPutChar`.
5. **`dba2f9a` — Phase B: a real AIOS GUEST RUNS (family A, the fault-EP trap loop).** `guest_hello` (a
   static AIOS-ABI ELF in the image CPIO) runs to completion: its `svc` UnknownSyscall-faults →
   `pal_guest_next` = one `seL4_Recv` decodes the 13-MR fault (nr=MR7, args=X0..X5) → the kernel services
   WRITE (reads the buffer from the guest's frames via `vspace_access_page_with_callback` — the seL4
   `process_vm_readv`) → `pal_guest_return` replies (X0=retval, FaultIP+4) → EXIT → "hello from an
   AIOS-ABI program" + exit 42. **The trap model that replaces ptrace works.**
6. **Phase C.1 — mmap + the resume-past-syscall fix** (validated + reviewed; commit `f21faff`). Family B
   begins: `pal_guest_mmap` allocates anonymous memory via `vspace_new_pages` (Untyped→Frames mapped into
   the guest VSpace — the proven 0.4.x MMAP_ANON pattern) and stashes the vaddr; **`pal_guest_resume` now
   serves two cases** — (a) the Inactive spawn-kick (`seL4_TCB_Resume`) and (b) resume a fault-stopped
   guest past a serviced syscall (reply FaultIP+4 with the stashed x0) — **fixing the Phase B review's
   must-fix**. Test guest `guest_mmap` mmaps 64 KiB, round-trips a pattern across two pages, exits 42.

## The mental model (how the seL4 backend replaces ptrace) — carry forward

- **A guest is a seL4 THREAD** (a `sel4utils_process_t`) in its own VSpace/CSpace, configured with a
  **fault endpoint** pointing at the root task. There is NO ptrace.
- **Trap = fault.** The guest's AIOS `svc` (x7=nr, Phase 0) is an *unknown syscall* to seL4 → an
  UnknownSyscall FAULT delivered to the root's fault EP. `pal_guest_next` = one `seL4_Recv`. The 13-MR
  fault frame is snapshotted into `g_fregs` (X0..X7=MR0..7, FaultIP=8; nr is MR7).
- **Return/resume = reply.** To resume a fault-stopped guest, REPLY (label 0, length 9, X0=result, X1..X7
  echoed, **FaultIP+4** to step past the svc). A length-0 or un-advanced-FaultIP reply re-faults forever.
  `reply_resume()` is the shared helper (`pal_guest_return` = reply with the retval; `pal_guest_resume`
  case (b) = reply with an inject primitive's stashed x0).
- **Guest memory** = `vspace_access_page_with_callback` (double-map the guest's own frame into the root
  vspace, cacheable=1). **mmap** = `vspace_new_pages` into the guest vspace. The Untyped pool is the
  memory budget.
- **Non-MCS reply cap:** the implicit one-slot caller cap from the fault `Recv` survives the service path
  because only KERNEL-OBJECT invocations run between the fault and the reply (Retype/Page_Map — never a
  userspace-endpoint `seL4_Call`). Once a userspace fs/net SERVER `Call` intervenes (Phase D/F),
  `seL4_CNode_SaveCaller` + a deferred reply becomes necessary [[feedback_sel4_nested_call]].

## Build + boot recipe (the Mac IS the build host)

`./build_environment.sh --check` = 12 OK (aarch64-linux-gnu-gcc 15.2 via brew + cmake/ninja/qemu 11.0.2/
dtc). The seL4 15.0.0-dev SDK + libs are in `deps/` (standard seL4 repo layout; `projects/*/CMakeLists.txt`
is globbed). Build tree `build-uk/` is gitignored.

```sh
mkdir -p build-uk && cd build-uk
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- \
  -DKernelSel4Arch=aarch64 -DKernelMaxNumNodes=1 -DAIOS_UK_BUILD=1 \
  -DAIOS_SETTINGS=settings-uk.cmake -DKernelPlatform=qemu-arm-virt ..
ninja                                     # FULL ninja -- the "[N/N] Generating ...image..." step is the boot artifact
qemu-system-aarch64 -machine virt,virtualization=on -cpu cortex-a53 -smp 1 -m 2G -display none \
  -serial file:/tmp/boot.log -kernel images/aios-uk-sel4-image-arm-qemu-arm-virt &
# scrape /tmp/boot.log for the exit code; then: pkill -f aios-uk-sel4-image
```

**Gotchas:** after editing `boot.c`, run the FULL `ninja` (not `ninja aios-uk-sel4` — that builds the
rootserver ELF but NOT the elfloader-wrapped image). QEMU won't exit on its own (boot.c halts) → run
backgrounded + `pkill -f aios-uk-sel4-image`. macOS has no `timeout`/`setsid`. The Bash cwd persists
across calls (avoid re-`cd build-uk`; use absolute paths or `ninja -C`). Guest built via
`add_custom_command` (freestanding, own `_start`), NOT `add_executable` (that relinks sel4runtime).

## State + open items

- **Unpushed commits:** Phase B (`dba2f9a`) + Phase C.1 (`f21faff`) — Bryan pushes (he pushed through A).
- **RPi5 OFFLINE all session** (mDNS `tkrpi5.local` unreachable — powered down/off-network). **Phase 0's
  gcc-15 recheck is PENDING** (low-risk: inert codegen, gcc-13 validated). Re-gate `sh gate.sh` when it's
  back. The RPi5 `/opt/aios` aios-console persistent deploy (v0.5.40) is untouched.
- No ABI change / version bump for Phases 0/A/B/C.1: the RUNNABLE Linux kernel is untouched (boot.c
  compiles ONLY in the seL4 build; `aios_kernel.c`/`pal.h`/the uk Makefile/the pal_sel4.c link-canary are
  unchanged → the Linux backends + `make PAL=sel4` are provably green, no colima re-gate needed for the
  seL4-only work).
- Phase C is being done in sub-milestones: **C.1 mmap = DONE.** Remaining: **C.2 fork** (VSpace copy +
  child TCB + the two-return + resume both), **C.3 exec** (the kernel IS the ELF loader; needs the tarfs),
  **C.4 a read-only tarfs** (open/read/close/fstat over the aiosroot.tar), **C.5 pipes** (an in-root-task
  byte-ring + park/wake). Also: **object teardown** — `pal_guest_exit` currently suspends but does NOT
  `sel4utils_destroy_process` (a leak, fine for one-shot; lands with the fork/exec churn). And the
  single-guest state (`g_proc`, `g_fregs`, `g_started`, `g_inject_x0`) must generalize to a **process
  table** for fork (C.2's first task).

---

## >>> SEED PROMPT (next session) <<<

Continue building AIOS toward the verified-seL4 destination — the REAL seL4 PORT is UNDERWAY (the
2026-06-24 pivot; **AArch64 ratified**; verification is the soul; programs see only the AIOS ABI). READ
FIRST: memory [[project_pivot_linux_userspace_kernel]] (the DIRECTION line + the M6/scope/Phase-0/A/B/C.1
entries) + docs/PLAN_20260709_sel4_real_port.md + docs/DESIGN_20260703_pal_sel4_seam.md +
docs/HANDOVER_20260711_session28.md + uk/pal/sel4/boot.c.

WORKING BRANCH = `main` (commit per milestone; Bryan pushes; confirm unpushed with Bryan). DONE this arc:
M6 the seam scaffold (kernel LINKS a non-Linux PAL, `make PAL=sel4`), the real-port SCOPE + AArch64
ratification (docs/PLAN_20260709_sel4_real_port.md), then **Phases 0/A/B/C.1** — the x7 pin / the
host-agnostic `aios_kernel.c` BOOTS as an seL4 root task on qemu-arm-virt (EL2 hyp, unicore) / a real
AIOS GUEST RUNS via the fault-EP trap loop (guest_hello, exit 42) / **C.1 mmap + the resume-past-syscall
reply fix** (guest_mmap mmaps 64 KiB + round-trips a pattern, exit 42). The real seL4 PAL is
`uk/pal/sel4/boot.c` (built by `projects/aios-uk/`, `-DAIOS_UK_BUILD=1`, `settings-uk.cmake`); the
`uk/pal/pal_sel4.c` scaffold stays the `make PAL=sel4` link canary; `aios_kernel.c` is compiled
BYTE-IDENTICAL (`-Dmain=aios_kernel_main`). Build+boot recipe + the seL4 mental model + gotchas are in the
handover. Every seL4 API is kernel-source-validated by a research Workflow BEFORE coding, and every
milestone gets an adversarial find→verify review BEFORE commit (both caught real latent issues — KEEP
DOING BOTH).

PRIMARY TASK → **continue Phase C (family B, the process model; the single biggest phase).** Next
sub-milestone = **C.2 fork** — generalize the single-guest state to a PROCESS TABLE, then `pal_guest_fork`:
new VSpace + TCB, copy the parent's frames (EAGER copy first; COW is a later optimization behind the same
fault EP), set the child's registers to the parent's fault frame with x0=0 + FaultIP+4, and resume BOTH
(the kernel's do_fork resumes parent then child). Reuse the 0.4.x parts bin: `src/process/fork.c` +
`src/boot/spawn_util.c` (+ `cow.c`/`reap.c`). Then **C.3 exec** (the kernel parses the ELF + rebuilds the
VSpace; needs C.4), **C.4 a read-only tarfs** (serve aiosroot.tar: open/read/close/fstat), **C.5 pipes**
(in-root-task byte-ring + PAL_EWOULDBLOCK/EPIPE + park/wake). Phase C's gate-key target is `pipebig`. Also
land **object teardown** (`pal_guest_exit` → `sel4utils_destroy_process`) once fork/exec churn arrives.
TRIPWIRE (from the plan): if Phase C exceeds ~4 sessions, ship eager-copy fork and drop COW.

OTHER (Bryan's call): re-run the RPi5 gcc-15 gate when the Pi is back (Phase 0 recheck, pending); the
later seL4 phases D (fs/console)/E (signals+login)/F (net)/G (confinement→28/28 QEMU parity)/H (hardware +
the stall probe)/I (verification alignment) per the plan.

GOTCHAS (build on the MAC; the seL4 line, NOT colima): FULL `ninja` after a boot.c edit (the image step is
separate); `pkill -f aios-uk-sel4-image` (qemu won't self-exit); macOS has no `timeout`/`setsid`; the Bash
cwd persists (use absolute paths); a guest is built via `add_custom_command` (freestanding), never
`add_executable`. Non-MCS reply cap survives kernel-object invocations but NOT a userspace-endpoint
`seL4_Call` (matters at Phase D/F — use SaveCaller then). The RPi5 stays the Linux-line HW validator once
it is back online.
