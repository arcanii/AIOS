# scx_aios — AIOS owns scheduling (an AIOS-aware sched_ext BPF scheduler)

The design doc's **Phase 2**: AIOS is a userspace kernel over a commodity host, and here it authors the
**host's CPU scheduler**. `scx_aios` is a [sched_ext](https://docs.kernel.org/scheduler/sched-ext.html)
(SCX) scheduler — a BPF program loaded into the kernel's `sched_ext` class — so while it is attached the
kernel schedules every task by **AIOS's policy**, not the default EEVDF/CFS.

## The policy: prioritise the AIOS workload

Because AIOS *is* a userspace kernel, the host's CPU scheduler should **favour the AIOS workload** — the
`aios-uk` kernel process and every guest it runs — over unrelated host tasks. `scx_aios` does that in
three steps (see `scx_aios.bpf.c`):

1. **Tag AIOS tasks by comm-ancestry.** The AIOS kernel process runs as comm `aios-uk`; every guest is
   one of its `fork` descendants (`aios-uk` forks+execs the init guest; guests fork further). So a task
   is *AIOS* if it — or an ancestor within **8 `real_parent` hops** — has comm `aios-uk`. We read `comm`
   and walk `real_parent` with **CO-RE** (`BPF_CORE_READ` / `BPF_CORE_READ_STR_INTO`) in a fully-unrolled,
   bounded loop, so the verifier can prove termination and every read is fault-safe (`bpf_probe_read`).
2. **Prioritise via two dispatch queues.** AIOS tasks are enqueued onto a **HIGH** DSQ; everything else
   onto a **NORMAL** DSQ. `dispatch` drains HIGH first, then NORMAL — **strict priority**, both with the
   default time slice.
