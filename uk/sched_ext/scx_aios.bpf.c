/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_aios.bpf.c -- AIOS's own scheduling policy, as a sched_ext BPF program.
 *
 * The design-doc Phase 2 ("AIOS owns scheduling"): AIOS is a userspace kernel over a commodity host,
 * and here it authors the HOST's CPU scheduler. This is a sched_ext (SCX) scheduler -- a BPF program
 * loaded into the kernel's sched_ext class -- so while it is attached, the kernel schedules tasks by
 * AIOS's policy, not the default EEVDF/CFS. It governs the WHOLE host.
 *
 * This policy is AIOS-AWARE: because AIOS is a userspace kernel, the host's CPU scheduler should FAVOUR
 * the AIOS workload (the aios-uk kernel process + every guest it runs) over unrelated host tasks. It
 * does that in two steps:
 *
 *   (1) TAG AIOS tasks by comm-ancestry. The AIOS kernel process runs as comm "aios-uk"; every guest is
 *       one of its fork descendants (aios-uk forks+execs the init guest; guests fork further). So a task
 *       is "AIOS" if it -- or an ancestor within a bounded number of real_parent hops -- has comm
 *       "aios-uk". We read comm + walk real_parent with CO-RE (BPF_CORE_READ), in a fully-unrolled,
 *       bounded loop so the verifier can prove termination.
 *
 *   (2) PRIORITISE via two dispatch queues. AIOS tasks go on a HIGH DSQ, everything else on a NORMAL
 *       DSQ; dispatch drains HIGH first, then NORMAL. Both keep the default time slice. This is STRICT
 *       priority, deliberately -- and it does NOT starve ordinary host tasks (e.g. sshd) in practice:
 *       AIOS is bursty and blocks constantly (aios-uk sits in waitpid/ppoll; guests block on I/O, pipes,
 *       ptrace stops), so the HIGH DSQ drains to empty frequently and NORMAL gets served. And on an
 *       SMP box a single CPU-bound AIOS task occupies one CPU while the others (finding HIGH empty)
 *       serve NORMAL. Only if AIOS genuinely saturates every CPU would NORMAL wait -- which is the
 *       intent. A weighted/budgeted policy (vtime, a NORMAL-starvation valve) is a later refinement;
 *       this is the honest, obviously-correct first AIOS-aware policy. (See README for the analysis.)
 *
 * OBSERVABLE: four global counters (mmap'd .bss) let the userspace loader SHOW the policy at work --
 * how many enqueues were tagged AIOS vs other, and how many dispatch cycles drained HIGH vs NORMAL.
 *
 * Targets the kernel-7.0 SCX API (scx_bpf_dsq_insert / scx_bpf_dsq_move_to_local), confirmed from the
 * RPi5's own BTF. Self-contained: it declares the handful of SCX kfuncs it uses rather than vendoring
 * the full scx/common.bpf.h -- in the spirit of growing our own (like libaios).
 */
#include "vmlinux.h"          /* generated from the target kernel's BTF (bpftool btf dump) */
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

char _license[] SEC("license") = "GPL";

#define AIOS_DSQ_HI      0    /* AIOS tasks (aios-uk + its guests): drained first */
#define AIOS_DSQ_NORMAL  1    /* every other host task */

#define AIOS_COMM_LEN     16  /* task_struct.comm is char[16] (TASK_COMM_LEN) */
#define AIOS_ANCESTRY_HOPS 8  /* how far up the real_parent chain to look for "aios-uk" */

/* Observability: bumped atomically, read by the loader via skel->bss (see scx_aios.c). */
__u64 aios_enq;      /* enqueues classified as AIOS (kernel + guests) */
__u64 other_enq;     /* enqueues classified as non-AIOS */
__u64 hi_dispatch;   /* dispatch cycles that drained a task from the HIGH DSQ */
__u64 norm_dispatch; /* dispatch cycles that drained a task from the NORMAL DSQ */

/* SCX kfuncs (kernel 7.0). Declared here so we need no external scx header. */
extern s32  scx_bpf_create_dsq(u64 dsq_id, s32 node) __ksym;
extern void scx_bpf_dsq_insert(struct task_struct *p, u64 dsq_id, u64 slice, u64 enq_flags) __ksym;
extern bool scx_bpf_dsq_move_to_local(u64 dsq_id) __ksym;

/* comm == "aios-uk" (7 chars + NUL). Compare the exact bytes; the trailing NUL makes it exact. */
static __always_inline int comm_is_aios(const char *c)
{
	return c[0] == 'a' && c[1] == 'i' && c[2] == 'o' && c[3] == 's' &&
	       c[4] == '-' && c[5] == 'u' && c[6] == 'k' && c[7] == '\0';
}

/*
 * Is p part of the AIOS workload? True if p or an ancestor within AIOS_ANCESTRY_HOPS real_parent hops
 * has comm "aios-uk". Fully unrolled + bounded, all reads via CO-RE (safe on any layout / can't fault),
 * with a self-parent early-out (the swapper/init root points at itself) -- verifier-friendly.
 */
static __always_inline int task_is_aios(struct task_struct *p)
{
	struct task_struct *t = p;
	char comm[AIOS_COMM_LEN];
	int i;

#pragma unroll
	for (i = 0; i < AIOS_ANCESTRY_HOPS; i++) {
		struct task_struct *parent;

		if (!t)
			break;

		__builtin_memset(comm, 0, sizeof(comm));
		BPF_CORE_READ_STR_INTO(&comm, t, comm);
		if (comm_is_aios(comm))
			return 1;

		parent = BPF_CORE_READ(t, real_parent);
		if (!parent || parent == t)   /* reached the root of the process tree */
			break;
		t = parent;
	}
	return 0;
}

/* .init runs once at attach: create both dispatch queues. It is sleepable (struct_ops.s). */
SEC("struct_ops.s/aios_init")
s32 BPF_PROG(aios_init)
{
	s32 err;

	err = scx_bpf_create_dsq(AIOS_DSQ_HI, -1);
	if (err)
		return err;
	return scx_bpf_create_dsq(AIOS_DSQ_NORMAL, -1);
}

/* .enqueue: a task became runnable -> HIGH DSQ if it is AIOS, else NORMAL (both FIFO, default slice). */
SEC("struct_ops/aios_enqueue")
void BPF_PROG(aios_enqueue, struct task_struct *p, u64 enq_flags)
{
	if (task_is_aios(p)) {
		__sync_fetch_and_add(&aios_enq, 1);
		scx_bpf_dsq_insert(p, AIOS_DSQ_HI, SCX_SLICE_DFL, enq_flags);
	} else {
		__sync_fetch_and_add(&other_enq, 1);
		scx_bpf_dsq_insert(p, AIOS_DSQ_NORMAL, SCX_SLICE_DFL, enq_flags);
	}
}

/* .dispatch: a CPU needs work -> drain the HIGH DSQ first; only if it is empty, drain NORMAL. */
SEC("struct_ops/aios_dispatch")
void BPF_PROG(aios_dispatch, s32 cpu, struct task_struct *prev)
{
	if (scx_bpf_dsq_move_to_local(AIOS_DSQ_HI)) {
		__sync_fetch_and_add(&hi_dispatch, 1);
		return;
	}
	if (scx_bpf_dsq_move_to_local(AIOS_DSQ_NORMAL))
		__sync_fetch_and_add(&norm_dispatch, 1);
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
