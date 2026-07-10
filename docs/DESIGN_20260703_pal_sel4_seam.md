# DESIGN — the seL4 PAL seam (M6): proving the boundary + the proof obligations for a real port

**Date:** 2026-07-03 (session 28) · **Status:** the *scaffold* is done and gated; a real seL4 port is
a separate, later epic (scope settled with Bryan — this session delivers the scaffold, not the port).

**Companion artifact:** `uk/pal/pal_sel4.c` — the scaffold itself. Each stub there states its *local*
proof obligation inline; this doc states the *cross-cutting* ones and the infrastructure a real port
needs. Read the two together.

> **UPDATE (session 28, 2026-07-10) — the real port is now SCOPED, and two claims below are revised by
> `docs/PLAN_20260709_sel4_real_port.md`** (which supersedes this doc's §7 target discussion):
> (1) **Target = AArch64, ratified** (not x86-64): AArch64 completed full functional-correctness
> verification incl. fastpath (Apr 2024) + integrity/availability (Apr 2025), so it is now a *stronger*
> verified config than x86-64 (which is FC-to-C only) — and it runs the existing aarch64 guests. The §1
> and §7 "verified x86-64" framing is superseded.
> (2) **The seL4 SDK is largely already in-repo** — `deps/` pins seL4 kernel 15.0.0-dev +
> seL4_libs/sel4runtime/musl/tools/util_libs — so §7's "infrastructure not yet stood up / SDK … none
> installed" was too pessimistic. See the PLAN doc for the target matrix, the 10-phase plan, and the
> risk register. The proof-obligation *content* below (the three families, the trap model) stands.

---

## 1. Why this milestone exists

The 2026-06-24 pivot re-based AIOS as a **gVisor-style userspace kernel**: AIOS programs compile for the
**AIOS ABI** and never see the host; their syscalls are *trapped* and serviced by the AIOS userspace
kernel, which reaches the host only through a narrow **PAL** (`uk/include/pal.h`). Linux is the *interim*
substrate; the destination is **verified seL4** — *"verification is the soul."* The PAL seam is the
future **verified boundary**: the smaller it is, the smaller the eventual proof obligation.

For that endgame to be real, one property must hold and be *checkable*:

> **`kernel/aios_kernel.c` is genuinely host-agnostic** — it depends on *nothing* Linux-specific; it
> reaches the host **only** through `pal.h`.

M9 (the seccomp backend) rehearsed the proof with a *second Linux* trap mechanism. **M6 carries it to a
third, _non-Linux_ backend** — the one that matters — and makes the property fail loudly if it is ever
violated:

- `make PAL=sel4` compiles `kernel/aios_kernel.c` + `pal/pal_sel4.c` and **links** them into `aios-uk`
  **without** `pal_linux_common.c` and without any Linux-PAL symbol. A single leaked dependency — a stray
  `ptrace`, a symbol reached around the seam — would make the **link fail**. It links.
- Empirically it links **on both the Linux container and macOS/arm64** — a wholly different OS from the
  target. The kernel needs nothing a generic C environment lacks; the *only* file that ever knew about
  the host was the PAL, exactly as the seam intends.

So this milestone's deliverable is **a compiling third backend + this proof-obligation document**. It
does **not** run seL4 (see §7 for what that would take). The scaffold's `pal_guest_spawn` announces
itself and refuses to run guests, so the proof binary is honest about being a proof binary.

The `gate.sh` acceptance gate now runs a **third pass**: `PAL=sel4` must build/link, and running it must
announce the scaffold and refuse to spawn (non-zero) — `RESULT: linux=0 seccomp=0 sel4=0`.

---

## 2. The one structural difference: tracer (Linux) vs. root task (seL4)

On Linux the AIOS kernel is a **tracer**. It traps guest syscalls with `ptrace`, reads/writes guest
memory with `process_vm_readv`/`POKE`, injects `mmap`/`clone`/`execve` into the stopped tracee, and
rewrites registers with `PTRACE_*REGSET`. Every one of those is a *host-kernel service*.

seL4 offers **none** of them. There is no `ptrace`, no `mmap`, no host process model, no filesystem, no
network stack, no clock, no terminal *in the kernel*. A real `pal_sel4.c` is instead a seL4 **root task**
holding **capabilities**, and the PAL primitives become **seL4 object invocations**. They fall into three
families — the recurring structure in `pal_sel4.c` and in the obligations below:

| Family | On Linux (today) | On seL4 (the obligation) |
|---|---|---|
| **A. Guest world** — trap, registers, guest memory | ptrace stop; `process_vm_readv`; `PTRACE_*REGSET` | a guest is a **TCB** in its own **VSpace**; its AIOS `svc` **faults** to the AIOS kernel's fault endpoint; memory via the kernel's own mapping of the guest's **Frame** caps; registers via `seL4_TCB_{Read,Write}Registers` |
| **B. Loader/pager** — mmap, fork, exec, exit | injected `mmap`/`clone`/`execve`/`exit_group` | **retype Untyped → Frames** and map them; **copy/CoW the VSpace**; the kernel **is the ELF loader**; **suspend + revoke** to exit |
| **C. Host driver** — files, dirs, sockets, clock, tty | direct host syscalls | an **IPC (`seL4_Call`) to a userspace SERVER** (fs / net / timer / console) the kernel holds an **endpoint cap** to |

The rest of this doc is those three families in detail, then confinement, verification, and the real
port's infrastructure.

---

## 3. Family A — the guest world (the trap model)

### 3.1 How a guest traps without ptrace

A guest is a native seL4 **thread** (a `TCB`) running in its own **VSpace**, configured (`seL4_TCB_Configure`)
with a **fault endpoint** pointing at the AIOS kernel. On aarch64/x86-64, a userspace thread that executes
an `svc`/`syscall` carrying a number seL4 does not recognise raises an **unknown-syscall fault**, delivered
to that fault endpoint as an IPC message **carrying the thread's registers**. AIOS numbers its syscalls
`≥ 0x1000` (and, for the seccomp backend, funnels through a gateway) precisely so they are *not* host
syscalls — on seL4 that same disjoint numbering means every AIOS `svc` is an unknown-syscall fault.

So the trap model is:

- **`pal_guest_next`** = `seL4_Recv` on the kernel's fault + notification endpoint set. One receive
  demultiplexes *all* of `pal_guest_next`'s return codes: a guest fault → code 1 (an AIOS syscall; the
  fault message already carries the registers, so `sc` is filled with no extra round-trip); a child-exit
  bookkeeping notification → code 0; a server notification (socket-ready / timer / console) → code 4 / 3.
  This is the seL4 analogue of `waitpid()`+`ppoll()` as **one** blocking wait over a unified event source
  — exactly the shape `pal.h` already imposes ("wait for any guest, service one").
