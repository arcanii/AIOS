/*
 * spawn_util.c -- Process spawn helpers
 *
 * Extracted from aios_root.c (v0.4.53 modularization).
 * Configures and spawns seL4 processes with endpoint capabilities.
 */
#include "aios/root_shared.h"
#include "aios/sched_strategy.h"
#include <sel4utils/process.h>
#include <sel4utils/process_config.h>
#include <stdio.h>
#include <string.h>

/* Mirror of the kernel AIOS_SIBLING_TIMER_MASK (deps/kernel src/arch/arm/kernel/boot.c). When 1 (the
 * default) the timer PPI is masked on cores 1..N-1 so they get NO timer preemption -- the cluster-
 * survival circumvention (a 1-core wedge no longer freezes the cluster). The consequence: placing
 * RUNNABLE work on a secondary is safe ONLY for cooperative/yielding work; a CPU-bound thread there
 * monopolizes the core (never preempted -> starvation). The distribution knobs below
 * (coresched/placement/pincore/distribute) all target cores 1..N-1, so they make this constraint LOUD
 * via append_mask_warn() instead of silently distributing onto non-preemptible cores. KEEP IN SYNC
 * with the kernel define (there is no userspace-readable kernel global for it). */
#ifndef AIOS_SIBLING_TIMER_MASK
#define AIOS_SIBLING_TIMER_MASK 1
#endif

/* Append the timer-mask preemption warning to a /proc command's output when secondary placement is
 * being enabled. No-op on a single-core build or when the mask is compiled off. */
static int append_mask_warn(char *buf, int w, int bufsize) {
#if (CONFIG_MAX_NUM_NODES > 1) && AIOS_SIBLING_TIMER_MASK
    if (w >= 0 && w < bufsize - 1)
        w += snprintf(buf + w, bufsize - w,
            "  WARNING: cores 1..%d are TIMER-MASKED (non-preemptible survive config) -- place only "
            "cooperative/yielding work there; CPU-bound work monopolizes the core (no timer preemption). "
            "Rebuild kernel AIOS_SIBLING_TIMER_MASK=0 to preempt secondaries (drops 1-core wedge survival).\n",
            CONFIG_MAX_NUM_NODES - 1);
#else
    (void)buf; (void)bufsize;
#endif
    return w;
}

int spawn_with_args(const char *name, uint8_t prio,
                    sel4utils_process_t *proc,
                    vka_object_t *fault_ep,
                    int ep_count, seL4_CPtr *eps,
                    seL4_CPtr *child_slots) {
    int error;
    sel4utils_process_config_t config = process_config_new(&simple);
    config = process_config_elf(config, name, true);
    config = process_config_create_cnode(config, 12);
    config = process_config_create_vspace(config, NULL, 0);
    config = process_config_priority(config, prio);
    config = process_config_auth(config, simple_get_tcb(&simple));
    config = process_config_fault_endpoint(config, *fault_ep);

    error = sel4utils_configure_process_custom(proc, &vka, &vspace, config);
    if (error) return error;

    char argv_bufs[8][16];
    char *child_argv[8];
    for (int i = 0; i < ep_count && i < 8; i++) {
        child_slots[i] = sel4utils_copy_cap_to_process(proc, &vka, eps[i]);
        snprintf(argv_bufs[i], 16, "%lu", (unsigned long)child_slots[i]);
        child_argv[i] = argv_bufs[i];
    }

    return sel4utils_spawn_process_v(proc, &vka, &vspace,
                                      ep_count, child_argv, 1);
}

