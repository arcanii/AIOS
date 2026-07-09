# PLAN — the real seL4 port (the option-B epic scoped)

**Date:** 2026-07-09 (session 28). **Status:** SCOPE, not yet started. Follows the M6 seam scaffold
(`uk/pal/pal_sel4.c`, commit 38ba63c) + its proof-obligation doc
(`docs/DESIGN_20260703_pal_sel4_seam.md`). This document turns "the soul is scoped" into a concrete,
phased, target-decided plan. It was produced by a research + audit fan-out (6 agents: 3 live-web on
current seL4 facts, 3 auditing this repo) → two synthesis architects → adversarial verification of every
load-bearing claim; the web facts and the repo facts below were spot-checked by hand.

> **One-line recommendation:** target **AArch64 / seL4 15.0.0 / non-MCS / unicore**, develop on
> **qemu-arm-virt**, reuse the **0.4.x parts bin** for families B and C, and defer the hardware-board
> choice (RPi4 vs RPi5) to the end. **This revises the pivot's "verified seL4 (x86-64)" destination** —
> x86-64 is now the *weakest* verified seL4 config and has zero code/guest reuse. Two decisions need
> Bryan (§7).

---

## 1. What the research overturned (the load-bearing facts, verified)

Three findings change the picture the pivot doc and my own M6 §7 were written against:

1. **AArch64 is now strongly verified — and the SDK is already in-repo.** seL4's AArch64 functional-
   correctness proof (incl. fastpath) completed **Apr 2024**; integrity/availability **Apr 2025**;
   confidentiality was scheduled Q2/26 but is **not yet announced (do not claim it)**. And the infra
   barrier I called "none installed" in M6 §7 is largely *already paid*: `deps/` pins **seL4 kernel
   15.0.0-dev**, seL4_libs, sel4runtime, musllibc, seL4_tools, util_libs (see `DEPS.md`), with
   `build_environment.sh` to clone+patch them. `deps/kernel/configs/AARCH64_bcm2711_verified.cmake`
   (the verified RPi4 config) exists in-tree.

2. **x86-64 is the *weakest* verified config, and costs the most.** X64 is FC-to-C **only** — no
   fastpath, no integrity/confidentiality theorems, no binary verification, no VT-x, unicore — with no
   funded deepening effort found for 2024–2026. It would *also* force a full libaios asm port and a
   rebuild of every guest. There is no dimension on which x86-64 wins. **RISC-V RV64** is actually the
   *deepest*-verified arch (FC-to-**binary** = compiler out of the TCB, plus the only completed **MCS**
   FC proof, 29 Jun 2026) — recorded as the endgame option if compiler-out-of-TCB ever becomes a
   requirement, not paid now.

3. **The guest userland already fits AArch64 — with one mandatory one-instruction fix.** The guests are
   static aarch64 ELFs. On seL4/aarch64 the syscall number is decoded from **x7**, and the
   **UnknownSyscall fault message carries x0–x7 but NOT x8/x9**. Today libaios puts the gateway in x8 and
   the real AIOS number in **x9** — which would *never reach the fault handler*, and worse, an
   uncontrolled x7 that happens to equal a valid seL4 syscall number (e.g. `seL4_Yield` = −7) makes the
   `svc` a **silent no-op** (the guest reads its own argument back as the return value). **Pinning
   `x7 := AIOS nr` in libaios's 3 `asys` stubs + the sigreturn trampoline is mandatory** — and it is
   inert under both Linux backends (x7 is a dead caller-saved register there), so it ships and
   regression-tests on the Linux line *before any seL4 code exists* (Phase 0).

### Verification scorecard (as of 2026-07-09, from official sources)

| Arch | FC (to C) | Fastpath | Binary verif. | Integrity | Confidentiality | MCS FC | Notes |
|---|---|---|---|---|---|---|---|
| **AArch64** | ✅ Apr 2024 | ✅ | ❌ | ✅ Apr 2025 | ⏳ (Q2/26, unannounced) | ❌ (~Q3/27) | verified kernel is **EL2 hyp + FPU**, unicore |
| x86-64 | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | weakest; no VT-x |
| RISC-V RV64 | ✅ | ❌ | ✅ | (roadmap) | (roadmap) | ✅ 29 Jun 2026 | deepest; compiler out of TCB |
| ARMv7 32-bit | ✅ | ✅ | ✅ | ✅ (orig) | ✅ (orig) | ❌ | stagnant, 32-bit ABI rework |

