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
#include <elf/elf.h>
#include <sel4utils/elf.h>
#include <sel4utils/api.h>

#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 800)
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
static volatile uint32_t *uart;

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

char elf_buf[1024 * 1024]; /* shared between exec_thread, fork, exec */

/* Foreground process tracking (for Ctrl-C) */
volatile int fg_pid = -1;
volatile seL4_CPtr fg_fault_ep = 0;
volatile int fg_killed = 0;

static int blk_read_sector(uint64_t sector, void *buf);
static int blk_write_sector(uint64_t sector, const void *buf);

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
static int blk_read_sector(uint64_t sector, void *buf) {
    struct virtq_desc  *desc  = (struct virtq_desc *)(blk_dma);
    struct virtq_avail *avail = (struct virtq_avail *)(blk_dma + 0x100);
    struct virtq_used  *used  = (struct virtq_used  *)(blk_dma + 0x1000);
    struct virtio_blk_req *req = (struct virtio_blk_req *)(blk_dma + 0x2000);
    uint64_t req_pa = blk_dma_pa + 0x2000;

    req->type = VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = sector;
    req->status = 0xFF;

    desc[0].addr = req_pa; desc[0].len = 16;
    desc[0].flags = VIRTQ_DESC_F_NEXT; desc[0].next = 1;
    desc[1].addr = req_pa + 16; desc[1].len = 512;
    desc[1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT; desc[1].next = 2;
    desc[2].addr = req_pa + 16 + 512; desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE; desc[2].next = 0;

    __asm__ volatile("dmb sy" ::: "memory");
    avail->ring[avail->idx % 16] = 0;
    __asm__ volatile("dmb sy" ::: "memory");
    avail->idx += 1;
    __asm__ volatile("dmb sy" ::: "memory");
    blk_vio[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

    uint16_t last = used->idx;
    for (int t = 0; t < 10000000; t++) {
        __asm__ volatile("dmb sy" ::: "memory");
        if (used->idx != last) break;
    }
    blk_vio[VIRTIO_MMIO_INTERRUPT_ACK / 4] = blk_vio[VIRTIO_MMIO_INTERRUPT_STATUS / 4];

    if (used->idx == last || req->status != 0) return -1;
    uint8_t *src = req->data;
    uint8_t *dst = (uint8_t *)buf;
    for (int i = 0; i < 512; i++) dst[i] = src[i];
    return 0;
}

/* Write a 512-byte sector via virtio-blk */
static int blk_write_sector(uint64_t sector, const void *buf) {
    struct virtq_desc  *desc  = (struct virtq_desc *)(blk_dma);
    struct virtq_avail *avail = (struct virtq_avail *)(blk_dma + 0x100);
    struct virtq_used  *used  = (struct virtq_used  *)(blk_dma + 0x1000);
    struct virtio_blk_req *req = (struct virtio_blk_req *)(blk_dma + 0x2000);
    uint64_t req_pa = blk_dma_pa + 0x2000;

    req->type = VIRTIO_BLK_T_OUT;
    req->reserved = 0;
    req->sector = sector;
    req->status = 0xFF;

    /* Copy data into request buffer */
    const uint8_t *src = (const uint8_t *)buf;
    for (int i = 0; i < 512; i++) req->data[i] = src[i];

    /* Descriptor 0: header (device reads) */
    desc[0].addr = req_pa; desc[0].len = 16;
    desc[0].flags = VIRTQ_DESC_F_NEXT; desc[0].next = 1;
    /* Descriptor 1: data (device reads — NOT VIRTQ_DESC_F_WRITE) */
    desc[1].addr = req_pa + 16; desc[1].len = 512;
    desc[1].flags = VIRTQ_DESC_F_NEXT; desc[1].next = 2;
    /* Descriptor 2: status (device writes) */
    desc[2].addr = req_pa + 16 + 512; desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE; desc[2].next = 0;

    __asm__ volatile("dmb sy" ::: "memory");
    avail->ring[avail->idx % 16] = 0;
    __asm__ volatile("dmb sy" ::: "memory");
    avail->idx += 1;
    __asm__ volatile("dmb sy" ::: "memory");
    blk_vio[VIRTIO_MMIO_QUEUE_NOTIFY / 4] = 0;

    uint16_t last = used->idx;
    for (int t = 0; t < 10000000; t++) {
        __asm__ volatile("dmb sy" ::: "memory");
        if (used->idx != last) break;
    }
    blk_vio[VIRTIO_MMIO_INTERRUPT_ACK / 4] = blk_vio[VIRTIO_MMIO_INTERRUPT_STATUS / 4];

    if (used->idx == last || req->status != 0) return -1;
    return 0;
}


/* ── File permission check ── */
int main(int argc, char *argv[]) {
    int error;

    printf("\n");
    printf("============================================\n");
    printf("  " AIOS_VERSION_FULL "\n");
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

    /* Init kernel log */
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

    vka_alloc_endpoint(&vka, &fault_ep);
    vka_alloc_endpoint(&vka, &serial_ep);

    int total_mem = 0; /* also stored in aios_total_mem */
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

    /* Register system processes */
    /* Clear active process table */
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++) active_procs[i].active = 0;

    int root_pid = proc_add("root", 200);
    for (int pi = 0; pi < PROC_MAX; pi++) {
        if (proc_table[pi].active && proc_table[pi].pid == root_pid)
            proc_table[pi].threads = 1;
    }


    /* Set root to priority 200 for round-robin with children */
    seL4_TCB_SetPriority(seL4_CapInitThreadTCB,
                         seL4_CapInitThreadTCB, 200);


    /* ========= Filesystem init ========= */
    /* quiet */
    {
        #define VIRTIO_BASE_ADDR 0xa000000UL
        #define VIRTIO_SLOT_SIZE 0x200
        #define VIRTIO_NUM_SLOTS 32

        /* Map 4 pages of virtio MMIO */
        vka_object_t vio_frames[4];
        seL4_CPtr vio_caps[4];
        int vio_ok = 1;
        for (int p = 0; p < 4; p++) {
            error = sel4platsupport_alloc_frame_at(&vka,
                VIRTIO_BASE_ADDR + p * 0x1000, seL4_PageBits, &vio_frames[p]);
            if (error) { vio_ok = 0; break; }
            vio_caps[p] = vio_frames[p].cptr;
        }
        if (!vio_ok) {
            printf("[fs] Failed to alloc virtio frames\n");
            goto skip_blk;
        }
        void *vio_vaddr = vspace_map_pages(&vspace, vio_caps, NULL,
            seL4_AllRights, 4, seL4_PageBits, 0);
        if (!vio_vaddr) {
            printf("[fs] Failed to map virtio\n");
            goto skip_blk;
        }

        /* Find block device */
        int blk_slot = -1;
        for (int i = 0; i < VIRTIO_NUM_SLOTS; i++) {
            volatile uint32_t *slot = (volatile uint32_t *)((uintptr_t)vio_vaddr + i * VIRTIO_SLOT_SIZE);
            if (slot[0] == VIRTIO_MAGIC && slot[VIRTIO_MMIO_DEVICE_ID/4] == VIRTIO_BLK_DEVICE_ID) {
                blk_slot = i;
                break;
            }
        }
        if (blk_slot < 0) {
            printf("[fs] No block device (add -drive to QEMU)\n");
            goto skip_blk;
        }
        volatile uint32_t *vio = (volatile uint32_t *)((uintptr_t)vio_vaddr + blk_slot * VIRTIO_SLOT_SIZE);
        /* quiet */

        /* Allocate 16K contiguous DMA via single untyped */
        vka_object_t dma_ut;
        error = vka_alloc_untyped(&vka, 14, &dma_ut); /* 2^14 = 16K */
        if (error) {
            printf("[fs] DMA untyped alloc failed: %d\n", error);
            goto skip_blk;
        }

        /* Retype untyped into 4 contiguous frames */
        seL4_CPtr dma_caps[4];
        for (int i = 0; i < 4; i++) {
            seL4_CPtr slot;
            error = vka_cspace_alloc(&vka, &slot);
            if (error) { printf("[fs] DMA cslot alloc failed\n"); goto skip_blk; }
            error = seL4_Untyped_Retype(dma_ut.cptr,
                seL4_ARM_SmallPageObject, seL4_PageBits,
                seL4_CapInitThreadCNode, 0, 0, slot, 1);
            if (error) { printf("[fs] DMA retype %d failed: %d\n", i, error); goto skip_blk; }
            dma_caps[i] = slot;
        }

        /* Map DMA pages */
        void *dma_vaddr = vspace_map_pages(&vspace, dma_caps, NULL,
            seL4_AllRights, 4, seL4_PageBits, 0);
        if (!dma_vaddr) {
            printf("[fs] DMA map failed\n");
            goto skip_blk;
        }

        /* Get physical address — contiguous guaranteed by single untyped */
        seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_caps[0]);
        if (ga.error) { printf("[fs] DMA GetAddress failed\n"); goto skip_blk; }
        uint64_t dma_pa = ga.paddr;

        /* Zero DMA region */
        uint8_t *dma = (uint8_t *)dma_vaddr;
        for (int i = 0; i < 16384; i++) dma[i] = 0;

        /* Legacy virtio init */
        #define VIO_R(off) vio[(off)/4]
        #define VIO_W(off, val) vio[(off)/4] = (val)

        VIO_W(VIRTIO_MMIO_STATUS, 0);
        VIO_W(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
        VIO_W(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
        VIO_W(VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
        VIO_W(VIRTIO_MMIO_DRV_FEATURES, 0);
        VIO_W(VIRTIO_MMIO_QUEUE_SEL, 0);
        uint32_t qmax = VIO_R(VIRTIO_MMIO_QUEUE_NUM_MAX);
        uint32_t qsz = qmax < 16 ? qmax : 16;
        VIO_W(VIRTIO_MMIO_QUEUE_NUM, qsz);
        VIO_W(VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(dma_pa / 4096));
        VIO_W(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
        /* quiet */

        /* Layout: desc at 0, avail at 0x100, used at 0x1000, req at 0x2000 */
        struct virtq_desc  *desc  = (struct virtq_desc *)(dma);
        struct virtq_avail *avail = (struct virtq_avail *)(dma + 0x100);
        struct virtq_used  *used  = (struct virtq_used  *)(dma + 0x1000);
        struct virtio_blk_req *req = (struct virtio_blk_req *)(dma + 0x2000);
        uint64_t req_pa = dma_pa + 0x2000;

        /* Read sector 2 (ext2 superblock) */
        req->type = VIRTIO_BLK_T_IN;
        req->reserved = 0;
        req->sector = 2;
        req->status = 0xFF;

        desc[0].addr  = req_pa;
        desc[0].len   = 16;
        desc[0].flags = VIRTQ_DESC_F_NEXT;
        desc[0].next  = 1;

        desc[1].addr  = req_pa + 16;
        desc[1].len   = 512;
        desc[1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
        desc[1].next  = 2;

        desc[2].addr  = req_pa + 16 + 512;
        desc[2].len   = 1;
        desc[2].flags = VIRTQ_DESC_F_WRITE;
        desc[2].next  = 0;

        __asm__ volatile("dmb sy" ::: "memory");
        avail->ring[avail->idx % qsz] = 0;
        __asm__ volatile("dmb sy" ::: "memory");
        avail->idx += 1;
        __asm__ volatile("dmb sy" ::: "memory");

        VIO_W(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

        /* Poll for completion */
        uint16_t last_used = 0;
        int done = 0;
        for (int t = 0; t < 10000000; t++) {
            __asm__ volatile("dmb sy" ::: "memory");
            if (used->idx != last_used) { done = 1; break; }
        }

        VIO_R(VIRTIO_MMIO_INTERRUPT_STATUS);
        VIO_W(VIRTIO_MMIO_INTERRUPT_ACK, 1);

        if (!done) {
            printf("[fs] Read timeout\n");
            goto skip_blk;
        }
        if (req->status != 0) {
            printf("[fs] Read error status=%u\n", req->status);
            goto skip_blk;
        }

        /* Check ext2 magic at offset 0x38 in superblock */
        uint16_t ext2_magic = req->data[0x38] | (req->data[0x39] << 8);
        if (ext2_magic == 0xEF53) {
            /* quiet */
    
            /* Save virtio state for fs thread */
            blk_vio = vio;
            blk_dma = dma;
            blk_dma_pa = dma_pa;

            /* Init ext2 */
            vfs_init();
            proc_init();
            int fs_err = ext2_init(&ext2, blk_read_sector);
            if (fs_err == 0) {
                ext2_init_write(&ext2, blk_write_sector);
                vfs_mount("/", &ext2_fs_ops, &ext2);
                vfs_mount("/proc", &procfs_ops, NULL);
                proc_add("fs_thread", 200);
                proc_add("exec_thread", 200);
                proc_add("thread_server", 200);
                LOG_INFO("ext2 + procfs mounted");

                printf("[boot] Filesystems mounted\n");
            } else {
                printf("[fs] ext2 init failed: %d\n", fs_err);
            }
        } else {
            printf("[fs] ext2 not found (got 0x%04x)\n", ext2_magic);
        }
    }
skip_blk:


    /* ========= Interactive Shell ========= */
    /* Create filesystem endpoint + thread */
    vka_object_t fs_ep_obj;
    vka_alloc_endpoint(&vka, &fs_ep_obj);
    fs_ep_cap = fs_ep_obj.cptr;

    vka_object_t exec_ep_obj;
    vka_alloc_endpoint(&vka, &exec_ep_obj);
    seL4_CPtr exec_ep_cap = exec_ep_obj.cptr;

    /* Thread server endpoint */
    vka_object_t thread_ep_obj;
    vka_alloc_endpoint(&vka, &thread_ep_obj);
    thread_ep_cap = thread_ep_obj.cptr;

    /* Auth server endpoint */
    vka_object_t auth_ep_obj;
    vka_alloc_endpoint(&vka, &auth_ep_obj);
    auth_ep_cap = auth_ep_obj.cptr;
    {
        sel4utils_thread_t fs_thread;
        error = sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
            simple_get_cnode(&simple), seL4_NilData, &fs_thread);
        if (!error) {
            seL4_TCB_SetPriority(fs_thread.tcb.cptr, simple_get_tcb(&simple), 200);
            sel4utils_start_thread(&fs_thread,
                (sel4utils_thread_entry_fn)fs_thread_fn,
                (void *)(uintptr_t)fs_ep_cap, NULL, 1);
            /* quiet */
        }
    }

    /* Start exec thread */
    {
        sel4utils_thread_t exec_thread;
        error = sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
            simple_get_cnode(&simple), seL4_NilData, &exec_thread);
        if (!error) {
            seL4_TCB_SetPriority(exec_thread.tcb.cptr, simple_get_tcb(&simple), 200);
            sel4utils_start_thread(&exec_thread,
                (sel4utils_thread_entry_fn)exec_thread_fn,
                (void *)(uintptr_t)exec_ep_cap, NULL, 1);
            /* quiet */
        }
    }

    /* Start thread server */
    {
        sel4utils_thread_t tsrv_thread;
        error = sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
            simple_get_cnode(&simple), seL4_NilData, &tsrv_thread);
        if (!error) {
            seL4_TCB_SetPriority(tsrv_thread.tcb.cptr,
                                  simple_get_tcb(&simple), 200);
            LOG_INFO("Thread server started");
            sel4utils_start_thread(&tsrv_thread,
                (sel4utils_thread_entry_fn)thread_server_fn,
                (void *)(uintptr_t)thread_ep_cap, NULL, 1);
        }
    }

    /* Pipe server endpoint + thread */
    vka_object_t pipe_ep_obj;
    vka_alloc_endpoint(&vka, &pipe_ep_obj);
    pipe_ep_cap = pipe_ep_obj.cptr;
    {
        sel4utils_thread_t pipe_thread;
        error = sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
            simple_get_cnode(&simple), seL4_NilData, &pipe_thread);
        if (!error) {
            seL4_TCB_SetPriority(pipe_thread.tcb.cptr,
                                  simple_get_tcb(&simple), 200);
            sel4utils_start_thread(&pipe_thread,
                (sel4utils_thread_entry_fn)pipe_server_fn,
                (void *)(uintptr_t)pipe_ep_cap, NULL, 1);
            proc_add("pipe_server", 200);
        }
    }


    /* quiet */
    {
        sel4utils_process_t serial_proc;
        seL4_CPtr caps[1], slots[1];
        caps[0] = serial_ep.cptr;

        error = spawn_with_args("tty_server", 200, &serial_proc,
                                &fault_ep, 1, caps, slots);
        if (error) { printf("[proc] tty FAILED\n"); goto idle; }
        proc_add("tty_server", 200);

        /* Spawn auth_server as isolated process */
        sel4utils_process_t auth_proc;
        seL4_CPtr auth_caps[2] = { serial_ep.cptr, auth_ep_cap };
        seL4_CPtr auth_slots[2];
        error = spawn_with_args("auth_server", 200, &auth_proc,
                                &fault_ep, 2, auth_caps, auth_slots);
        if (error) { printf("[proc] auth FAILED\n"); goto idle; }
        proc_add("auth_server", 200);

        /* Send /etc/passwd to auth_server via IPC */
        {
            static char pw_buf[4096];
            int pw_len = vfs_read("/etc/passwd", pw_buf, sizeof(pw_buf) - 1);
            if (pw_len > 0) {
                pw_buf[pw_len] = '\0';
                seL4_SetMR(0, (seL4_Word)pw_len);
                int mr = 1;
                seL4_Word w = 0;
                for (int i = 0; i < pw_len; i++) {
                    w |= ((seL4_Word)(uint8_t)pw_buf[i]) << ((i % 8) * 8);
                    if (i % 8 == 7 || i == pw_len - 1) { seL4_SetMR(mr++, w); w = 0; }
                }
                seL4_Call(auth_ep_cap, seL4_MessageInfo_new(52, 0, 0, mr));
            } else {
                printf("[boot] /etc/passwd not found, using defaults\n");
            }
        }
        printf("[boot] Auth: isolated process\n");

        /* Spawn getty from disk via exec_thread (EXEC_RUN_shell)
         * exec_thread creates proper VSpace with page tracking,
         * enabling fork()+exec() to work correctly */
        {
            const char *sh_cmd = "/bin/getty CWD=0:0:/";
            int sh_pl = 0;
            while (sh_cmd[sh_pl]) sh_pl++;
            seL4_SetMR(0, (seL4_Word)sh_pl);
            int sh_mr = 1;
            seL4_Word sh_w = 0;
            for (int i = 0; i < sh_pl; i++) {
                sh_w |= ((seL4_Word)(uint8_t)sh_cmd[i]) << ((i % 8) * 8);
                if (i % 8 == 7 || i == sh_pl - 1) {
                    seL4_SetMR(sh_mr++, sh_w);
                    sh_w = 0;
                }
            }
            seL4_Call(exec_ep_cap,
                seL4_MessageInfo_new(EXEC_RUN_BG, 0, 0, sh_mr));
        }
        /* quiet */
    }

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
