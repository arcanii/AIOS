/*
 * AIOS 0.4.x Root Task — Phase 3
 * Multiple processes + IPC
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

#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 200)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];
static sel4utils_alloc_data_t vspace_data;

static simple_t simple;
static vka_t vka;
static vspace_t vspace;
static allocman_t *allocman;

static int spawn_child(const char *name, uint8_t prio,
                       sel4utils_process_t *proc, vka_object_t *fault_ep) {
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

int main(int argc, char *argv[]) {
    int error;

    printf("\n");
    printf("============================================\n");
    printf("  AIOS 0.4.x Root Task — Phase 3\n");
    printf("  Multi-process + IPC\n");
    printf("============================================\n\n");

    seL4_BootInfo *info = platsupport_get_bootinfo();
    if (!info) { printf("FATAL: No bootinfo\n"); return -1; }

    simple_default_init_bootinfo(&simple, info);
    allocman = bootstrap_use_current_simple(
        &simple, ALLOCATOR_STATIC_POOL_SIZE, allocator_mem_pool);
    if (!allocman) { printf("FATAL: allocman\n"); return -1; }
    allocman_make_vka(&vka, allocman);

    error = sel4utils_bootstrap_vspace_with_bootinfo_leaky(
        &vspace, &vspace_data,
        simple_get_pd(&simple), &vka, info);
    if (error) { printf("FATAL: vspace: %d\n", error); return -1; }
    printf("[boot] All subsystems OK\n\n");

    /* Endpoints */
    vka_object_t fault_ep, echo_ep;
    vka_alloc_endpoint(&vka, &fault_ep);
    vka_alloc_endpoint(&vka, &echo_ep);

    /* Lower root priority */
    seL4_TCB_SetPriority(seL4_CapInitThreadTCB,
                         seL4_CapInitThreadTCB, 100);

    /* === Spawn hello children === */
    printf("[proc] === Spawning processes ===\n\n");

    sel4utils_process_t child1, child2;
    error = spawn_child("hello_child", 200, &child1, &fault_ep);
    printf("[proc] hello_child #1: %s\n", error ? "FAILED" : "OK");

    error = spawn_child("hello_child", 200, &child2, &fault_ep);
    printf("[proc] hello_child #2: %s\n", error ? "FAILED" : "OK");

    /* === Spawn echo server with endpoint === */
    sel4utils_process_t echo_proc;
    {
        sel4utils_process_config_t config = process_config_new(&simple);
        config = process_config_elf(config, "echo_server", true);
        config = process_config_create_cnode(config, 12);
        config = process_config_create_vspace(config, NULL, 0);
        config = process_config_priority(config, 200);
        config = process_config_auth(config, simple_get_tcb(&simple));
        config = process_config_fault_endpoint(config, fault_ep);

        error = sel4utils_configure_process_custom(
            &echo_proc, &vka, &vspace, config);
        if (error) { printf("[proc] echo_server configure FAILED: %d\n", error); goto idle; }

        /* Copy endpoint into child's CSpace */
        seL4_CPtr child_ep_slot = sel4utils_copy_cap_to_process(
            &echo_proc, &vka, echo_ep.cptr);
        printf("[proc] echo_server: ep at child slot %lu\n",
               (unsigned long)child_ep_slot);

        /* Pass EP slot as argv[0] */
        char ep_str[16];
        snprintf(ep_str, sizeof(ep_str), "%lu", (unsigned long)child_ep_slot);
        char *echo_argv[] = { ep_str };
        error = sel4utils_spawn_process_v(
            &echo_proc, &vka, &vspace, 1, echo_argv, 1);
        printf("[proc] echo_server: %s\n", error ? "FAILED" : "OK");
    }

    printf("\n[proc] Waiting for hello children to exit...\n");
    for (int i = 0; i < 2; i++) {
        seL4_Word badge;
        seL4_Recv(fault_ep.cptr, &badge);
        printf("[root] Child exited\n");
    }

    /* === IPC test === */
    printf("\n[ipc] === IPC with echo_server ===\n\n");
    for (int i = 0; i < 5; i++) {
        seL4_SetMR(0, (seL4_Word)(i * 10));
        seL4_MessageInfo_t reply = seL4_Call(
            echo_ep.cptr, seL4_MessageInfo_new(0, 0, 0, 1));
        seL4_Word result = seL4_GetMR(0);
        printf("[root] Sent %d, got %lu\n", i * 10, (unsigned long)result);
    }

    /* Collect echo server exit */
    {
        seL4_Word badge;
        seL4_Recv(fault_ep.cptr, &badge);
        printf("[root] echo_server exited\n");
    }

    printf("\n============================================\n");
    printf("  Phase 3 complete!\n");
    printf("  3 processes + IPC verified\n");
    printf("============================================\n\n");

idle:
    printf("[root] Idle\n");
    while (1) { seL4_Yield(); }
    return 0;
}
