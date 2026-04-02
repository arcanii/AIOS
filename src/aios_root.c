/*
 * AIOS 0.4.x Root Task — Phase 2
 */
#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4platsupport/bootinfo.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>
#include <sel4utils/vspace.h>
#include <sel4utils/process.h>
#include <sel4utils/process_config.h>
#include <simple/simple.h>
#include <simple-default/simple-default.h>
#include <utils/util.h>

#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 100)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];
static sel4utils_alloc_data_t vspace_data;

static simple_t simple;
static vka_t vka;
static vspace_t vspace;
static allocman_t *allocman;

int main(int argc, char *argv[]) {
    int error;

    printf("\n");
    printf("============================================\n");
    printf("  AIOS 0.4.0 Root Task — Phase 2\n");
    printf("  seL4 direct mode (no Microkit)\n");
    printf("============================================\n\n");

    seL4_BootInfo *info = platsupport_get_bootinfo();
    if (!info) { printf("FATAL: No bootinfo\n"); return -1; }
    printf("[boot] BootInfo OK, CNode: %zu slots\n",
           (size_t)BIT(info->initThreadCNodeSizeBits));

    simple_default_init_bootinfo(&simple, info);
    allocman = bootstrap_use_current_simple(
        &simple, ALLOCATOR_STATIC_POOL_SIZE, allocator_mem_pool);
    if (!allocman) { printf("FATAL: allocman\n"); return -1; }
    allocman_make_vka(&vka, allocman);

    error = sel4utils_bootstrap_vspace_with_bootinfo_leaky(
        &vspace, &vspace_data,
        simple_get_pd(&simple), &vka, info);
    if (error) { printf("FATAL: vspace: %d\n", error); return -1; }

    int total_mem = 0;
    for (seL4_Word i = info->untyped.start; i < info->untyped.end; i++) {
        seL4_UntypedDesc *ut = &info->untypedList[i - info->untyped.start];
        if (!ut->isDevice) total_mem += BIT(ut->sizeBits);
    }
    printf("[boot] RAM: %d MB\n", total_mem / (1024 * 1024));
    printf("[boot] All subsystems OK\n\n");

    /* Create fault endpoint */
    vka_object_t fault_ep;
    error = vka_alloc_endpoint(&vka, &fault_ep);
    if (error) { printf("FATAL: fault ep\n"); return -1; }

    /* Spawn child with explicit priority */
    printf("[proc] Spawning hello_child...\n");
    sel4utils_process_t child_proc;

    sel4utils_process_config_t config = process_config_new(&simple);
    config = process_config_elf(config, "hello_child", true);
    config = process_config_create_cnode(config, 12);
    config = process_config_create_vspace(config, NULL, 0);
    config = process_config_priority(config, seL4_MaxPrio - 1);
    config = process_config_auth(config, simple_get_tcb(&simple));
    config = process_config_fault_endpoint(config, fault_ep);

    error = sel4utils_configure_process_custom(
        &child_proc, &vka, &vspace, config);
    if (error) {
        printf("[proc] FATAL: configure_process failed: %d\n", error);
        goto idle;
    }
    printf("[proc] Child configured (priority %d):\n", seL4_MaxPrio - 1);
    printf("[proc]   VSpace: isolated\n");
    printf("[proc]   CSpace: 4096 slots\n");
    printf("[proc]   TCB: kernel-scheduled\n");

    /* Lower root task priority so child can run */
    error = seL4_TCB_SetPriority(seL4_CapInitThreadTCB,
                                  seL4_CapInitThreadTCB,
                                  seL4_MaxPrio - 2);
    if (error) {
        printf("[proc] WARNING: Could not lower root priority: %d\n", error);
    }

    /* Start child */
    error = sel4utils_spawn_process_v(
        &child_proc, &vka, &vspace, 0, NULL, 1);
    if (error) {
        printf("[proc] FATAL: spawn failed: %d\n", error);
        goto idle;
    }
    printf("[proc] Child spawned (it should preempt us now)!\n\n");

    /* Wait for child to fault (exit = cap fault on reply cap) */
    printf("[root] Waiting for child on fault endpoint...\n");
    seL4_Word badge;
    seL4_MessageInfo_t msg = seL4_Recv(fault_ep.cptr, &badge);
    printf("[root] Child faulted/exited (badge=%lu, label=%lu)\n",
           (unsigned long)badge,
           (unsigned long)seL4_MessageInfo_get_label(msg));

    printf("\n============================================\n");
    printf("  Phase 2 complete — child ran in its\n");
    printf("  own VSpace + TCB + CSpace!\n");
    printf("============================================\n\n");

idle:
    printf("[root] Idle\n");
    while (1) { seL4_Yield(); }
    return 0;
}