3. **Make it observable.** Four global counters (mmap'd `.bss`, read live by the loader) show the policy
   at work: `aios_enq` / `other_enq` (enqueues tagged AIOS vs not) and `hi_dispatch` / `norm_dispatch`
   (dispatch cycles that drained HIGH vs NORMAL).

### Does strict priority starve everything else? — no, in practice

The strict-priority choice is deliberate, and it does **not** starve ordinary host tasks (e.g. sshd) in
practice, for two reasons:

- **AIOS blocks constantly.** `aios-uk` sits in `waitpid`/`ppoll` waiting for guest events and socket
  readiness; guests block on I/O, pipes, waits, and ptrace stops. So the HIGH DSQ drains to empty
  frequently, and NORMAL is served whenever it does.
- **SMP.** On the 4-CPU RPi5, a single CPU-bound AIOS task occupies one CPU; the other CPUs, finding HIGH
  empty, serve NORMAL. `scx_bpf_dsq_move_to_local` also skips tasks that can't run on the calling CPU, so
  per-CPU kthreads in NORMAL are still dispatched correctly.

Only if AIOS genuinely saturates **every** CPU would NORMAL wait — which is the intent (AIOS is the
workload the box exists to run). The empirical proof: the full AIOS acceptance gate passes **both** PAL
backends while this scheduler owns the host, and an interactive ssh session stays responsive throughout.

**Honest limits** (documented, not hidden — all in the *under*-prioritise / safe direction):

- The **8-hop cap** means an AIOS task buried more than 8 `real_parent` hops below `aios-uk` (a
  pathologically deep nest of subshells) is tagged NORMAL — a priority *miss*, never a safety bug.
- An **orphaned** AIOS guest (whose guest-parent exited so it was reparented toward host pid 1) can lose
  the `aios-uk` ancestry and fall to NORMAL — again a priority miss, not a correctness bug.
- A non-AIOS task is only ever tagged AIOS if it truly has an `aios-uk` ancestor (i.e. it *is* part of
  the AIOS workload). The one way to fake it is a host process that sets its own comm to `aios-uk` via
  `prctl` — a cooperative-scheduler gaming concern, out of scope here.

A weighted/budgeted policy (vtime fairness, a NORMAL-starvation valve, per-guest weights) is the natural
**later refinement**; this is the honest, obviously-correct *first* AIOS-aware policy.

## Files

- `scx_aios.bpf.c` — the BPF scheduler: `struct sched_ext_ops` with `init`/`enqueue`/`dispatch`/`exit`
  plus the comm-ancestry tagger. Targets the kernel-7.0 SCX API (`scx_bpf_dsq_insert` /
  `scx_bpf_dsq_move_to_local`), self-contained (declares the few SCX kfuncs it uses; no vendored
  `scx/common.bpf.h`).
- `scx_aios.c` — the userspace loader: opens + loads the BPF, attaches the struct_ops (switching the
  kernel to AIOS's policy), prints the counters every 2s, holds until Ctrl-C, then detaches (the kernel
  reverts to its default).
- `Makefile` — build rules.

## Requirements

- **Build:** `clang` (BPF target) + `bpftool` + `libbpf-dev` + `<bpf/bpf_core_read.h>` (CO-RE) + the
  target kernel's `vmlinux.h` (dumped from its BTF). The RPi5's stock Ubuntu-26.04 image ships **only
  gcc** (no clang/bpftool/libbpf-dev), so build in an **`ubuntu:26.04` container** (clang 21 / bpftool 7.7
  / libbpf 1.6.3 — ABI-identical to the RPi5) **against the RPi5's BTF**: copy the RPi5's
  `/sys/kernel/btf/vmlinux` out (it is world-readable) and `make BTF=vmlinux.btf`.
- **Load:** **root** + a kernel with **`CONFIG_SCHED_CLASS_EXT=y`**. The RPi5's stock Ubuntu-26.04 kernel
  (7.0) has it (`/sys/kernel/sched_ext/`); the RPi4's stock kernel does **not**. colima's kernel (6.8)
  predates sched_ext, so it cannot even load-test this.

## Build + run

Native on a box that has the toolchain:

```sh
make                 # generates vmlinux.h from /sys/kernel/btf/vmlinux, builds scx_aios
sudo ./scx_aios      # attach the AIOS scheduler (prints counters every 2s; Ctrl-C to detach)
```

Cross-build in a container against the RPi5's BTF, then copy the self-contained loader over and run it:

```sh
# on the RPi5: the BTF is world-readable, so just copy it out
scp aios@tkrpi5.local:/sys/kernel/btf/vmlinux vmlinux.btf
# in an ubuntu:26.04 (arm64) container with clang/bpftool/libbpf-dev installed:
make BTF=vmlinux.btf
# copy the binary to the RPi5 and run as root:
scp scx_aios aios@tkrpi5.local:~/ ; ssh aios@tkrpi5.local 'sudo ./scx_aios'
# confirm from another shell:
cat /sys/kernel/sched_ext/root/ops     # -> "aios"
cat /sys/kernel/sched_ext/state        # -> "enabled"
```

The loader binary is self-contained (it embeds the BPF object via the skeleton), so a build produced in
an ABI-matching environment can simply be copied to the target and run as root — no toolchain needed to
*run* it, only libbpf at runtime (the RPi5 has `libbpf.so.1.6.3`).

## Status

**HW-VALIDATED end-to-end on the RPi5** (Ubuntu 26.04, kernel 7.0):

- the BPF **verifier accepts** the comm-ancestry tagger (`nr_rejected` = 0); attach →
  `/sys/kernel/sched_ext/state` = `enabled`, `/sys/kernel/sched_ext/root/ops` = `aios`;
- **the full AIOS gate passes both PAL backends (`linux=0 seccomp=0`) while the AIOS scheduler owns the
  host** — AIOS running correctly on a machine it schedules;
- the policy **tags + prioritises** the AIOS workload: over one gate run `aios_enq` reached ~200 (with
  `hi_dispatch` == `aios_enq` — every AIOS task drained from HIGH) while ~23 800 other host enqueues went
  to NORMAL;
- detach (SIGTERM) → `state` = `disabled`, the kernel reverts to its default scheduler, clean exit.

Built in an `ubuntu:26.04` container (ABI-identical to the RPi5); the self-contained loader binary was
copied over and run as root. "AIOS owns scheduling" (design-doc Phase 2) is real — and now **AIOS-aware**
— on hardware.
