/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_aios.bpf.c -- AIOS's own scheduling policy, as a sched_ext BPF program.
 *
 * The design-doc Phase 2 ("AIOS owns scheduling"): AIOS is a userspace kernel over a commodity host,
 * and here it authors the HOST's CPU scheduler. This is a sched_ext (SCX) scheduler -- a BPF program
 * loaded into the kernel's sched_ext class -- so while it is attached, the kernel schedules tasks by
 * AIOS's policy, not the default EEVDF/CFS. (It governs the whole host; AIOS-specialised policy -- e.g.
 * prioritising the AIOS kernel + its guests -- is a later refinement.)
 *
 * This first policy is deliberately minimal + obviously-correct: a single global dispatch queue served
 * FIFO. Every runnable task is inserted onto one shared DSQ with a default time slice; each CPU, when
 * it needs work, moves the head of that DSQ to its local queue. Round-robin fairness falls out of the
 * slice + FIFO ordering. It is the sched_ext analogue of the "simplest thing that is a real scheduler".
 *
 * Targets the kernel-7.0 SCX API (scx_bpf_dsq_insert / scx_bpf_dsq_move_to_local), confirmed from the
 * RPi5's own BTF. Self-contained: it declares the handful of SCX kfuncs it uses rather than vendoring
 * the full scx/common.bpf.h -- in the spirit of growing our own (like libaios).
 */
#include "vmlinux.h"          /* generated from the target kernel's BTF (bpftool btf dump) */
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char _license[] SEC("license") = "GPL";

#define AIOS_DSQ  0           /* our single shared dispatch queue id */

/* SCX kfuncs (kernel 7.0). Declared here so we need no external scx header. */
extern s32  scx_bpf_create_dsq(u64 dsq_id, s32 node) __ksym;
extern void scx_bpf_dsq_insert(struct task_struct *p, u64 dsq_id, u64 slice, u64 enq_flags) __ksym;
extern bool scx_bpf_dsq_move_to_local(u64 dsq_id) __ksym;

/* .init runs once at attach: create the shared DSQ. It is sleepable (struct_ops.s). */
SEC("struct_ops.s/aios_init")
s32 BPF_PROG(aios_init)
{
	return scx_bpf_create_dsq(AIOS_DSQ, -1);
}

/* .enqueue: a task became runnable -> put it on the shared DSQ with a default slice (FIFO order). */
SEC("struct_ops/aios_enqueue")
void BPF_PROG(aios_enqueue, struct task_struct *p, u64 enq_flags)
{
	scx_bpf_dsq_insert(p, AIOS_DSQ, SCX_SLICE_DFL, enq_flags);
}

/* .dispatch: a CPU needs work -> pull the head of the shared DSQ to this CPU's local queue. */
SEC("struct_ops/aios_dispatch")
void BPF_PROG(aios_dispatch, s32 cpu, struct task_struct *prev)
{
	scx_bpf_dsq_move_to_local(AIOS_DSQ);
}

/* .exit: the scheduler is being unloaded (or the kernel ejected it). Nothing to tear down. */
SEC("struct_ops/aios_exit")
void BPF_PROG(aios_exit, struct scx_exit_info *ei)
{
}

SEC(".struct_ops.link")
struct sched_ext_ops aios_ops = {
	.init     = (void *)aios_init,
	.enqueue  = (void *)aios_enqueue,
	.dispatch = (void *)aios_dispatch,
	.exit     = (void *)aios_exit,
	.flags    = 0,
	.name     = "aios",
};