/* v0.4.257 Stage S: distribute user processes across cores. The root SERVERS stay pinned to
 * core 0 (the lock-free vka/cspace allocator is single-owner -- concurrent allocs from other
 * cores tear the slot bitmap). USER processes have their OWN PD/cspace, reaching the root
 * allocator only via IPC to the core-0 servers, so they run SAFELY on cores 1..N-1. The
 * kernel's per-ASID residency-masked TLB shootdown ([[project_stall_hunt]]) keeps teardown
 * correct AND stall-free with work spread across cores. Round-robin over cores 1..N-1 (core 0
 * runs the servers + the spawn machinery). DEFAULT OFF: AIOS is IPC-bound (everything funnels
 * through the core-0 servers) and seL4's big kernel lock makes 4 cores hammering syscalls
 * CONTEND rather than parallelize -- broad distribution regresses the pipeline ceiling (30->6,
 * measured). It HELPS CPU-bound parallel work, so it ships opt-in: /proc/coresched.1 spreads,
 * .0 pins to core 0 (default). Inert on a 1-core build. */
#if CONFIG_MAX_NUM_NODES > 1
volatile int g_proc_distribute = 0;
/* Phase A step 3 prerequisite: per-core SESSION/PROC bind. When >=0, every subsequently
 * spawned USER process is pinned to this core, OVERRIDING coresched + the load policy -- so
 * a "victim" session can be bound to core N (sleep->exit there = a teardown-after-idle wedge
 * on core N) and a "survivor" session to core M, the deterministic placement the HW
 * confinement gate needs. Sticky until reset; default -1 == off. */
volatile int g_pincore = -1;
static unsigned g_core_rr;   /* round-robin counter (legacy coresched + strat_rr); hint only, races benign */

/* ── Load-and-locality-aware placement (general work assignment) ─────────────────────────
 * seL4 has NO kernel load-balancer and threads never migrate on their own, so this is a
 * userspace policy applied ONCE at thread creation (assign-once == "stop core hopping").
 * Load = live threads per core: immortal root servers in g_server_load_n[], user procs by
 * SCANNING the live active_procs table keyed by a parallel g_proc_core[] (no ++/-- drift --
 * a dead proc clears active_procs[].active and stops being counted; its g_proc_core slot is
 * overwritten when reused). Policy = HYBRID (Bryan's choice): the least-loaded core AMONG
 * the currently-ACTIVE ones, spilling onto a cold core only when every active core is at/over
 * g_spill_threshold. NOT a stall cure (s12: a busy core still wedges on teardown) -- this is
 * for locality + balance + fewer migrations + predictable blast-radius. */
volatile int g_place_auto = 0;        /* 1 = hybrid load policy drives user-proc placement */
volatile int g_spill_threshold = 4;   /* open a cold core only when every active core >= this */
static int g_proc_core[MAX_ACTIVE_PROCS];          /* core recorded per proc slot (scan source) */
static unsigned g_server_load_n[CONFIG_MAX_NUM_NODES];  /* server count per core (recomputed on every pin) */

unsigned aios_core_load(unsigned c) {
    unsigned n = (c < CONFIG_MAX_NUM_NODES) ? g_server_load_n[c] : 0;
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++)
        if (active_procs[i].active && (unsigned)g_proc_core[i] == c) n++;
    return n;
}

/* The placement strategies are EXTRACTED into include/aios/sched_strategy.h as pure
 * functions of the per-core load, so they can be A/B'd (select via /proc/placement.sN) and
 * host-tested (scripts/placement_strategy_host_test.c). We supply the live load here. */
volatile int g_place_strategy = 0;   /* index into AIOS_PLACE_STRATEGIES (default hybrid) */

static unsigned load_cb(unsigned core, void *cookie) { (void)cookie; return aios_core_load(core); }

static unsigned pick_core(int hint_core) {
    int s = g_place_strategy;
    if (s < 0 || s >= AIOS_N_PLACE_STRATEGIES) s = 0;
    aios_place_ctx_t ctx = {
        .n_cores = CONFIG_MAX_NUM_NODES, .threshold = g_spill_threshold,
        .hint_core = hint_core, .load = load_cb, .cookie = 0, .rr = &g_core_rr,
    };
    return AIOS_PLACE_STRATEGIES[s].fn(&ctx);
}

