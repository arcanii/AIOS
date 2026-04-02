/*
 * AIOS 0.4.x Root Task — Phase 3b
 * Serial server + mini shell via IPC
 */
#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4platsupport/bootinfo.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>
#include <sel4utils/vspace.h>
#include <sel4utils/process.h>
#include <sel4utils/process_config.h>
#include <vka/capops.h>
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

/* Spawn a server process, pass an endpoint cap, start it */
static int spawn_with_ep(const char *name, uint8_t prio,
                         sel4utils_process_t *proc,
                         vka_object_t *fault_ep,
                         seL4_CPtr ep_cap,
                         seL4_CPtr *child_slot_out) {
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

    /* Copy endpoint into child's CSpace */
    seL4_CPtr child_ep = sel4utils_copy_cap_to_process(proc, &vka, ep_cap);
    if (child_slot_out) *child_slot_out = child_ep;

    /* Pass EP slot as argv[0] */
    char ep_str[16];
    int len = 0;
    unsigned long v = child_ep;
    if (v == 0) { ep_str[len++] = '0'; }
    else {
        char tmp[16]; int ti = 0;
        while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
        while (ti--) ep_str[len++] = tmp[ti];
    }
    ep_str[len] = '\0';

    char *child_argv[] = { ep_str };
    error = sel4utils_spawn_process_v(proc, &vka, &vspace, 1, child_argv, 1);
    return error;
}

int main(int argc, char *argv[]) {
    int error;

    printf("\n");
    printf("============================================\n");
    printf("  AIOS 0.4.x Root Task — Phase 3b\n");
    printf("  Serial Server + Shell via IPC\n");
    printf("============================================\n\n");

    /* Boot */
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
    vka_object_t fault_ep, serial_ep;
    vka_alloc_endpoint(&vka, &fault_ep);
    vka_alloc_endpoint(&vka, &serial_ep);

    /* Lower root priority */
    seL4_TCB_SetPriority(seL4_CapInitThreadTCB,
                         seL4_CapInitThreadTCB, 50);

    /* 1. Spawn serial server */
    printf("[proc] Spawning serial_server...\n");
    sel4utils_process_t serial_proc;
    seL4_CPtr serial_child_slot;
    error = spawn_with_ep("serial_server", 200, &serial_proc,
                          &fault_ep, serial_ep.cptr, &serial_child_slot);
    if (error) { printf("[proc] serial_server FAILED: %d\n", error); goto idle; }
    printf("[proc] serial_server OK (ep slot %lu)\n",
           (unsigned long)serial_child_slot);

    /* 2. Spawn mini shell — give it same serial endpoint
     * Badge the endpoint so serial_server can identify the shell */
    printf("[proc] Spawning mini_shell...\n");

    /* Create a badged copy of serial_ep for the shell */
    seL4_CPtr shell_serial_ep;
    {
        cspacepath_t src, dest;
        vka_cspace_make_path(&vka, serial_ep.cptr, &src);
        error = vka_cspace_alloc_path(&vka, &dest);
        if (!error) {
            error = vka_cnode_mint(&dest, &src, seL4_AllRights,
                                   1);
        }
        if (error) {
            printf("[proc] WARNING: badge failed, using unbadged ep\n");
            shell_serial_ep = serial_ep.cptr;
        } else {
            shell_serial_ep = dest.capPtr;
        }
    }

    sel4utils_process_t shell_proc;
    seL4_CPtr shell_ep_slot;
    error = spawn_with_ep("mini_shell", 150, &shell_proc,
                          &fault_ep, shell_serial_ep, &shell_ep_slot);
    if (error) { printf("[proc] mini_shell FAILED: %d\n", error); goto idle; }
    printf("[proc] mini_shell OK (ep slot %lu)\n",
           (unsigned long)shell_ep_slot);

    printf("\n[root] All processes spawned. Waiting...\n\n");

    /* Wait for shell to exit */
    {
        seL4_Word badge;
        seL4_Recv(fault_ep.cptr, &badge);
        printf("[root] mini_shell exited\n");
    }

    printf("\n============================================\n");
    printf("  Phase 3b complete!\n");
    printf("  Serial server + shell via IPC\n");
    printf("============================================\n\n");

idle:
    printf("[root] Idle\n");
    while (1) { seL4_Yield(); }
    return 0;
}
