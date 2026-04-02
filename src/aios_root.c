/*
 * AIOS 0.4.x Root Task — Phase 3c
 * Polls UART, forwards keyboard to serial_server via dedicated EP
 */
#include <stdio.h>
#include <stdint.h>
#include <sel4/sel4.h>
#include <sel4platsupport/bootinfo.h>
#include <sel4platsupport/device.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>
#include <sel4utils/vspace.h>
#include <sel4utils/process.h>
#include <sel4utils/process_config.h>
#include <simple/simple.h>
#include <simple-default/simple-default.h>
#include <utils/util.h>
#include <vka/capops.h>

#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 200)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];
static sel4utils_alloc_data_t vspace_data;

static simple_t simple;
static vka_t vka;
static vspace_t vspace;
static allocman_t *allocman;

#define UART0_PADDR 0x9000000UL
#define UART_DR   0x000
#define UART_FR   0x018
#define FR_RXFE   (1 << 4)

#define SER_KEY_PUSH 4

static int spawn_with_args(const char *name, uint8_t prio,
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

    /* Copy all endpoint caps and build argv */
    char argv_bufs[4][16];
    char *child_argv[4];
    for (int i = 0; i < ep_count && i < 4; i++) {
        child_slots[i] = sel4utils_copy_cap_to_process(proc, &vka, eps[i]);
        snprintf(argv_bufs[i], 16, "%lu", (unsigned long)child_slots[i]);
        child_argv[i] = argv_bufs[i];
    }

    return sel4utils_spawn_process_v(proc, &vka, &vspace,
                                      ep_count, child_argv, 1);
}

int main(int argc, char *argv[]) {
    int error;

    printf("\n");
    printf("============================================\n");
    printf("  AIOS 0.4.x Root Task — Phase 3c\n");
    printf("  Interactive Shell + UART Keyboard\n");
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
    printf("[boot] All subsystems OK\n");

    /* Map UART into root VSpace for keyboard polling */
    printf("[dev] Mapping UART at 0x%lx...\n", UART0_PADDR);
    volatile uint32_t *uart = NULL;
    {
        vka_object_t frame_obj;
        error = sel4platsupport_alloc_frame_at(&vka, UART0_PADDR,
                                                seL4_PageBits, &frame_obj);
        if (error) {
            printf("[dev] alloc_frame_at failed: %d\n", error);
        } else {
            /* Map device frame into our VSpace */
            void *vaddr = vspace_map_pages(&vspace,
                &frame_obj.cptr, NULL,
                seL4_AllRights, 1, seL4_PageBits, 0);
            if (vaddr) {
                uart = (volatile uint32_t *)vaddr;
                printf("[dev] UART mapped at %p\n", vaddr);
            } else {
                printf("[dev] vspace_map_pages failed\n");
            }
        }
    }
    if (!uart) printf("[dev] WARNING: No keyboard (UART not mapped)\n");

    /* Allocate endpoints */
    vka_object_t fault_ep, serial_ep;
    vka_alloc_endpoint(&vka, &fault_ep);
    vka_alloc_endpoint(&vka, &serial_ep);

    /* Lower root priority */
    seL4_TCB_SetPriority(seL4_CapInitThreadTCB,
                         seL4_CapInitThreadTCB, 200);

    /* Spawn serial_server (gets serial_ep) */
    printf("[proc] Spawning serial_server...\n");
    sel4utils_process_t serial_proc;
    seL4_CPtr ser_caps[1]; seL4_CPtr ser_slots[1];
    ser_caps[0] = serial_ep.cptr;
    error = spawn_with_args("serial_server", 200, &serial_proc,
                            &fault_ep, 1, ser_caps, ser_slots);
    if (error) { printf("[proc] serial FAILED: %d\n", error); goto idle; }
    printf("[proc] serial_server OK\n");

    /* Spawn mini_shell (gets serial_ep) */
    printf("[proc] Spawning mini_shell...\n");
    sel4utils_process_t shell_proc;
    seL4_CPtr sh_caps[1]; seL4_CPtr sh_slots[1];
    sh_caps[0] = serial_ep.cptr;
    error = spawn_with_args("mini_shell", 200, &shell_proc,
                            &fault_ep, 1, sh_caps, sh_slots);
    if (error) { printf("[proc] shell FAILED: %d\n", error); goto idle; }
    printf("[proc] mini_shell OK\n");
    printf("\n[root] System ready. Type commands.\n\n");

    /* Poll UART, push keyboard chars to serial_server */
    while (1) {
        if (uart && !(uart[UART_FR / 4] & FR_RXFE)) {
            char c = (char)(uart[UART_DR / 4] & 0xFF);
            seL4_SetMR(0, (seL4_Word)c);
            seL4_Call(serial_ep.cptr,
                      seL4_MessageInfo_new(SER_KEY_PUSH, 0, 0, 1));
        }
        seL4_Yield();
    }

idle:
    printf("[root] Idle\n");
    while (1) { seL4_Yield(); }
    return 0;
}