void aios_assign_core(int proc_idx, seL4_CPtr tcb) {
    unsigned core;
    if (g_pincore >= 0 && g_pincore < CONFIG_MAX_NUM_NODES)
        core = (unsigned)g_pincore;                                    /* manual override (test) */
    else if (g_place_auto)
        core = pick_core(-1);                          /* selected strategy (locality hint TBD) */
    else if (g_proc_distribute)
        core = 1u + (g_core_rr++ % (unsigned)(CONFIG_MAX_NUM_NODES - 1));  /* legacy coresched RR */
    else
        core = 0;                                                     /* default: home core */
    seL4_TCB_SetAffinity(tcb, core);
    if (proc_idx >= 0 && proc_idx < MAX_ACTIVE_PROCS) g_proc_core[proc_idx] = (int)core;
}

/* /proc/placement[.0|.1|.sN|.tN] -- load-and-locality policy for user-proc placement.
 * .1 enable / .0 disable (legacy coresched/core-0) / .sN select strategy N / .tN spill
 * threshold N. Lists the strategy menu + live per-core load so the policy is observable
 * and A/B-able. */
int placement_cmd(const char *args, char *buf, int bufsize) {
    if (args[0] == '.') {
        if (args[1] == '0' || args[1] == '1') g_place_auto = (args[1] == '1');
        else if (args[1] == 't' && args[2] >= '1' && args[2] <= '9') g_spill_threshold = args[2] - '0';
        else if (args[1] == 's' && args[2] >= '0' && args[2] <= '9') {
            int s = args[2] - '0';
            if (s < AIOS_N_PLACE_STRATEGIES) g_place_strategy = s;
        }
    }
    int w = snprintf(buf, bufsize,
        "placement: auto=%d strategy=%d(%s) spill_threshold=%d (.1/.0 on-off / .sN strategy / .tN threshold)\n",
        g_place_auto, g_place_strategy, AIOS_PLACE_STRATEGIES[g_place_strategy].name, g_spill_threshold);
    w += snprintf(buf + w, bufsize - w, "  strategies:");
    for (int i = 0; i < AIOS_N_PLACE_STRATEGIES && w < bufsize - 1; i++)
        w += snprintf(buf + w, bufsize - w, " %d=%s", i, AIOS_PLACE_STRATEGIES[i].name);
    if (w < bufsize - 1) w += snprintf(buf + w, bufsize - w, "\n");
    for (unsigned c = 0; c < CONFIG_MAX_NUM_NODES && w < bufsize - 1; c++)
        w += snprintf(buf + w, bufsize - w, "  core %u load %u (servers %u)\n",
                      c, aios_core_load(c), g_server_load_n[c]);
    if (g_place_auto) w = append_mask_warn(buf, w, bufsize);
    return w;
}
#else
volatile int g_proc_distribute = 0;
volatile int g_pincore = -1;
volatile int g_place_auto = 0;
volatile int g_spill_threshold = 4;
volatile int g_place_strategy = 0;
void aios_assign_core(int proc_idx, seL4_CPtr tcb) { (void)proc_idx; (void)tcb; }
int placement_cmd(const char *args, char *buf, int bufsize) {
    (void)args;
    return snprintf(buf, bufsize, "placement: single-core build (no-op)\n");
}
#endif

/* /proc/coresched[.0|.1] -- toggle user-process core distribution (the Stage S kill switch). */
int coresched_cmd(const char *args, char *buf, int bufsize) {
    if (args[0] == '.' && (args[1] == '0' || args[1] == '1'))
        g_proc_distribute = (args[1] == '1');
    int w = snprintf(buf, bufsize,
        "coresched: distribute=%d (user procs round-robin cores 1..%d; .0 pin-core0 / .1 spread)\n",
        g_proc_distribute, CONFIG_MAX_NUM_NODES - 1);
    if (g_proc_distribute) w = append_mask_warn(buf, w, bufsize);
    return w;
}

/* /proc/pincore[.N|.r] -- bind every subsequent user spawn to core N (overrides coresched),
 * or .r to reset to the coresched/core-0 default. The per-core session-bind primitive for the
 * Phase A step-3 confinement gate. Default OFF (-1). */