- **`pal_guest_return`** = `seL4_Reply` to the fault with the result register set (replying to a fault
  *resumes* the faulter). **`pal_guest_setret`** = write the result register but **do not** reply/resume
  (the kernel interposes signal delivery first). **`pal_guest_resume`** = reply/`seL4_TCB_Resume`.

**Why not a VMM?** seL4 also supports full virtualization (`seL4_ARM_VCPU` on aarch64 hyp; VT-x/EPT on
x86-64): a guest runs in a VM and its EL1/ring-3 syscalls VMExit to a VMM. That is heavier (a vCPU, a
guest-physical address space, instruction/exit decoding) and is only *needed* when the guest may execute
arbitrary privileged instructions. **AIOS guests never do** — they are AIOS-ABI programs that only ever
emit AIOS `svc` + ordinary userspace instructions — so the **fault-handler model above is sufficient and
far lighter**, and it keeps the guest a first-class seL4 thread (directly schedulable by scx-equivalent
policy, directly confinable by its CSpace). **Recommendation: the fault-handler model; keep the VMM in
reserve** for a future "run an unmodified Linux binary" goal that is explicitly out of AIOS's scope.

### 3.2 Guest memory without `process_vm_readv`

`pal_guest_read`/`write` copy bytes in/out of a guest — including into a *different* guest than the one
trapped (a `wait` status into a parked parent). The AIOS kernel **minted every Frame** in every guest's
VSpace (family B), so it **retains the Frame caps** and can map any guest frame into its **own** VSpace on
demand. Guest memory access is then a plain `memcpy` through the kernel's scratch mapping of the frame
backing that guest page. **Invariant:** the kernel maps only frames it owns for that guest → no
cross-guest or kernel/guest aliasing. This is the clean seL4 answer to "there is no `process_vm_readv`."

### 3.3 Registers + signal delivery

`pal_guest_setret`/`deliver`/`sigreturn` manipulate the guest's registers — host-specific, so `pal.h`
puts them in the PAL. On seL4: `seL4_TCB_{Read,Write}Registers` on the guest's TCB cap. Delivery reads the
guest's context into the opaque `savebuf` (which now holds a `seL4_UserContext`, not a Linux
`user_regs_struct` — but the kernel never inspects it, so `PAL_SIGSAVE_SIZE` opaque bytes still fits),
writes `pc=handler / arg0=signum / lr=tramp`, resumes; the trampoline's `SIGRETURN` faults back and the
kernel restores `savebuf`. Any handler stack frame is written via the family-A cap-mapped memory path.

