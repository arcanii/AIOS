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
#include "aios/hw_info.h"
#include "aios/vka_audit.h"
#include "aios/fb_console.h"
#include "aios/net.h"
#include "aios/gpu.h"
#include <elf/elf.h>
#include <sel4utils/elf.h>
#include <sel4utils/api.h>

#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 8000)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];
static sel4utils_alloc_data_t vspace_data;

simple_t simple;
vka_t vka;
vspace_t vspace;
allocman_t *allocman;

/* PL011 UART registers (QEMU virt) */
#define UART_DR   0x000
#define UART_FR   0x018
#define FR_RXFE   (1 << 4)
#define UART_IMSC 0x038   /* interrupt mask set/clear */
#define UART_ICR  0x044   /* interrupt clear */
#define IMSC_RXIM (1<<4)  /* RX interrupt mask bit */

#ifdef PLAT_RPI4
/* BCM2835 mini UART (auxiliary UART) registers.
 * On RPi4 without overlays/disable-bt.dtbo, GPIO 14/15 are
 * connected to mini UART (ALT5), NOT PL011. The seL4 kernel
 * also uses mini UART for debug output (bcm2835-aux-uart).
 * AUX block at 0xFE215000, mini UART regs at offset 0x40. */
#define MU_BASE   0x040   /* mini UART offset within AUX page */
#define MU_IO     (MU_BASE + 0x00)  /* data register */
#define MU_IER    (MU_BASE + 0x04)  /* interrupt enable */
#define MU_LSR    (MU_BASE + 0x14)  /* line status */
#define MU_CNTL   (MU_BASE + 0x20)  /* control: bit0=RX_EN, bit1=TX_EN */
#define MU_STAT   (MU_BASE + 0x24)  /* extra status */
#define LSR_DATA_READY (1 << 0)     /* RX data available */
#define LSR_TX_EMPTY   (1 << 5)     /* TX FIFO can accept data */
/* Mini UART MMIO address (AUX block base, kernel marks userAvailable) */
#define RPI4_AUX_PADDR 0xFE215000UL
#endif
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

/* Log drive (second virtio-blk device) */
ext2_ctx_t ext2_log;
volatile uint32_t *blk_vio_log;
uint8_t *blk_dma_log;
uint64_t blk_dma_pa_log;
seL4_CPtr fs_ep_cap;
vka_object_t serial_ep;
uint32_t aios_total_mem = 0;

char elf_buf[8 * 1024 * 1024]; /* shared between exec_thread, fork, exec */

/* Foreground process tracking (for Ctrl-C) */
volatile int fg_pid = -1;
volatile seL4_CPtr fg_fault_ep = 0;
volatile int fg_killed = 0;
volatile int fg_sigint_sent = 0;  /* v0.4.85: root-side two-stage Ctrl-C */

/* Network state (virtio-net) */
uint8_t net_mac[6] = {0};
int net_available = 0;
seL4_CPtr net_ep_cap = 0;
seL4_CPtr net_drv_ntfn_cap = 0;
seL4_CPtr net_srv_ntfn_cap = 0;
struct net_rx_ring net_rx_ring;

/* Display state (virtio-gpu) */
volatile uint32_t *gpu_vio = NULL;
uint8_t *gpu_dma = NULL;
uint64_t gpu_dma_pa = 0;
uint32_t *gpu_fb = NULL;
uint64_t gpu_fb_pa = 0;
int gpu_available = 0;
int gpu_vio_slot = -1;
seL4_CPtr disp_ep_cap = 0;
seL4_CPtr crypto_ep_cap = 0;
static seL4_CPtr uart_irq_cap = 0;
static seL4_CPtr main_ntfn_cap = 0;
static int irq_uart_active = 0;
uint32_t gpu_width = 0;
uint32_t gpu_height = 0;

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

#ifdef PLAT_RPI4
    /* RPi4 LED diagnostic: 3 fast blinks = root task reached.
     * Visible on ACT LED (GPIO 42) without serial adapter. */
    {
        vka_object_t gpio_frame;
        int gerr = sel4platsupport_alloc_frame_at(&vka, 0xFE200000,
                                                   seL4_PageBits, &gpio_frame);
        if (!gerr) {
            volatile uint32_t *gpio = vspace_map_pages(&vspace,
                &gpio_frame.cptr, NULL, seL4_AllRights, 1, seL4_PageBits, 0);
            if (gpio) {
                uint32_t val = gpio[0x10/4];
                val &= ~(7U << 6);
                val |= (1U << 6);
                gpio[0x10/4] = val;
                for (int blink = 0; blink < 3; blink++) {
                    gpio[0x20/4] = (1U << 10);
                    for (volatile int d = 0; d < 500000; d++) {}
                    gpio[0x2C/4] = (1U << 10);
                    for (volatile int d = 0; d < 500000; d++) {}
                }
                gpio[0x20/4] = (1U << 10);
            }
        }
    }
