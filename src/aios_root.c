/*
 * AIOS 0.4.x Root Task — Phase 5
 * Tests: multi-threading, process isolation, interactive shell
 */
#include <stdio.h>
#include "aios/version.h"
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
#include "virtio.h"
#include "aios/ext2.h"
#include "aios/vfs.h"
#include "aios/procfs.h"
#define LOG_MODULE "root"
#define LOG_LEVEL LOG_LEVEL_DEBUG
#include "aios/aios_log.h"
#include "aios/root_shared.h"
#include "aios/vka_audit.h"
#include <elf/elf.h>
#include <sel4utils/elf.h>
#include <sel4utils/api.h>

#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 4000)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];
static sel4utils_alloc_data_t vspace_data;

simple_t simple;
vka_t vka;
vspace_t vspace;
allocman_t *allocman;

#define UART0_PADDR 0x9000000UL
#define UART_DR   0x000
#define UART_FR   0x018
#define FR_RXFE   (1 << 4)
volatile uint32_t *uart;

/* ── Thread management ── */


active_proc_t active_procs[MAX_ACTIVE_PROCS];
seL4_CPtr thread_ep_cap;
seL4_CPtr pipe_ep_cap;

/* ── Pipe management ── */


pipe_t pipes[MAX_PIPES];
seL4_CPtr auth_ep_cap;

/* Filesystem state (shared with fs thread) */
ext2_ctx_t ext2;
volatile uint32_t *blk_vio;
uint8_t *blk_dma;
uint64_t blk_dma_pa;
seL4_CPtr fs_ep_cap;
vka_object_t serial_ep;
uint32_t aios_total_mem = 0;

char elf_buf[8 * 1024 * 1024]; /* shared between exec_thread, fork, exec */

/* Foreground process tracking (for Ctrl-C) */
volatile int fg_pid = -1;
volatile seL4_CPtr fg_fault_ep = 0;
volatile int fg_killed = 0;

/* ── File permission check ── */
/* PSCI shutdown -- seL4_DebugHalt stops QEMU cleanly */
void aios_system_shutdown(void) {
    printf("\n");
    printf("============================================\n");
    printf("  AIOS shutdown complete\n");
    printf("============================================\n");
#ifdef CONFIG_DEBUG_BUILD
    seL4_DebugHalt();
#endif
    /* Fallback: halt CPU if debug syscall unavailable */
    while (1) { asm volatile("wfi"); }
}

int main(int argc, char *argv[]) {
    int error;
    (void)argc; (void)argv;

    printf("\n");
    printf("============================================\n");
    printf("  " AIOS_VERSION_FULL "\n");
    printf("============================================\n\n");

    /* Phase 1: Core init (bootinfo, allocman, vspace) */
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

    aios_log_init();

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

    vka_object_t fault_ep;
    vka_audit_endpoint(VKA_SUB_BOOT);
    vka_alloc_endpoint(&vka, &fault_ep);
    vka_audit_endpoint(VKA_SUB_BOOT);
    vka_alloc_endpoint(&vka, &serial_ep);

    /* Memory accounting */
    int total_mem = 0;
    for (seL4_Word i = info->untyped.start; i < info->untyped.end; i++) {
        seL4_UntypedDesc *ut = &info->untypedList[i - info->untyped.start];
        if (!ut->isDevice) total_mem += BIT(ut->sizeBits);
    }
    aios_total_mem = (uint32_t)total_mem;
    printf("[boot] RAM: %d MB, UART: %s\n",
           total_mem / (1024 * 1024), uart ? "OK" : "no");
    AIOS_LOG_INFO_V("RAM available: ", total_mem / (1024 * 1024));
    AIOS_LOG_INFO("All subsystems OK");
    printf("[boot] All subsystems OK\n\n");

    /* Init process table */
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++) active_procs[i].active = 0;
    int root_pid = proc_add("root", 200);
    for (int pi = 0; pi < PROC_MAX; pi++) {
        if (proc_table[pi].active && proc_table[pi].pid == root_pid)
            proc_table[pi].threads = 1;
    }
    seL4_TCB_SetPriority(seL4_CapInitThreadTCB,
                         seL4_CapInitThreadTCB, 200);

    /* Phase 2: Filesystem (virtio-blk + ext2 + VFS) */
    boot_fs_init();

    /* Phase 3: Server threads + process spawning */
    boot_start_services(&fault_ep);

    /* Main loop: keyboard polling + exec requests */
    while (1) {
        /* Poll UART for keyboard */
        if (uart && !(uart[UART_FR / 4] & FR_RXFE)) {
            char c = (char)(uart[UART_DR / 4] & 0xFF);
            if (c == 0x03 && fg_pid > 0) {
                /* Ctrl-C: signal foreground process to die */
                fg_killed = 1;
                seL4_CPtr kep = fg_fault_ep;
                /* Destroy child process (stops execution) */
                for (int ki = 0; ki < MAX_ACTIVE_PROCS; ki++) {
                    if (active_procs[ki].active && active_procs[ki].pid == fg_pid) {
                        sel4utils_destroy_process(&active_procs[ki].proc, &vka);
                        break;
                    }
                }
                /* Unblock exec_thread via fault EP */
                if (kep) {
                    seL4_SetMR(0, 0xDEAD);
                    seL4_Send(kep, seL4_MessageInfo_new(0, 0, 0, 1));
                }
                /* Push ^C to serial so shell sees it */
                seL4_SetMR(0, (seL4_Word)0x03);
                seL4_Call(serial_ep.cptr,
                          seL4_MessageInfo_new(SER_KEY_PUSH, 0, 0, 1));
            } else {
                seL4_SetMR(0, (seL4_Word)c);
                seL4_Call(serial_ep.cptr,
                          seL4_MessageInfo_new(SER_KEY_PUSH, 0, 0, 1));
            }
        }

        seL4_Yield();
    }

idle:
    printf("[root] Idle\n");
    while (1) { seL4_Yield(); }
    return 0;
}