int pincore_cmd(const char *args, char *buf, int bufsize) {
#if CONFIG_MAX_NUM_NODES > 1
    if (args[0] == '.') {
        if (args[1] == 'r') g_pincore = -1;
        else if (args[1] >= '0' && args[1] <= '9') {
            int c = args[1] - '0';
            if (c < CONFIG_MAX_NUM_NODES) g_pincore = c;   /* ignore out-of-range */
        }
    }
    int w = snprintf(buf, bufsize,
        "pincore: target=%d (.N bind next user spawns to core N (0..%d) / .r reset to coresched/core0)\n",
        g_pincore, CONFIG_MAX_NUM_NODES - 1);
    if (g_pincore >= 1) w = append_mask_warn(buf, w, bufsize);   /* core 0 is preemptible; 1..N-1 masked */
    return w;
#else
    (void)args;
    return snprintf(buf, bufsize, "pincore: single-core build (no-op)\n");
#endif
}

/* ── Symmetric-kernel Phase A step 2: ROOT SERVER core distribution ──────────────────
 * Un-pin the root server threads from core 0 so a per-core idle->wake wedge takes down
 * only the servers on that core, not the whole cluster (docs/NEXT_20260623_symmetric_
 * kernel_redesign.md). SAFE ONLY because the global lock-free vka/allocman is now
 * serialized (aios_vka_install_lock, src/boot/vka_lock.c) -- otherwise concurrent allocs
 * from servers on different cores tear the CSpace-slot bitmap (the v0.4.178 bug).
 *
 * DEFAULT OFF == every server pinned to core 0, byte-for-byte today's behavior. It ships
 * as a runtime A/B knob (/proc/distribute) so the lock + distribution can be proven in
 * situ first. Each un-pinnable server registers its TCB here via aios_server_pin() at
 * spawn (which also applies the current policy); toggling the knob re-pins the LIVE TCBs
 * (SetAffinity on a running/blocked thread just changes where it next runs -- safe in
 * seL4; a thread migrated while holding the vka lock simply releases it on the new core).
 *
 * NOT distributed (stay pinned, by design): the survive threads core0_hb (core 0, the
 * core-0 stall detector) + core1_wd (WD_CORE, kernel-timer-masked so it survives a wedge),
 * and the SHARED system timer (timer_server) -- a single shared timer on core N would make
 * a wedge of N freeze every sleeper on every core (a cross-core BLOCKING dep, the exact
 * s12 failure). The per-core timer that removes that dep is Phase B. */
#if CONFIG_MAX_NUM_NODES > 1
/* 0 = off (all core 0, today's behavior); 1 = FULL round-robin across cores 0..N-1;
 * 2 = SURGICAL preset (Phase A step 3): distribute only the allocator-heavy + teardown-
 * trigger servers -- pipe (fork/reap), exec (spawn), fs (file I/O) -- onto dedicated cores
 * and keep the latency-critical + yield-spinning ones (console/net, timer, flush,
 * serverstats, display, crypto, root) on the home core 0. Full round-robin was ~2.6x slower
 * (barrier B4) AND moved the console; surgical is the low-contention distribution the HW
 * confinement gate wants. */
volatile int g_server_distribute = 0;
#define AIOS_MAX_SERVER_TCBS 32
static struct { const char *name; seL4_CPtr tcb; } g_servers[AIOS_MAX_SERVER_TCBS];
static int g_server_count;

/* Surgical (mode 2) name->core map. The keys match the names passed to aios_server_pin
 * (== the start_server_thread / aios_acct names). Anything unlisted stays on the home core. */
static unsigned surgical_core(const char *name) {
    if (name) {
        if (!strcmp(name, "pipe")) return 1u % CONFIG_MAX_NUM_NODES;
        if (!strcmp(name, "exec")) return 2u % CONFIG_MAX_NUM_NODES;
        if (!strcmp(name, "fs"))   return 3u % CONFIG_MAX_NUM_NODES;
    }
    return 0;
}