---

## 4. Family B — the loader/pager

| Primitive | seL4 obligation |
|---|---|
| **`pal_guest_mmap`** | `seL4_Untyped_Retype` Untyped → `len/PAGE` Frames; map into the guest VSpace at a free vaddr (retyping intermediate page-table objects). **The Untyped pool is the guest's memory budget** — a natural capability-based quota. Invariant: no alias/overlap with existing regions. |
| **`pal_guest_fork`** | New VSpace + TCB; reproduce the parent's memory — eagerly (copy each frame via the kernel's scratch mappings) or, better, **copy-on-write** (map parent frames read-only into both VSpaces, fault-copy on first write; the kernel is the pager, so the write-fault arrives on the same fault EP). Duplicate the parent's server-facing caps into the child's CSpace; share the fault EP. |
| **`pal_guest_exec`** | No `execve`. The kernel opens the (confined) path via the fs server, parses the ELF, revokes the current VSpace frames, builds a fresh VSpace+stack (as in spawn), and points the existing TCB at it. AIOS fds surviving exec = the guest's server-facing caps are preserved across the image swap. |
| **`pal_guest_exit`** | `seL4_TCB_Suspend`, then `seL4_CNode_Revoke`/`Delete` the TCB/VSpace/page-tables/Frames back to Untyped — the memory returns to the pool it came from. The "exit" the kernel then observes is its own bookkeeping notification, since it initiated teardown. |
| **`pal_guest_spawn`** | The root-task bring-up: VSpace + ELF load + initial stack + TCB (with CSpace, IPC buffer, **fault EP**) + set PC/SP + resume. Invariant: the guest's CSpace/VSpace name **only** objects AIOS minted for it — the root of every confinement guarantee. |

---

## 5. Family C — the host driver is userspace servers

There is no fs/net/clock/tty in the seL4 kernel. The AIOS kernel holds **endpoint caps** to userspace
**servers**, and each `pal_host_*` becomes a `seL4_Call` marshalling arguments into the IPC message +
shared buffers; the `pal_file_t` backings become per-connection caps / server handles. Concretely:

- **Files/dirs** (`open`/`read`/`write`/`close`/`lseek`/`fstat`/`getdents`/`stat`/`unlink`/`mkdir`/…/the
  `*at` family/metadata) → an **fs server**.
- **Sockets** (`socket`/`connect`/`bind`/`listen`/`accept`/`setsockopt`/`getsockname`/`sock_error`/
  `sock_writable`) → a **net server**. The park/wake seam (`pal_net_watch_*`, `pal_net_wait_ready`)
  becomes net-server **readiness notifications** folded into the same `seL4_Recv` as `pal_guest_next` —
  the co-wait is structural, not a `ppoll`.
- **Clock** (`clock_gettime`) → a **timer source** (a timer driver / MCS time / a shared read-only time
  page; REALTIME = MONOTONIC + a boot offset, as AIOS already did with SNTP-at-boot on the Pi — seL4 has
  no in-kernel RTC either).
- **Terminal** (`isatty`/`tcgetattr`/`tcsetattr`, and the `pal_take_term_signal` ^C/^Z path) → a
  **console/serial server** that owns the line discipline and notifies the kernel on the interrupt char.

The AIOS error codes `pal.h` mandates are host-agnostic (`AIOS_E*`); a server maps its own failures onto
the same set — the kernel is unchanged.

---

## 6. Confinement becomes *structural* (the elegant part)

Today's confinement is a set of PAL *policies* enforced with Linux tricks: `AIOS_ROOT` via
`openat2(RESOLVE_IN_ROOT)` (M4.2), exec via an `openat2`+`/proc/self/fd` canonicalisation (M4.3),
`AIOS_NET_ALLOW`/`AIOS_NET_BIND_ALLOW` via in-PAL rule matching. `pal.h` deliberately keeps all of this
*inside* the PAL — the kernel and ABI never change — precisely so a different backend can enforce it
differently.

On seL4 it stops being a *check* and becomes **capability confinement**:

