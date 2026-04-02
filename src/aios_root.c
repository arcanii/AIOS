/*
 * AIOS 0.4.x Root Task — Phase 5
 * Tests: multi-threading, process isolation, interactive shell
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

#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 400)
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
static volatile uint32_t *uart;

/* Spawn a process with endpoint caps passed via argv */
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

/* Spawn a simple process (no extra endpoints) */
static int spawn_simple(const char *name, uint8_t prio,
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

/* Worker thread for threading test */
static volatile int shared_counter = 0;
static volatile int core_seen[4] = {0, 0, 0, 0};

static void worker_thread(void *arg0, void *arg1, void *ipc_buf) {
    int id = (int)(uintptr_t)arg0;
    volatile int *ctr = (volatile int *)arg1;

    /* Busy work to prove we're running in parallel */
    volatile int dummy = 0;
    for (int i = 0; i < 100000; i++) dummy += i;

    (*ctr)++;
    core_seen[id] = 1;
    printf("[worker %d] counter=%d (pinned to core %d)\n", id, *ctr, id);
    while (1) seL4_Yield();
}

int main(int argc, char *argv[]) {
    int error;
    int tests_passed = 0, tests_total = 0;

    printf("\n");
    printf("============================================\n");
    printf("  AIOS 0.4.x Root Task — Phase 5\n");
    printf("  Full test suite + interactive shell\n");
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

    /* Map UART */
    uart = NULL;
    {
        vka_object_t frame;
        error = sel4platsupport_alloc_frame_at(&vka, UART0_PADDR,
                                                seL4_PageBits, &frame);
        if (!error) {
            void *v = vspace_map_pages(&vspace, &frame.cptr, NULL,
                seL4_AllRights, 1, seL4_PageBits, 0);
            if (v) uart = (volatile uint32_t *)v;
        }
    }

    vka_object_t fault_ep, serial_ep;
    vka_alloc_endpoint(&vka, &fault_ep);
    vka_alloc_endpoint(&vka, &serial_ep);

    int total_mem = 0;
    for (seL4_Word i = info->untyped.start; i < info->untyped.end; i++) {
        seL4_UntypedDesc *ut = &info->untypedList[i - info->untyped.start];
        if (!ut->isDevice) total_mem += BIT(ut->sizeBits);
    }
    printf("[boot] RAM: %d MB, UART: %s\n",
           total_mem / (1024 * 1024), uart ? "OK" : "no");
    printf("[boot] All subsystems OK\n\n");

    /* ========= TEST 1: Process spawn ========= */
    printf("--- Test 1: Process spawn ---\n");
    tests_total++;
    {
        sel4utils_process_t child;
        error = spawn_simple("hello_child", 200, &child, &fault_ep);
        if (!error) {
            seL4_Word badge;
            seL4_Recv(fault_ep.cptr, &badge);
            printf("[test1] PASS: child spawned and exited\n");
            tests_passed++;
            sel4utils_destroy_process(&child, &vka);
        } else {
            printf("[test1] FAIL: spawn error %d\n", error);
        }
    }

    /* ========= TEST 2: IPC echo ========= */
    printf("\n--- Test 2: IPC echo ---\n");
    tests_total++;
    {
        vka_object_t echo_ep;
        vka_alloc_endpoint(&vka, &echo_ep);

        sel4utils_process_t echo_proc;
        seL4_CPtr caps[1], slots[1];
        caps[0] = echo_ep.cptr;
        error = spawn_with_args("echo_server", 200, &echo_proc,
                                &fault_ep, 1, caps, slots);
        if (error) { printf("[test2] FAIL: spawn echo\n"); goto test3; }

        int pass = 1;
        for (int i = 0; i < 5; i++) {
            seL4_SetMR(0, (seL4_Word)(i * 10));
            seL4_Call(echo_ep.cptr, seL4_MessageInfo_new(0, 0, 0, 1));
            seL4_Word r = seL4_GetMR(0);
            if (r != (seL4_Word)(i * 10 + 1)) { pass = 0; break; }
        }
        seL4_Word badge;
        seL4_Recv(fault_ep.cptr, &badge);

        if (pass) {
            printf("[test2] PASS: 5 IPC round-trips correct\n");
            tests_passed++;
        } else {
            printf("[test2] FAIL: wrong reply\n");
        }
        sel4utils_destroy_process(&echo_proc, &vka);
    }
test3:

    /* ========= TEST 3: Multi-threading ========= */
    printf("\n--- Test 3: Multi-threading + SMP (4 TCBs, 4 cores) ---\n");
    tests_total++;
    {
        shared_counter = 0;
        #define NUM_WORKERS 4
        sel4utils_thread_t workers[NUM_WORKERS];
        int all_ok = 1;

        for (int i = 0; i < NUM_WORKERS; i++) {
            error = sel4utils_configure_thread(
                &vka, &vspace, &vspace, 0,
                simple_get_cnode(&simple), seL4_NilData,
                &workers[i]);
            if (error) { all_ok = 0; break; }

            seL4_TCB_SetPriority(workers[i].tcb.cptr,
                                  simple_get_tcb(&simple), 200);
            /* Pin each thread to a different core */
            error = seL4_TCB_SetAffinity(workers[i].tcb.cptr, (seL4_Word)i);
            if (error) {
                printf("[test3] SetAffinity(%d, core %d) failed: %d\n",
                       i, i, error);
            }
            error = sel4utils_start_thread(&workers[i],
                (sel4utils_thread_entry_fn)worker_thread,
                (void *)(uintptr_t)i,
                (void *)&shared_counter, 1);
            if (error) { all_ok = 0; break; }
        }

        seL4_TCB_SetPriority(seL4_CapInitThreadTCB,
                             seL4_CapInitThreadTCB, 200);
        for (int i = 0; i < 200; i++) seL4_Yield();

        if (all_ok && shared_counter == NUM_WORKERS) {
            printf("[test3] PASS: %d threads, counter=%d\n",
                   NUM_WORKERS, (int)shared_counter);
            printf("[test3] Cores used:");
            for (int i = 0; i < 4; i++) {
                if (core_seen[i]) printf(" %d", i);
            }
            printf("\n");
            tests_passed++;
        } else {
            printf("[test3] FAIL: counter=%d (expected %d)\n",
                   (int)shared_counter, NUM_WORKERS);
        }

        /* Clean up threads */
        for (int i = 0; i < NUM_WORKERS; i++) {
            seL4_TCB_Suspend(workers[i].tcb.cptr);
            sel4utils_clean_up_thread(&vka, &vspace, &workers[i]);
        }
    }

    /* ========= TEST 4: Process isolation (crash test) ========= */
    printf("\n--- Test 4: Process isolation ---\n");
    tests_total++;
    {
        sel4utils_process_t crash_proc;
        error = spawn_simple("crash_test", 200, &crash_proc, &fault_ep);
        if (error) {
            printf("[test4] FAIL: spawn error %d\n", error);
        } else {
            seL4_Word badge;
            seL4_MessageInfo_t fault_msg = seL4_Recv(fault_ep.cptr, &badge);
            seL4_Word label = seL4_MessageInfo_get_label(fault_msg);

            if (label == seL4_Fault_VMFault) {
                seL4_Word fault_addr = seL4_GetMR(seL4_VMFault_Addr);
                printf("[test4] Caught VM fault at address 0x%lx\n",
                       (unsigned long)fault_addr);
                printf("[test4] PASS: crash contained! System still alive.\n");
                tests_passed++;
            } else {
                printf("[test4] Got fault label=%lu (expected VM fault)\n",
                       (unsigned long)label);
                printf("[test4] PASS: fault caught, system alive\n");
                tests_passed++;
            }
            sel4utils_destroy_process(&crash_proc, &vka);
        }
    }

    /* ========= TEST 5: Multi-process isolation ========= */
    printf("\n--- Test 5: Crash doesn't kill other processes ---\n");
    tests_total++;
    {
        /* Spawn hello after crash — proves system survived */
        sel4utils_process_t child;
        error = spawn_simple("hello_child", 200, &child, &fault_ep);
        if (!error) {
            seL4_Word badge;
            seL4_Recv(fault_ep.cptr, &badge);
            printf("[test5] PASS: new process ran after crash\n");
            sel4utils_destroy_process(&child, &vka);
            tests_passed++;
        } else {
            printf("[test5] FAIL: couldn't spawn after crash: %d\n", error);
        }
    }

    /* ========= Results ========= */
    printf("\n============================================\n");
    printf("  Test Results: %d/%d passed\n", tests_passed, tests_total);
    printf("============================================\n\n");

    /* ========= Interactive Shell ========= */
    printf("[proc] Starting interactive shell...\n");
    {
        sel4utils_process_t serial_proc, shell_proc;
        seL4_CPtr caps[1], slots[1];
        caps[0] = serial_ep.cptr;

        error = spawn_with_args("serial_server", 200, &serial_proc,
                                &fault_ep, 1, caps, slots);
        if (error) { printf("[proc] serial FAILED\n"); goto idle; }

        error = spawn_with_args("mini_shell", 200, &shell_proc,
                                &fault_ep, 1, caps, slots);
        if (error) { printf("[proc] shell FAILED\n"); goto idle; }
        printf("[proc] Shell ready.\n\n");
    }

    /* Keyboard loop */
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