/* The core a server lands on under the current mode (single source of truth, also used by
 * the /proc/distribute listing so the reported placement == the affinity actually set). */
static unsigned server_target_core(int idx) {
    switch (g_server_distribute) {
        case 1:  return (unsigned)(idx % CONFIG_MAX_NUM_NODES);   /* full round-robin */
        case 2:  return surgical_core(g_servers[idx].name);       /* surgical preset  */
        default: return 0;                                        /* off -> home core */
    }
}

static void server_apply_affinity(int idx) {
    seL4_TCB_SetAffinity(g_servers[idx].tcb, server_target_core(idx));
}

/* Recompute the per-core server load (immortal threads) from the registry, so the load
 * policy's aios_core_load() sees servers. Called after any (re)pin. */
static void recompute_server_load(void) {
    for (unsigned c = 0; c < CONFIG_MAX_NUM_NODES; c++) g_server_load_n[c] = 0;
    for (int i = 0; i < g_server_count; i++) {
        unsigned c = server_target_core(i);
        if (c < CONFIG_MAX_NUM_NODES) g_server_load_n[c]++;
    }
}

void aios_server_pin(const char *name, seL4_CPtr tcb) {
    int idx = g_server_count;
    if (idx >= AIOS_MAX_SERVER_TCBS) {
        seL4_TCB_SetAffinity(tcb, 0);   /* registry full: pin to core 0, don't track */
        return;
    }
    g_servers[idx].name = name;   /* all callers pass string literals (static storage) */
    g_servers[idx].tcb = tcb;
    g_server_count = idx + 1;
    server_apply_affinity(idx);
    recompute_server_load();
}

/* /proc/distribute[.0|.1|.2] -- server core-distribution mode; re-pins the live TCBs and
 * lists each server's assigned core (so the gate can confirm placement). */
int distribute_cmd(const char *args, char *buf, int bufsize) {
    if (args[0] == '.' && args[1] >= '0' && args[1] <= '2') {
        g_server_distribute = args[1] - '0';
        for (int i = 0; i < g_server_count; i++) server_apply_affinity(i);
        recompute_server_load();
    }
    int w = snprintf(buf, bufsize,
        "distribute: mode=%d servers=%d cores=0..%d (.0 off/core0 / .1 full round-robin /"
        " .2 surgical: pipe->1 exec->2 fs->3, rest home); survive threads + shared timer stay pinned\n",
        g_server_distribute, g_server_count, CONFIG_MAX_NUM_NODES - 1);
    for (int i = 0; i < g_server_count && w < bufsize - 1; i++)
        w += snprintf(buf + w, bufsize - w, "  %-13s core %u\n",
                      g_servers[i].name ? g_servers[i].name : "?", server_target_core(i));
    if (g_server_distribute) w = append_mask_warn(buf, w, bufsize);
    return w;
}
#else
volatile int g_server_distribute = 0;
void aios_server_pin(const char *name, seL4_CPtr tcb) { (void)name; (void)tcb; }
int distribute_cmd(const char *args, char *buf, int bufsize) {
    (void)args;
    return snprintf(buf, bufsize, "distribute: single-core build (no-op)\n");
}
#endif

int spawn_simple(const char *name, uint8_t prio,
                 sel4utils_process_t *proc,
                 vka_object_t *fault_ep) {
    sel4utils_process_config_t config = process_config_new(&simple);
    config = process_config_elf(config, name, true);
    config = process_config_create_cnode(config, 12);
    config = process_config_create_vspace(config, NULL, 0);
    config = process_config_priority(config, prio);
    config = process_config_auth(config, simple_get_tcb(&simple));
    config = process_config_fault_endpoint(config, *fault_ep);

    int error = sel4utils_configure_process_custom(proc, &vka, &vspace, config);
    if (error) return error;
    return sel4utils_spawn_process_v(proc, &vka, &vspace, 0, NULL, 1);
}
