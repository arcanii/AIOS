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
