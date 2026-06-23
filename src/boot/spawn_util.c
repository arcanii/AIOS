/*
 * spawn_util.c -- Process spawn helpers
 *
 * Extracted from aios_root.c (v0.4.53 modularization).
 * Configures and spawns seL4 processes with endpoint capabilities.
 */
#include "aios/root_shared.h"
#include <sel4utils/process.h>
#include <sel4utils/process_config.h>
#include <stdio.h>

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
static volatile unsigned g_core_rr;
void aios_assign_core(seL4_CPtr tcb) {
    if (!g_proc_distribute) { seL4_TCB_SetAffinity(tcb, 0); return; }
    unsigned core = 1u + (g_core_rr++ % (unsigned)(CONFIG_MAX_NUM_NODES - 1));
    seL4_TCB_SetAffinity(tcb, core);
}
#else
volatile int g_proc_distribute = 0;
void aios_assign_core(seL4_CPtr tcb) { (void)tcb; }
#endif

/* /proc/coresched[.0|.1] -- toggle user-process core distribution (the Stage S kill switch). */
int coresched_cmd(const char *args, char *buf, int bufsize) {
    if (args[0] == '.' && (args[1] == '0' || args[1] == '1'))
        g_proc_distribute = (args[1] == '1');
    return snprintf(buf, bufsize,
        "coresched: distribute=%d (user procs round-robin cores 1..%d; .0 pin-core0 / .1 spread)\n",
        g_proc_distribute, CONFIG_MAX_NUM_NODES - 1);
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
volatile int g_server_distribute = 0;
#define AIOS_MAX_SERVER_TCBS 32
static seL4_CPtr g_server_tcbs[AIOS_MAX_SERVER_TCBS];
static int g_server_tcb_count;

/* Round-robin across ALL cores 0..N-1 by registration index (stable -> A/B-reproducible:
 * a given server always lands on the same core, so re-pinning is idempotent). */
static void server_apply_affinity(int idx) {
    unsigned core = g_server_distribute ? (unsigned)(idx % CONFIG_MAX_NUM_NODES) : 0u;
    seL4_TCB_SetAffinity(g_server_tcbs[idx], core);
}

void aios_server_pin(seL4_CPtr tcb) {
    int idx = g_server_tcb_count;
    if (idx >= AIOS_MAX_SERVER_TCBS) {
        seL4_TCB_SetAffinity(tcb, 0);   /* registry full: pin to core 0, don't track */
        return;
    }
    g_server_tcbs[idx] = tcb;
    g_server_tcb_count = idx + 1;
    server_apply_affinity(idx);
}

/* /proc/distribute[.0|.1] -- spread the root servers across cores / pin them to core 0. */
int distribute_cmd(const char *args, char *buf, int bufsize) {
    if (args[0] == '.' && (args[1] == '0' || args[1] == '1')) {
        g_server_distribute = (args[1] == '1');
        for (int i = 0; i < g_server_tcb_count; i++) server_apply_affinity(i);
    }
    return snprintf(buf, bufsize,
        "distribute: on=%d servers=%d cores=0..%d (.1 spread + re-pin live TCBs / .0 pin-core0);"
        " survive threads + shared timer stay pinned\n",
        g_server_distribute, g_server_tcb_count, CONFIG_MAX_NUM_NODES - 1);
}
#else
volatile int g_server_distribute = 0;
void aios_server_pin(seL4_CPtr tcb) { (void)tcb; }
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