All verified configs are **unicore** (`KernelMaxNumNodes=1`); SMP/multikernel verification is ~Q3/28.
A verified AIOS-on-seL4 is single-core for the foreseeable future — which is fine: the uk kernel is
single-threaded by design, and 0.4.x measured seL4's big-kernel-lock making SMP ~2.6× *slower* for IPC.

---

## 2. The target decision (matrix)

Scored 1 (worst) – 5 (best). Full reasoning in the session's synthesis; summary:

| Criterion | A: AArch64/QEMU | B: AArch64/RPi4 | C: x86-64 | D: RISC-V | E: ARMv7 |
|---|---|---|---|---|---|
| Verification today | 4 | 4 | 2 | 5 | 4 |
| Guest-userland reuse | 5 | 5 | 1 | 1 | 1 |
| 0.4.x code reuse | 5 | 5 | 1 | 1 | 2 |
| Stall-risk | 5 | **1** | 5 | 4 | 3 |
| Dev-loop (Bryan's arm64 Mac) | 5 | 2 | 3 | 3 | 2 |
| Effort to first-light | 5 | 3 | 1.5 | 1.5 | 1 |
| **Total** | **29** | **20** | **13.5** | **15.5** | **13** |

**Recommendation: staged AArch64.** Develop on **A (qemu-arm-virt)**; attach verified-*hardware* claims
later on a real board (§7 decision 2). The guest binaries run with the one-line x7 change; the 0.4.x
parts bin (build recipe, sel4utils spawn/fork/COW/reap, fs/net/timer/console servers, virtio drivers)
is aarch64 and reuses *only* on this arch; AArch64 has the FC+integrity proofs. The genuinely **new**
work — the part no target avoids — is **family A**: the fault-EP trap loop + guest-frame scratch mapping
+ the static-ELF loader/initial-stack builder.

### Kernel-flavor calls (decided)

- **Master (non-MCS) API, pinned to seL4 15.0.0.** The verified AArch64 kernel is non-MCS (MCS FC is
  RISC-V-only until ~Q3/27; Microkit would *force* MCS and is static-PD-only besides). Non-MCS also
  maximizes 0.4.x reuse (its deferred-reply servers are all `SaveCaller`-based). **Hedge:** abstract a
  "guest reply token" in the real PAL from day one (a `SaveCaller` slot on master; a reply object on
  MCS) so a ~2027 MCS migration is contained.
- **Unicore.** All verified configs are unicore; and unicore sidesteps the entire 0.4.x multi-core
  teardown patch set — **the port baselines STOCK seL4 15.0.0 with no kernel patches**, preserving the
  verified-kernel claim (the 1865-line 0.4.x kernel patch was RPi4-stall + SMP-teardown medicine + stall
  diagnostics, none of which the verified unicore config needs).
- **Framework:** a classic C root task on **sel4runtime + seL4_libs** (vka/vspace/sel4utils/allocman) —
  the only supported *dynamic* path (create/destroy processes at runtime) and exactly what 0.4.x used.

---

## 3. How the build fits together (the mechanical shape)

- **The uk/ tree is untouched.** A new `projects/aios-uk/` root-task project reuses the 0.4.x CMake
  recipe (`settings.cmake` → qemu-arm-virt; later `settings-rpi4.cmake`). `deps/kernel` currently
  **symlinks to an external checkout** (`~/Desktop/github_repos/seL4`) — Phase A re-pins stock 15.0.0
  into the manifest (Risk R9).
- **`kernel/aios_kernel.c` is compiled by reference, byte-identical** — never copied — with a
  `sha256sum` tripwire in the seL4 gate. The **`main()` collision** (aios_kernel.c owns `main(argc,argv)`;
  sel4runtime also enters at `main`) is resolved by compiling the kernel with **`-Dmain=aios_kernel_main`**
  (same spirit as `PAL=` selection); the real PAL's `boot.c` provides the root-task `main`, reads the
  guest path + confinement config from a **boot-config file in the CPIO** (no `getenv` on seL4 — the
  `AIOS_ROOT`/`AIOS_NET_*` env vars become config keys with the *same fail-closed parsing*), and calls
  `aios_kernel_main(2, {"aios-uk","/sbin/init"})`.
- **The real PAL lives in a new `uk/pal/sel4/` directory** (`boot.c`/`guest.c`/`loader.c`/`fs.c`/…) that
  needs seL4 headers. **The M6 scaffold `uk/pal/pal_sel4.c` stays untouched** as the `make PAL=sel4`
  link-purity canary (gate pass 3) — the uk Makefile must never ingest seL4 headers.
- **The guest image is built by the existing uk Makefile** (static aarch64 ELFs → `mkaiosroot.sh` →
  `aiosroot.tar`), bundled as a CPIO module. The 0.4.x caps-in-argv userland convention is **not** the
  guest contract here — guests get the plain SysV `argc/argv/envp` stack (verified against `_start`,
  which reads only `argc` at `[sp]` and `argv` at `sp+8`; **no auxv, no TLS, no vDSO**).
- **The gate is re-hosted** (`uk/sel4-gate.sh`, grown per phase): boot QEMU with serial on a pty →
  drive expect-style (the hardened `test/login_pty.c` pattern) → an in-image dash script runs the
  `prog_*` binaries and prints `KEY=rc` markers → the host scrapes + aggregates. Net keys use
  virtio-net + SLIRP hostfwd to the existing `test/` helpers. **Discipline unchanged:** every phase =
  commit + `sh gate.sh` (Linux backends + scaffold, proving zero regression) + `sh sel4-gate.sh` (that
  phase's key subset) + an adversarial Workflow review, before commit.

---

## 4. The phases (each ends runnable + committed + gated)

| Phase | Goal | New family / mechanism | Gate keys (cum.) | Effort |
|---|---|---|---|---|
| **0** | **libaios x7 pin** (on the Linux line, now) | guest ABI prep; no seL4 | Linux stays 28/28 | **S** |
| **A** | workspace + `aios_kernel.c` boots as root task + real console `write` | root-task bring-up; PL011; CPIO config | 0 (banner) | **M** |
| **B** | family A: one guest faults → serviced → replies | fault-EP `Recv` loop; ELF loader; scratch-map; reply token | 0 (M1 demo) | **L** |
| **C** | family B: process model (mmap/fork/exec/exit/pipes) + RO tarfs | VSpace copy/teardown; Untyped budget | **1** (pipebig) | **L** |
| **D** | writable RAM-fs from `aiosroot.tar` → **dash + sbase run** | breadth (~24 fs ops); CNTVCT clock | **10** | **L** |
| **E** | signals + job control + termios → **login on seL4** | signal frame via `WriteRegisters`; console line discipline; bound notification | **19** | **M–L** |
| **F** | timer + net (**net last**, reuse 0.4.x stack in-proc) | ltimer; virtio-net; the co-wait as one-shot timer + sticky notifications | **24** | **L** |
| **G** | confinement-as-capabilities + red-team → **28/28 parity (QEMU)** | subtree-root fs handle; net allow-list; x7 escape probe | **28** | **M** |
| **H** | hardware bring-up + **the stall probe** | RPi4 (default) mini-UART + GENET, or RPi5 | 28 on HW | **M–L** |
| **I** | verification alignment (pinned verified config, honest trust story) | `KernelVerificationBuild` variant | 28 on verified config | **M** |

**Total effort feel: ~15–25 sessions** — a multi-month epic at the project's cadence. Say so plainly.

Notes on the load-bearing phases:
- **Phase 0** (do first, half a session): pin `x7` at the **8 conforming svc sites** (libaios `asys`/
  `asys4`/`asys5` + `__aios_sigtramp`; the dev guests `guest_{hello,cat,fileio}` + guest_escape's
  *gateway* svc). Leave **guest_escape's second, raw svc unpinned** — it is the deliberate red-team
  escape vector. Then the seL4 decode is: *fault-message X7 ≥ 0x1000 → AIOS nr; else surfaced verbatim →
  the existing M4 kill policy.* Exit: `objdump -d` shows x7 pinned before every conforming `svc`; full
  `gate.sh` still `linux=0 seccomp=0 sel4=0`.
- **Phase B** measures the syscall round-trip (two slowpath IPCs, 13 words out / 9 back) vs the ptrace
  and seccomp backends — the "seL4 is the fast backend" assumption gets **confirmed or corrected here**,
  not presumed. Reply contract: echo x1–x7, x0=retval, **FaultIP += 4** (an empty reply re-executes the
  `svc` and fault-loops forever).
- **Phase D** runs the fs **in-proc as a library, not an IPC server** — because the 0.4.x `fs_server`
  `SaveCaller`-parks its *callers*, and the single-threaded kernel must never park (Risk R5). Same for
  net in Phase F. Persistence (ext2/virtio-blk) is a post-parity backlog item; the gate needs none.
- **Phase E**: the verified AArch64 kernel runs at **EL2 (hypervisor mode)** — AIOS never ran an EL2
  kernel; confirm no root-task/guest-layout constraints early (guests + root task stay EL0/EL1; the
  fault trap loop is unchanged, but this is an explicit open item). The signal-frame "LR" is unreachable
  from a fault reply (it maps to ELR_EL1), so budget one extra `WriteRegisters` per delivery for x30=tramp.

---

## 5. Risk register (top items)

| # | Risk | Mitigation |
|---|---|---|
| **R1** | **Stall recurrence on RPi4** — the verified config is *unicore*, and on BCM2711 unicore parks cores 1–3 in the armstub WFE spin-table = **exactly the original 32.4s stall trigger**; the known cure (nodes=4) is *unverified*. On this board you get the verified kernel **or** the stall mitigation, not both. | QEMU dev is immune. Phase H **day-one stall probe** (idle 60s → teardown/TLBI storm). Candidate cure: an armstub clock-keepalive (firmware-level, outside the proofs). Fallbacks: nodes=4-for-daily + unicore-for-claims, or pivot HW to RPi5. A *scheduled experiment*, never a surprise. |
| **R2** | MCS/verification mismatch (cleaner MCS fault model unverified on AArch64 until ~Q3/27) | Build non-MCS master (verified today, matches 0.4.x reuse); the reply-token abstraction bounds a future MCS migration. |
| **R5** | **single-threaded kernel × blocking seL4 IPC** — one blocking `seL4_Call` to a parked server freezes the whole OS | Architectural rule: the kernel blocks in **exactly one place** (the `seL4_Recv` in `pal_guest_next`; timeout = one-shot timer notification). fs + net run **in-proc**. If ever de-monolithized, servers must be bounded-time request/reply + readiness notifications — the 0.4.x `SaveCaller`-parks-the-caller protocol is **banned** for kernel-facing servers. |
| **R4** | effort blowup (10 phases, two L-phases) | every phase ends runnable+committed+gated; tripwires (Phase C >4 sessions → drop COW, ship eager copy; Phase F → client-net first). The 0.4.x parts bin de-risks B/C. |
| R3 | QEMU-vs-HW divergence (SLIRP is lossless — no TCP loss modeling) | plat-split driver seam (proven 0.4.x); util_libs ltimer; TCP-loss validation explicitly deferred to Phase H real-LAN. |
| R6 | debug-kernel hazard (`x7=SysDebugHalt` halts the machine) | red-team + verified claims on **release** builds only; dev-on-debug is fine (guests are ours). |
| R7 | fault-reply semantic traps (empty reply → svc re-exec loop; x30 unreachable from reply; Yield-with-garbage-x7 silent no-op) | Phase 0 x7 pin kills the silent case; corpus register maps are the Phase B/E review checklist; a register-echo test guest. |
| R8/R9 | RPi5 pioneer tier (merged to master 2026-07-07, no release/CI, 2GB-DTS, *is* the Linux prod box); `deps/kernel` external symlink | default RPi4 (§7); re-pin stock 15.0.0 in Phase A. |

---

## 6. What we would NOT pay for (bounded, deliberately deferred)

- **x86-64 / RISC-V / ARMv7** — no code or guest reuse; RISC-V recorded as the deepest-verification
  *endgame* option (compiler-out-of-TCB) behind a bounded libaios asm port, if ever required.
- **MCS**, **SMP** — no verified AArch64 path yet (~Q3/27 / ~Q3/28).
- **COW fork** (eager copy ships first), **ext2 persistence** (RAM-fs is enough for parity), **server
  de-monolithization** (in-proc first) — all post-parity backlog.

---

## 7. Decisions for Bryan

1. **[Blocking — before Phase 0] Ratify the target: AArch64 / non-MCS / seL4 15.0.0 / unicore, dev on
   qemu-arm-virt.** This **revises the pivot's "verified seL4 (x86-64)" wording** — x86-64 is the
   weakest verified config with zero reuse; AArch64 has FC+integrity and runs the existing guests. All
   phasing above assumes this.
2. **[Blocking by end of Phase G] Hardware board: resurrect the RPi4 (default) vs pioneer the RPi5.**
   RPi4 is in-tree verified but stall-tainted under the unicore shape (R1); RPi5 is pioneer-tier and is
   currently your AIOS-on-Linux production host (dual-role conflict). Not needed until families A–C work
   on QEMU — deferred, not dodged.
3. **[Non-blocking] Scope of the *first* deliverable.** The natural first commit is **Phase 0 alone**
   (the x7 pin — a real, self-contained ABI hardening on the Linux line, valuable even if the seL4 port
   pauses). Then Phase A stands up the workspace. Confirm you want me to proceed phase-by-phase with the
   usual gate+review discipline, or to stop at the plan.

**The seam is proved. The target is chosen. The first step (x7) is a half-session on the Linux line.**