- A confined guest's kernel simply holds a **cap to the AIOS-root subtree** (not the whole fs), so an
  out-of-root path is **unnameable** — there is nothing to clamp, no symlink to resolve away (the M4.2
  chmod-through-a-symlink subtlety evaporates: the guest's fs cap can only name in-subtree objects).
- Network reach/claim is an **allow-list-parameterised net cap**: a disallowed peer or bind is refused by
  the server the guest's kernel was *given*, not by a string match.
- The M4.3 "init is the trusted entry, everything it spawns is jailed" model is literally "the root task
  holds broad caps; the caps it hands each guest are the narrowed subset."

This is why the design keeps confinement in the PAL: it is the same *guarantee* with a stronger, verifiable
*mechanism* on the destination.

---

## 7. What a real seL4 port needs (the infrastructure — the option-B epic)

The scaffold deliberately stops here. A working port is gated on infrastructure not yet stood up; the
decisions below are the ones to make **before** sinking multi-session effort:

1. **Target + verification story (a real decision, stated honestly).** The pivot names **x86-64** as the
   verified destination. Note seL4's *completed* functional-correctness + binary-level proofs are for
   **ARM (ARMv7, 32-bit)** and **RISC-V (RV64)**; **x86-64 is supported but its verification is not
   complete**, and it has no verified boot/DMA story. So "verified seL4 on x86-64" is partly aspirational
   today. The target choice trades: x86-64 (commodity dev hardware, matches the pivot's wording, weaker
   *current* proofs) vs. a verified ARM/RISC-V target (stronger proofs, different hardware). **Pick this
   first** — it drives everything below.
2. **seL4 SDK + toolchain.** `seL4`, `seL4_libs`/`sel4runtime` (or CAmkES / the Rust `rust-sel4`), a
   cross toolchain for the target, and QEMU for the target to develop without hardware. None installed.
3. **The root task = the AIOS kernel re-hosted.** `kernel/aios_kernel.c` stays *as-is* (that is the whole
   point); the work is a *real* `pal_sel4.c` that boots as the root task, manages the Untyped pool, and
   implements families A/B/C against the SDK.
4. **Trap model:** the fault-handler design of §3 (recommended) — no VMM. Budget the fault-message
   register marshalling + the guest-frame scratch-mapping allocator up front (they underpin families A+B).
5. **The three servers (family C).** An fs server (backed by an initrd/ramdisk or a real disk driver), a
   net server (a userspace TCP/IP stack, e.g. lwIP, over a NIC driver), a timer + console server. These
   are the bulk of the effort — the AIOS kernel is *ready*; the *world it talks to* must be built. Reuse
   is possible (see `docs/DR_20260623_linux_driver_reuse_on_sel4.md` on driver reuse).
6. **The initial guest image.** The existing aarch64 AIOS userland (dash + sbase) is fine if the target is
   aarch64; an x86-64 target needs the userland recompiled for x86-64 (the ABI is host-agnostic, so this
   is a rebuild, not a rewrite — but it is real work, and libaios has a little aarch64 asm: `_start`, the
   syscall stubs, `setjmp`/`sigsetjmp`, the sigreturn trampoline).

A sensible **phasing** for the real port: (a) root task boots + a debug-console `write` (the scaffold's
one live primitive, made real); (b) family A on a single hand-built guest (fault → service one AIOS
syscall → reply); (c) family B (mmap/fork/exec/exit) → the process model runs; (d) an fs server → dash +
sbase run; (e) net/timer/console servers → parity with the Linux backend; (f) confinement as caps; (g)
the verification work against the seam. Each phase is independently demonstrable — the same discipline the
Linux line used.

---

## 8. Scope + status (honest)

- **Done (this session):** `pal/pal_sel4.c` — the full `pal.h` contract as documented stubs; `make
  PAL=sel4` links the host-agnostic kernel against a third, non-Linux PAL; a `gate.sh` third pass proves
  it (`sel4=0`) alongside the two Linux backends; this document.
- **Not done (by design):** anything that *runs* on seL4. No SDK, no target, no servers, no VMM — those
  are the option-B epic scoped in §7.
- **The kernel banner** still reads "on Linux (ptrace PAL)" under `PAL=sel4`: that is the kernel's
  *cosmetic* default string. `kernel/aios_kernel.c` is **identical across all three backends** — that
  identity *is* the portability proof — so its banner names the default (ptrace) backend whichever PAL is
  linked; the `[pal_sel4]` notice printed at startup corrects it unmistakably. Naming the
  live backend in the kernel banner would require a new `pal.h` primitive (a `pal_backend_name()`) — an
  added proof obligation for a cosmetic, deliberately declined.

**The seam is proved. The soul is scoped.**
