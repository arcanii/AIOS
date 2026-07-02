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
 * does that in three layers:
 *
 *   (1) TAG AIOS tasks by comm-ancestry. The AIOS kernel process runs as comm "aios-uk"; every guest is
 *       one of its fork descendants (aios-uk forks+execs the init guest; guests fork further). So a task
 *       is "AIOS" if it -- or an ancestor within a bounded number of real_parent hops -- has comm
 *       "aios-uk". We read comm + walk real_parent with CO-RE (BPF_CORE_READ), in a fully-unrolled,
 *       bounded loop so the verifier can prove termination.
 *
 *   (2) PRIORITISE via two dispatch queues, WITH an anti-starvation valve. AIOS tasks go on a HIGH DSQ,
 *       everything else on a NORMAL DSQ; dispatch normally drains HIGH first, then NORMAL (AIOS wins).
 *       In practice AIOS is bursty and blocks constantly (aios-uk sits in waitpid/ppoll; guests block on
 *       I/O, pipes, ptrace stops), so HIGH drains to empty frequently and NORMAL gets served; on an SMP
 *       box a single CPU-bound AIOS task occupies one CPU while the others serve NORMAL. The ONE case
 *       strict priority could starve a host task (e.g. sshd) is AIOS saturating EVERY CPU -- so we add a
 *       STARVATION VALVE: while HIGH and NORMAL BOTH have runnable work, if NORMAL has gone unserved for
 *       longer than AIOS_NORMAL_STARVE_NS, the next dispatch serves NORMAL first (then resets the timer).
 *       So under sustained saturation the NORMAL group is served at least once per window -- a keep-alive
 *       share that GUARANTEES no NORMAL task is starved (each waits at most ~N*window for N runnable
 *       NORMAL tasks: bounded + finite, never infinite), while AIOS keeps the overwhelming majority of
 *       the CPU. Under ordinary load the valve NEVER fires (behaviour identical to strict priority). It
 *       is a liveness valve for cross-class (HIGH-vs-NORMAL) contention.
 *
 *   (3) Be FAIR + WEIGHTED *within* each class -- virtual-time (vtime) scheduling. Rather than plain
 *       FIFO inside a DSQ, each task carries a per-task virtual time; a DSQ is drained lowest-vtime-first
 *       (scx_bpf_dsq_insert_vtime), and a task's vtime advances by the CPU it uses SCALED BY ITS WEIGHT
 *       (a higher weight advances vtime slower -> more CPU). So among AIOS guests (and among NORMAL
 *       tasks) CPU is shared FAIRLY -- no guest starves another -- and PER-GUEST WEIGHTS work: the
 *       ordinary `nice` value sets p->scx.weight, so `nice`-ing a guest (inherited across fork) makes it
 *       get proportionally more/less of the AIOS CPU budget. Layer (2) still decides HIGH-vs-NORMAL;
 *       layer (3) decides who-within-a-class. This is the "finer weighted/vtime policy" (2) foretold.
 *
 * OBSERVABLE: five global counters (mmap'd .bss) let the userspace loader SHOW the policy at work -- how
 * many enqueues were tagged AIOS vs other, how many dispatch cycles drained HIGH vs NORMAL, and how many
 * times the anti-starvation valve fired (served NORMAL under HIGH pressure).
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

/* The anti-starvation valve threshold. Under sustained HIGH-vs-NORMAL contention, the NORMAL DSQ is
 * served (its head task dispatched) at least once every ~this long -- so NORMAL, as a group, gets a
 * keep-alive share of the CPU and can never be starved. An INDIVIDUAL NORMAL task's worst-case wait is
 * ~(runnable NORMAL tasks) x this (FIFO cycling): bounded + finite, never infinite. 50ms yields NORMAL a
 * small aggregate share while strongly favouring AIOS; smaller = lower NORMAL latency + more AIOS
 * dilution. This is a starvation VALVE (a liveness guarantee), not a fair-share scheduler. */
#define AIOS_NORMAL_STARVE_NS (50ULL * 1000000ULL)

/* Observability: bumped atomically, read by the loader via skel->bss (see scx_aios.c). */
__u64 aios_enq;      /* enqueues classified as AIOS (kernel + guests) */
__u64 other_enq;     /* enqueues classified as non-AIOS */
__u64 hi_dispatch;   /* dispatch cycles that drained a task from the HIGH DSQ */
__u64 norm_dispatch; /* dispatch cycles that drained a task from the NORMAL DSQ */
__u64 valve_fires;   /* times the anti-starvation valve served NORMAL ahead of a non-empty HIGH */

/* last_norm_ns: monotonic time (ns) NORMAL was last dispatched; drives the valve. 0 until first set. */
static __u64 last_norm_ns;

/* vtime_now: the global virtual-time clock (max vtime of any task that has run) -- the reference the
 * enqueue clamp uses so a long-sleeping task cannot bank unlimited vtime credit and then monopolise. */
static __u64 vtime_now;

/* WEIGHT_DFL: the nice-0 weight (Linux sched_prio_to_weight[20]); a task's vtime advances by
 * used_slice * WEIGHT_DFL / p->scx.weight, so weight WEIGHT_DFL runs vtime at real rate. */
#define AIOS_WEIGHT_DFL 100

/* Signed, wraparound-safe "is virtual time a before b?" (the standard vtime comparison). */
static __always_inline bool vtime_before(__u64 a, __u64 b) { return (__s64)(a - b) < 0; }

/* SCX kfuncs (kernel 7.0). Declared here so we need no external scx header. */
extern s32  scx_bpf_create_dsq(u64 dsq_id, s32 node) __ksym;
extern void scx_bpf_dsq_insert_vtime(struct task_struct *p, u64 dsq_id, u64 slice, u64 vtime, u64 enq_flags) __ksym;
extern bool scx_bpf_dsq_move_to_local(u64 dsq_id) __ksym;
extern s32  scx_bpf_dsq_nr_queued(u64 dsq_id) __ksym;   /* queued-task count -- does NORMAL have work? */

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

/* .enqueue: a task became runnable -> HIGH DSQ if it is AIOS, else NORMAL. Insert ordered by the task's
 * virtual time (layer 3) so each queue is drained lowest-vtime-first = weighted-fair among its tasks. */
SEC("struct_ops/aios_enqueue")
void BPF_PROG(aios_enqueue, struct task_struct *p, u64 enq_flags)
{
	u64 dsq = AIOS_DSQ_NORMAL;
	u64 vtime = p->scx.dsq_vtime;

	if (task_is_aios(p)) {
		__sync_fetch_and_add(&aios_enq, 1);
		dsq = AIOS_DSQ_HI;
	} else {
		__sync_fetch_and_add(&other_enq, 1);
	}

	/* Clamp a lagging vtime to at most one slice behind the global clock, on EVERY enqueue. This stops
	 * (a) a long sleeper banking unlimited credit while asleep and monopolising on wake, and (b) a
	 * BRAND-NEW task (dsq_vtime starts 0, far "before" a long-running clock) monopolising until it
	 * catches up. It does NOT flatten the weighting: the weight advantage comes from a LIGHT task's
	 * vtime racing AHEAD of the clock (charged used*100/weight with a small weight), not from the heavy
	 * task falling behind -- measured on HW, bursty guests split ~3:1 (nice 0 vs +5) and ~25:1 (nice 0
	 * vs +15) with this clamp, identical to a wakeup-only clamp but with the new-task hole closed.
	 *
	 * The clamp MUST be unconditional (not wakeup-gated) for a subtler reason too: with no ops.select_cpu,
	 * the kernel's default select-cpu DIRECT-DISPATCHES a wake-to-idle-CPU task to SCX_DSQ_LOCAL WITHOUT
	 * calling ops.enqueue at all (the common wake path on a lightly loaded box) -- so a wakeup-gated clamp
	 * would never see that wake, and the task would carry its ancient banked vtime into its NEXT enqueue.
	 * That next enqueue is the slice-expiry re-enqueue (enq_flags == 0, not a wakeup): exactly the path a
	 * WAKEUP-gated clamp skips, but this unconditional clamp catches -- so banked sleep credit is wiped at
	 * the task's first CONTENDED enqueue, bounding any advantage to one slice. (While the CPU was idle,
	 * running immediately with stale vtime starves no one.) */
	if (vtime_before(vtime, vtime_now - SCX_SLICE_DFL))
		vtime = vtime_now - SCX_SLICE_DFL;

	scx_bpf_dsq_insert_vtime(p, dsq, SCX_SLICE_DFL, vtime, enq_flags);
}

/* .dispatch: a CPU needs work. Normally drain HIGH first, then NORMAL (strict AIOS priority). But if a
 * runnable NORMAL task has been starved longer than AIOS_NORMAL_STARVE_NS, serve NORMAL FIRST this cycle
 * (the anti-starvation valve) so NORMAL cannot be starved even when AIOS saturates every CPU. last_norm_ns
 * is reset whenever NORMAL is dispatched (valve OR the normal fall-through), so the timer tracks the true
 * system-wide gap since NORMAL last ran. A cross-CPU race on last_norm_ns is harmless (a scalar timestamp;
 * worst case the valve fires a hair early/late). */
SEC("struct_ops/aios_dispatch")
void BPF_PROG(aios_dispatch, s32 cpu, struct task_struct *prev)
{
	__u64 now = bpf_ktime_get_ns();

	/* The starvation timer must age ONLY while a NORMAL task is genuinely being passed over for HIGH --
	 * i.e. BOTH queues have runnable work. Reset it whenever either queue is empty (no HIGH-vs-NORMAL
	 * contention, so nothing to starve -- this also covers deep idle, where dispatch is not even called)
	 * or on the first dispatch. Then `now - last_norm_ns` is the time a *waiting* NORMAL task has been
	 * held behind HIGH, and valve_fires is a true "AIOS is saturating the box" signature, not idle noise. */
	if (last_norm_ns == 0 ||
	    scx_bpf_dsq_nr_queued(AIOS_DSQ_NORMAL) == 0 ||
	    scx_bpf_dsq_nr_queued(AIOS_DSQ_HI) == 0) {
		last_norm_ns = now;
	} else if (now - last_norm_ns > AIOS_NORMAL_STARVE_NS) {
		if (scx_bpf_dsq_move_to_local(AIOS_DSQ_NORMAL)) {   /* valve: a starved NORMAL task goes first */
			last_norm_ns = now;
			__sync_fetch_and_add(&norm_dispatch, 1);
			__sync_fetch_and_add(&valve_fires, 1);
			return;
		}
	}

	if (scx_bpf_dsq_move_to_local(AIOS_DSQ_HI)) {         /* strict AIOS priority */
		__sync_fetch_and_add(&hi_dispatch, 1);
		return;
	}
	if (scx_bpf_dsq_move_to_local(AIOS_DSQ_NORMAL)) {     /* HIGH empty -> serve NORMAL */
		last_norm_ns = now;
		__sync_fetch_and_add(&norm_dispatch, 1);
	}
}

/* .running: a task is about to run -> advance the global vtime clock to it if we are behind, so vtime
 * tracks real progress and the enqueue clamp stays meaningful. */
SEC("struct_ops/aios_running")
void BPF_PROG(aios_running, struct task_struct *p)
{
	if (vtime_before(vtime_now, p->scx.dsq_vtime))
		vtime_now = p->scx.dsq_vtime;
}

/* .stopping: a task stopped running -> charge its virtual time by the CPU it just used, SCALED BY ITS
 * WEIGHT. p->scx.slice is the slice LEFT, so (SCX_SLICE_DFL - slice) is what it used; dividing by weight
 * (relative to the nice-0 default) makes a heavier task's vtime advance slower -> it is picked sooner ->
 * it gets proportionally more CPU. This is where per-guest `nice` weights take effect. */
SEC("struct_ops/aios_stopping")
void BPF_PROG(aios_stopping, struct task_struct *p, bool runnable)
{
	u32 weight = p->scx.weight ?: AIOS_WEIGHT_DFL;   /* weight is >= 1 in practice; guard anyway */
	u64 used = SCX_SLICE_DFL - p->scx.slice;


	p->scx.dsq_vtime += used * AIOS_WEIGHT_DFL / weight;
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
	.running  = (void *)aios_running,
	.stopping = (void *)aios_stopping,
	.exit     = (void *)aios_exit,
	.flags    = 0,
	.name     = "aios",
};