#endif

    /* Parse device tree for hardware discovery */
    boot_dtb_init();
    boot_hw_report();

    /* Map UART */
    uart = NULL;
    {
        vka_object_t frame;
#ifdef PLAT_RPI4
        /* RPi4: use mini UART (AUX block at 0xFE215000).
         * Without overlays/disable-bt.dtbo on SD card, GPIO 14/15
         * are ALT5 = mini UART. PL011 is not connected to GPIO.
         * The seL4 kernel also uses mini UART (userAvailable=true). */
        uint64_t uart_paddr = RPI4_AUX_PADDR;
#else
        uint64_t uart_paddr = hw_info.uart_paddr;
#endif
        error = sel4platsupport_alloc_frame_at(&vka, uart_paddr,
                                                seL4_PageBits, &frame);
        if (!error) {
            void *v = vspace_map_pages(&vspace, &frame.cptr, NULL,
                seL4_AllRights, 1, seL4_PageBits, 0);
            if (v) {
                uart = (volatile uint32_t *)v;
#ifdef PLAT_RPI4
                /* Enable mini UART RX + TX */
                uart[MU_CNTL / 4] = 0x03;  /* RX_EN | TX_EN */
                /* Drain stale data */
                while (uart[MU_LSR / 4] & LSR_DATA_READY)
                    (void)uart[MU_IO / 4];
                printf("[boot] Mini UART mapped at 0x%lx\n",
                       (unsigned long)uart_paddr);
#endif
            }
        }
        if (!uart) printf("[boot] UART map failed\n");
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

    /* Phase 2b: Network (optional virtio-net) */
    boot_net_init();

    /* Phase 2c: Display (optional framebuffer) */
    boot_display_init();

    /* Boot status on HDMI console (for RPi4 without serial adapter) */
    fb_console_printf("AIOS %s\n", AIOS_VERSION_STR);
    fb_console_printf("[boot] RAM: %lu MB, CPU: %s\n",
                      (unsigned long)(aios_total_mem / (1024 * 1024)),
                      hw_info.cpu_compat);
    fb_console_printf("[boot] UART: %s (0x%lx IRQ %u)\n",
                      uart ? "OK" : "no",
                      (unsigned long)hw_info.uart_paddr,
                      hw_info.uart_irq);
    if (hw_info.has_emmc)
        fb_console_printf("[dtb]  eMMC: 0x%lx IRQ %u\n",
                          (unsigned long)hw_info.emmc_paddr,
                          hw_info.emmc_irq);
    if (hw_info.has_genet)
        fb_console_printf("[dtb]  GENET: 0x%lx IRQ %u\n",
                          (unsigned long)hw_info.genet_paddr,
                          hw_info.genet_irq);
    if (hw_info.has_vc_mbox)
        fb_console_printf("[dtb]  VC mbox: 0x%lx\n",
                          (unsigned long)hw_info.vc_mbox_paddr);
    fb_console_printf("[fs]   %s\n",
                      ext2.read_sector ? "ext2 mounted" : "no filesystem");
    fb_console_printf("[net]  %s\n",
                      net_available ? "OK" : "not available");
    fb_console_printf("[gpu]  %s\n",
                      gpu_available ? "OK" : "not available");
    fb_console_printf("\n");

    /* Phase 3: Server threads + process spawning */
    boot_start_services(&fault_ep);
    fb_console_printf("[boot] Services started\n");

    /* --- UART IRQ + notification setup --- */
    {
        vka_object_t ntfn_obj;
        int ierr = vka_alloc_notification(&vka, &ntfn_obj);
        if (!ierr) {
            main_ntfn_cap = ntfn_obj.cptr;
            /* NOTE: do NOT bind notification to TCB -- bound notifications
             * interfere with seL4_Call (SER_KEY_PUSH IPC) by returning
             * the notification badge instead of the expected reply.
             * seL4_Wait on an unbound notification works fine. */
        }
        if (main_ntfn_cap && uart) {
#ifdef PLAT_RPI4
            /* RPi4 mini UART: use polling mode.
             * Mini UART IRQ is shared with AUX SPI and requires
             * different setup. Polling works fine for interactive use. */
            printf("[boot] UART: mini UART polling mode\n");
#else
            /* QEMU PL011: IRQ-driven input */
            cspacepath_t irq_path;
            ierr = vka_cspace_alloc_path(&vka, &irq_path);
            if (!ierr) {
                ierr = simple_get_IRQ_handler(&simple, hw_info.uart_irq, irq_path);
                if (!ierr) {
                    uart_irq_cap = irq_path.capPtr;
                    ierr = seL4_IRQHandler_SetNotification(
                        uart_irq_cap, main_ntfn_cap);
                    if (!ierr) {
                        uart[UART_IMSC / 4] |= IMSC_RXIM;
                        seL4_IRQHandler_Ack(uart_irq_cap);
                        irq_uart_active = 1;
                        printf("[boot] UART IRQ %u bound (notification)\n",
                               hw_info.uart_irq);
                    } else {
                        printf("[boot] IRQ SetNotification failed: %d\n", ierr);
                    }
                } else {
                    printf("[boot] UART IRQ handler failed: %d\n", ierr);
                }
            }
#endif
        }
        if (!irq_uart_active)
            printf("[boot] UART: polling mode (IRQ unavailable)\n");
    }

    /* Main loop: event-driven or polling fallback */
    while (1) {
        /* Poll UART for keyboard -- drain FIFO in burst for paste */
        int uart_batch = 0;
#ifdef PLAT_RPI4
        while (uart && (uart[MU_LSR / 4] & LSR_DATA_READY) && uart_batch < 64) {
            uart_batch++;
            char c = (char)(uart[MU_IO / 4] & 0xFF);
#else
        while (uart && !(uart[UART_FR / 4] & FR_RXFE) && uart_batch < 64) {
            uart_batch++;
            char c = (char)(uart[UART_DR / 4] & 0xFF);
#endif
            if (c == 0x03 && fg_pid > 0) {
                /* Ctrl-C: two-stage SIGINT delivery.
                 * First ^C: set SIGINT pending, process self-terminates
                 * via SIG_DFL at its next signal check point.
                 * Second ^C: force-destroy (fallback for caught/ignored
                 * SIGINT). Uses root-side flag so sig_check clearing
                 * sig_pending does not defeat the two-stage mechanism. */
                int ki;
                for (ki = 0; ki < MAX_ACTIVE_PROCS; ki++) {
                    if (active_procs[ki].active &&
                        active_procs[ki].pid == fg_pid)
                        break;
                }
                if (ki < MAX_ACTIVE_PROCS) {
                    if (fg_sigint_sent) {
                        /* Second ^C: send SIGKILL via pipe_server.
                         * pipe_server handles SIGKILL properly: calls
                         * handle_child_fault which reaps, wakes PIPE_WAIT,
                         * and cleans up capabilities safely. */
                        fg_sigint_sent = 0;
                        seL4_SetMR(0, (seL4_Word)fg_pid);
                        seL4_SetMR(1, (seL4_Word)9);  /* SIGKILL */
                        seL4_Call(pipe_ep_cap,
                            seL4_MessageInfo_new(75, 0, 0, 2));
                    } else {
                        /* First ^C: send SIGINT via pipe_server so it
                         * can wake blocked PIPE_READ readers (v0.4.87) */
                        fg_sigint_sent = 1;
                        seL4_SetMR(0, (seL4_Word)fg_pid);
                        seL4_SetMR(1, (seL4_Word)2);  /* SIGINT */
                        seL4_Call(pipe_ep_cap,
                            seL4_MessageInfo_new(75, 0, 0, 2));
                    }
                }
                /* Push ^C to serial (unblocks TTY_READ, shows ^C) */
                seL4_SetMR(0, (seL4_Word)0x03);
                seL4_Call(serial_ep.cptr,
                          seL4_MessageInfo_new(SER_KEY_PUSH, 0, 0, 1));
            } else {
                seL4_SetMR(0, (seL4_Word)c);
                seL4_Call(serial_ep.cptr,
                          seL4_MessageInfo_new(SER_KEY_PUSH, 0, 0, 1));
            }
        }

        /* virtio-net IRQ now handled by dedicated seL4 IRQ handler
         * (bound in boot_net_init.c, acked in net_driver.c) */

        /* ACK UART IRQ if active (re-arms for next delivery) */
        if (irq_uart_active && uart_irq_cap) {
            seL4_IRQHandler_Ack(uart_irq_cap);
        }

        /* Sleep only if FIFO is empty.
         * QEMU PL011 bug: writing ICR clears RX flag even when
         * data remains in FIFO, and the flag only re-asserts on
         * NEW data via pl011_put_fifo.  If we sleep with data in
         * the FIFO, the main loop blocks forever.  Fix: skip the
         * ICR write (unnecessary for level-triggered RX) and
         * check RXFE before sleeping. */
        if (irq_uart_active && main_ntfn_cap) {
#ifdef PLAT_RPI4
            if (uart && (uart[MU_LSR / 4] & LSR_DATA_READY)) {
#else
            if (uart && !(uart[UART_FR / 4] & FR_RXFE)) {
#endif
                seL4_Yield();  /* FIFO has data -- drain next iter */
            } else {
                seL4_Wait(main_ntfn_cap, NULL);  /* FIFO empty -- sleep */
            }
        } else {
            seL4_Yield();  /* fallback: busy-poll */
        }
    }

idle:
    printf("[root] Idle\n");
    while (1) { seL4_Yield(); }
    return 0;
}
