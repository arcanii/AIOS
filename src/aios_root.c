/*
 * AIOS 0.4.x Root Task — Phase 4
 * Multi-threading: multiple TCBs in one VSpace
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
#include <sel4utils/thread.h>
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

/* PL011 UART */
#define UART0_PADDR 0x9000000UL
#define UART_DR   0x000
#define UART_FR   0x018
#define FR_RXFE   (1 << 4)
#define SER_PUTC     1
#define SER_GETC     2
#define SER_KEY_PUSH 4
static volatile uint32_t *uart;

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

/* ========= Phase 4: Worker thread function ========= */
static void worker_thread(void *arg0, void *arg1, void *ipc_buf) {
    int id = (int)(uintptr_t)arg0;
    volatile int *counter = (volatile int *)arg1;

    printf("[worker %d] Started! Incrementing counter...\n", id);
    /* Atomic-ish increment (single core, so safe) */
    (*counter)++;
    printf("[worker %d] Done. Counter now = %d\n", id, *counter);

    /* Done — just spin. Root will continue after yields. */
    while (1) seL4_Yield();
}

int main(int argc, char *argv[]) {
    int error;

    printf("\n");
    printf("============================================\n");
    printf("  AIOS 0.4.x Root Task — Phase 4\n");
    printf("  Multi-threading + Interactive Shell\n");
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

    /* Map UART */
    uart = NULL;
    for (seL4_Word i = info->untyped.start; i < info->untyped.end; i++) {
        seL4_UntypedDesc *ut = &info->untypedList[i - info->untyped.start];
        if (ut->isDevice && ut->paddr == UART0_PADDR) {
            vka_object_t frame;
            error = sel4platsupport_alloc_frame_at(&vka, UART0_PADDR,
                                                    seL4_PageBits, &frame);
            if (!error) {
                void *v = vspace_map_pages(&vspace, &frame.cptr, NULL,
                    seL4_AllRights, 1, seL4_PageBits, 0);
                if (v) uart = (volatile uint32_t *)v;
            }
            break;
        }
    }
    printf("[dev] UART: %s\n", uart ? "OK" : "not mapped");

    /* Endpoints */
    vka_object_t fault_ep, serial_ep;
    vka_alloc_endpoint(&vka, &fault_ep);
    vka_alloc_endpoint(&vka, &serial_ep);

    /* ========= Phase 4: Thread Test ========= */
    printf("\n[phase4] === Multi-threading test ===\n\n");

    /* Create 4 worker threads in root task's VSpace */
    volatile int shared_counter = 0;
    #define NUM_WORKERS 4
    sel4utils_thread_t workers[NUM_WORKERS];

    for (int i = 0; i < NUM_WORKERS; i++) {
        error = sel4utils_configure_thread(
            &vka, &vspace, &vspace,
            0,  /* no fault endpoint */
            simple_get_cnode(&simple),
            seL4_NilData,
            &workers[i]);
        if (error) {
            printf("[phase4] Thread %d configure failed: %d\n", i, error);
            continue;
        }

        error = seL4_TCB_SetPriority(workers[i].tcb.cptr,
                                      simple_get_tcb(&simple), 200);
        if (error) {
            printf("[phase4] Thread %d set priority failed: %d\n", i, error);
            continue;
        }

        error = sel4utils_start_thread(
            &workers[i],
            (sel4utils_thread_entry_fn)worker_thread,
            (void *)(uintptr_t)i,
            (void *)&shared_counter,
            1 /* resume */);
        if (error) {
            printf("[phase4] Thread %d start failed: %d\n", i, error);
            continue;
        }
        printf("[phase4] Thread %d created (TCB=%lu)\n",
               i, (unsigned long)workers[i].tcb.cptr);
    }

    /* Let workers run */
    printf("[phase4] Yielding to let workers run...\n");
    seL4_TCB_SetPriority(seL4_CapInitThreadTCB,
                         seL4_CapInitThreadTCB, 200);
    for (int i = 0; i < 100; i++) seL4_Yield();

    printf("[phase4] shared_counter = %d\n", (int)shared_counter);
    if (shared_counter == NUM_WORKERS) {
        printf("[phase4] SUCCESS: %d threads ran with real TCBs!\n\n", NUM_WORKERS);
    } else {
        printf("[phase4] PARTIAL: %d/%d threads completed\n\n",
               (int)shared_counter, NUM_WORKERS);
    }

    /* ========= Interactive Shell ========= */
    printf("[proc] Spawning serial_server + mini_shell...\n");

    sel4utils_process_t serial_proc, shell_proc;
    seL4_CPtr caps[1], slots[1];
    caps[0] = serial_ep.cptr;

    error = spawn_with_args("serial_server", 200, &serial_proc,
                            &fault_ep, 1, caps, slots);
    if (error) { printf("[proc] serial FAILED: %d\n", error); goto idle; }

    error = spawn_with_args("mini_shell", 200, &shell_proc,
                            &fault_ep, 1, caps, slots);
    if (error) { printf("[proc] shell FAILED: %d\n", error); goto idle; }
    printf("[proc] All up. Type commands.\n\n");

    /* Keyboard polling loop */
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
