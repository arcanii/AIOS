# scx_aios — AIOS owns scheduling (a sched_ext BPF scheduler)

The design doc's **Phase 2**: AIOS is a userspace kernel over a commodity host, and here it authors the
**host's CPU scheduler**. `scx_aios` is a [sched_ext](https://docs.kernel.org/scheduler/sched-ext.html)
(SCX) scheduler — a BPF program loaded into the kernel's `sched_ext` class — so while it is attached the
kernel schedules every task by **AIOS's policy**, not the default EEVDF/CFS.

This first policy is deliberately minimal and obviously-correct: **a single global dispatch queue served
FIFO** with a default time slice (the sched_ext analogue of "the simplest thing that is a real
scheduler"). AIOS-specialised policy — e.g. prioritising the AIOS kernel and its guests — is a later
refinement.

## Files

- `scx_aios.bpf.c` — the BPF scheduler: `struct sched_ext_ops` with `init`/`enqueue`/`dispatch`/`exit`.
  Targets the kernel-7.0 SCX API (`scx_bpf_dsq_insert` / `scx_bpf_dsq_move_to_local`), self-contained
  (declares the few SCX kfuncs it uses; no vendored `scx/common.bpf.h`).
- `scx_aios.c` — the userspace loader: opens + loads the BPF, attaches the struct_ops (switching the
  kernel to AIOS's policy), holds it until Ctrl-C, then detaches (the kernel reverts to its default).
- `Makefile` — build rules.

## Requirements

- **Build:** `clang` (BPF target) + `bpftool` + `libbpf-dev` + the target kernel's `vmlinux.h` (dumped
  from its BTF). A toolchain matching the target's libbpf is best — the RPi5 (Ubuntu 26.04) has clang 21
  / bpftool 7.7 / libbpf 1.6.3; an `ubuntu:26.04` container matches it exactly for cross-building.
- **Load:** **root** + a kernel with **`CONFIG_SCHED_CLASS_EXT=y`**. The RPi5's stock Ubuntu-26.04 kernel
  (7.0) has it (`/sys/kernel/sched_ext/`); the RPi4's stock kernel does **not**. colima's kernel (6.8)
  predates sched_ext, so it cannot even load-test this.

## Build + run (on the RPi5)

```sh
make                 # generates vmlinux.h from /sys/kernel/btf/vmlinux, builds scx_aios
sudo ./scx_aios      # attach the AIOS scheduler (stays attached until Ctrl-C)
# in another shell, confirm the kernel is running AIOS's scheduler:
cat /sys/kernel/sched_ext/root/ops     # -> "aios"
cat /sys/kernel/sched_ext/state        # -> "enabled"
```

The loader binary is self-contained (it embeds the BPF object via the skeleton), so a build produced in
an ABI-matching environment can simply be copied to the target and run as root — no toolchain needed to
*run* it, only libbpf at runtime.

## Status

Written + **build-verified** against the RPi5's exact kernel-7.0 BTF and libbpf 1.6.3 (compiles, skeleton
generates, loader links). The runtime **load + attach** is validated on the RPi5 (needs root) — see the
session handover for the result.
