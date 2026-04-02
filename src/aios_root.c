/*
 * AIOS 0.4.x Root Task — Phase 3c
 * Interactive shell via serial server with UART
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
#include <vka/capops.h>

#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 200)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];
static sel4utils_alloc_data_t vspace_data;

static simple_t simple;
static vka_t vka;
static vspace_t vspace;
static allocman_t *allocman;

#define UART0_PADDR 0x9000000
#define UART0_IRQ   33

int main(int argc, char *argv[]) {
    int error;

    printf("\n");
    printf("============================================\n");
    printf("  AIOS 0.4.x Root Task — Phase 3c\n");
    printf("  Interactive Shell + UART Serial Server\n");
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
    vka_object_t fault_ep, serial_ep;
    vka_alloc_endpoint(&vka, &fault_ep);
    vka_alloc_endpoint(&vka, &serial_ep);

    /* Get UART device frame cap */
    printf("[dev] Getting UART frame cap (0x%x)...\n", UART0_PADDR);
    vka_object_t uart_frame;
    error = vka_alloc_frame_at(&vka, seL4_PageBits, UART0_PADDR, &uart_frame);
    if (error) {
        printf("[dev] WARNING: vka_alloc_frame_at failed: %d\n", error);
        printf("[dev] Trying simple_get_frame_cap...\n");
        cspacepath_t frame_path;
        vka_cspace_alloc_path(&vka, &frame_path);
        error = simple_get_frame_cap(&simple, (void *)UART0_PADDR,
                                      seL4_PageBits, &frame_path);
        if (error) {
            printf("[dev] WARNING: UART frame not available, using DebugPutChar\n");
            uart_frame.cptr = 0;
        } else {
            uart_frame.cptr = frame_path.capPtr;
            printf("[dev] UART frame cap via simple: %lu\n",
                   (unsigned long)uart_frame.cptr);
        }
    } else {
        printf("[dev] UART frame cap: %lu\n", (unsigned long)uart_frame.cptr);
    }

    /* Get UART IRQ handler cap */
    printf("[dev] Getting IRQ handler for IRQ %d...\n", UART0_IRQ);
    cspacepath_t irq_path;
    vka_cspace_alloc_path(&vka, &irq_path);
    error = simple_get_IRQ_handler(&simple, UART0_IRQ, irq_path);
    seL4_CPtr irq_cap = 0;
    if (error) {
        printf("[dev] WARNING: IRQ handler failed: %d (continuing without)\n", error);
    } else {
        irq_cap = irq_path.capPtr;
        printf("[dev] IRQ handler cap: %lu\n", (unsigned long)irq_cap);
    }

    /* Lower root priority */
    seL4_TCB_SetPriority(seL4_CapInitThreadTCB,
                         seL4_CapInitThreadTCB, 50);

    /* Spawn serial server with device caps */
    printf("\n[proc] Spawning serial_server...\n");
    sel4utils_process_t serial_proc;
    {
        sel4utils_process_config_t config = process_config_new(&simple);
        config = process_config_elf(config, "serial_server", true);
        config = process_config_create_cnode(config, 12);
        config = process_config_create_vspace(config, NULL, 0);
        config = process_config_priority(config, 200);
        config = process_config_auth(config, simple_get_tcb(&simple));
        config = process_config_fault_endpoint(config, fault_ep);

        error = sel4utils_configure_process_custom(
            &serial_proc, &vka, &vspace, config);
        if (error) { printf("[proc] serial configure FAILED: %d\n", error); goto idle; }

        /* Copy caps into child's CSpace */
        seL4_CPtr child_ep = sel4utils_copy_cap_to_process(
            &serial_proc, &vka, serial_ep.cptr);
        seL4_CPtr child_uart = 0;
        if (uart_frame.cptr) {
            child_uart = sel4utils_copy_cap_to_process(
                &serial_proc, &vka, uart_frame.cptr);
        }
        seL4_CPtr child_irq = 0;
        if (irq_cap) {
            child_irq = sel4utils_copy_cap_to_process(
                &serial_proc, &vka, irq_cap);
        }

        printf("[proc] serial caps: ep=%lu uart=%lu irq=%lu\n",
               (unsigned long)child_ep,
               (unsigned long)child_uart,
               (unsigned long)child_irq);

        /* Pass slots as argv */
        char s_ep[16], s_uart[16], s_irq[16];
        snprintf(s_ep, 16, "%lu", (unsigned long)child_ep);
        snprintf(s_uart, 16, "%lu", (unsigned long)child_uart);
        snprintf(s_irq, 16, "%lu", (unsigned long)child_irq);
        char *ser_argv[] = { s_ep, s_uart, s_irq };
        error = sel4utils_spawn_process_v(
            &serial_proc, &vka, &vspace, 3, ser_argv, 1);
        if (error) { printf("[proc] serial spawn FAILED: %d\n", error); goto idle; }
        printf("[proc] serial_server OK\n");
    }

    /* Spawn mini shell — give it serial endpoint */
    printf("[proc] Spawning mini_shell...\n");
    sel4utils_process_t shell_proc;
    {
        sel4utils_process_config_t config = process_config_new(&simple);
        config = process_config_elf(config, "mini_shell", true);
        config = process_config_create_cnode(config, 12);
        config = process_config_create_vspace(config, NULL, 0);
        config = process_config_priority(config, 150);
        config = process_config_auth(config, simple_get_tcb(&simple));
        config = process_config_fault_endpoint(config, fault_ep);

        error = sel4utils_configure_process_custom(
            &shell_proc, &vka, &vspace, config);
        if (error) { printf("[proc] shell configure FAILED: %d\n", error); goto idle; }

        seL4_CPtr child_ep = sel4utils_copy_cap_to_process(
            &shell_proc, &vka, serial_ep.cptr);

        char s_ep[16];
        snprintf(s_ep, 16, "%lu", (unsigned long)child_ep);
        char *shell_argv[] = { s_ep };
        error = sel4utils_spawn_process_v(
            &shell_proc, &vka, &vspace, 1, shell_argv, 1);
        if (error) { printf("[proc] shell spawn FAILED: %d\n", error); goto idle; }
        printf("[proc] mini_shell OK\n");
    }

    printf("\n[root] All processes up. Waiting for shell exit...\n\n");

    /* Wait for shell to exit */
    {
        seL4_Word badge;
        seL4_Recv(fault_ep.cptr, &badge);
        printf("\n[root] Shell exited.\n");
    }

    printf("\n============================================\n");
    printf("  Phase 3c complete!\n");
    printf("============================================\n\n");

idle:
    printf("[root] Idle\n");
    while (1) { seL4_Yield(); }
    return 0;
}
