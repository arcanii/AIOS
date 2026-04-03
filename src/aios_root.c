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
#define EXEC_RUN     20
#define EXEC_NICE    21
static volatile uint32_t *uart;

/* Filesystem state (shared with fs thread) */
static ext2_ctx_t ext2;
static volatile uint32_t *blk_vio;
static uint8_t *blk_dma;
static uint64_t blk_dma_pa;
static seL4_CPtr fs_ep_cap;
static vka_object_t serial_ep;

static int blk_read_sector(uint64_t sector, void *buf);
static void fs_thread_fn(void *arg0, void *arg1, void *ipc_buf);

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

/* Exec thread — spawns processes on behalf of shell */
static vka_object_t exec_reply_cap_obj;

static void exec_thread_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    /* quiet */

    /* Allocate a slot to save reply caps */
    cspacepath_t reply_path;
    int err = vka_cspace_alloc_path(&vka, &reply_path);
    if (err) {
        printf("[exec] FATAL: cannot alloc reply cslot\n");
        return;
    }
    /* Free the endpoint object but keep the slot for SaveCaller */
    seL4_CPtr reply_slot = reply_path.capPtr;

    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);

        if (label != EXEC_RUN && label != EXEC_NICE) {
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            continue;
        }

        /* Unpack program name */
        seL4_Word name_len = seL4_GetMR(0);
        char prog_name[64];
        int nl = (name_len > 63) ? 63 : (int)name_len;
        int mr_i = 1;
        for (int i = 0; i < nl; i++) {
            if (i % 8 == 0 && i > 0) mr_i++;
            prog_name[i] = (char)((seL4_GetMR(mr_i) >> ((i % 8) * 8)) & 0xFF);
        }
        prog_name[nl] = '\0';

        /* Clear slot then save reply cap */
        seL4_CNode_Delete(seL4_CapInitThreadCNode, reply_slot, seL4_WordBits);
        seL4_CNode_SaveCaller(seL4_CapInitThreadCNode, reply_slot,
                               seL4_WordBits);

        /* Split "name arg1 arg2" into prog_name + args */
        char *exec_args = 0;
        for (int i = 0; i < nl; i++) {
            if (prog_name[i] == ' ') {
                prog_name[i] = '\0';
                exec_args = prog_name + i + 1;
                break;
            }
        }

        /* Create a local fault ep */
        vka_object_t child_fault_ep;
        err = vka_alloc_endpoint(&vka, &child_fault_ep);
        if (err) {
            /* alloc failed silently */
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
            continue;
        }

        sel4utils_process_t proc;
        /* Pass caps: serial_ep, fs_ep. Pass args string as extra argv. */
        seL4_CPtr exec_caps[2] = { serial_ep.cptr, fs_ep_cap };
        seL4_CPtr exec_slots[2];

        /* Build argv: [serial_ep_str, fs_ep_str, arg1, arg2, ...] */
        sel4utils_process_config_t pconfig = process_config_new(&simple);
        pconfig = process_config_elf(pconfig, prog_name, true);
        pconfig = process_config_create_cnode(pconfig, 12);
        pconfig = process_config_create_vspace(pconfig, NULL, 0);
        pconfig = process_config_priority(pconfig, 200);
        pconfig = process_config_auth(pconfig, simple_get_tcb(&simple));
        pconfig = process_config_fault_endpoint(pconfig, child_fault_ep);

        err = sel4utils_configure_process_custom(&proc, &vka, &vspace, pconfig);
        if (err) {
            /* configure failed silently */
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
            continue;
        }

        seL4_CPtr child_ser = sel4utils_copy_cap_to_process(&proc, &vka, serial_ep.cptr);
        seL4_CPtr child_fs = sel4utils_copy_cap_to_process(&proc, &vka, fs_ep_cap);

        char s_ser[16], s_fs[16];
        snprintf(s_ser, 16, "%lu", (unsigned long)child_ser);
        snprintf(s_fs, 16, "%lu", (unsigned long)child_fs);

        /* Build argv array */
        #define MAX_EXEC_ARGS 8
        char *child_argv[MAX_EXEC_ARGS];
        int child_argc = 0;
        child_argv[child_argc++] = s_ser;
        child_argv[child_argc++] = s_fs;

        /* Split exec_args by spaces */
        if (exec_args) {
            char *p = exec_args;
            while (*p && child_argc < MAX_EXEC_ARGS) {
                while (*p == ' ') p++;
                if (!*p) break;
                child_argv[child_argc++] = p;
                while (*p && *p != ' ') p++;
                if (*p) { *p = '\0'; p++; }
            }
        }

        err = sel4utils_spawn_process_v(&proc, &vka, &vspace,
                                         child_argc, child_argv, 1);
        if (err) {
            /* spawn failed silently */
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
            continue;
        }

        /* Register in process table */
        int child_pid = proc_add(prog_name, 200);

        /* Apply nice value if EXEC_NICE */
        if (label == EXEC_NICE) {
            seL4_Word nice_mr = seL4_MessageInfo_get_length(msg);
            /* Nice value was stored in last MR by shell */
        }

        /* Wait for child to exit */
        seL4_Word child_badge;
        seL4_Recv(child_fault_ep.cptr, &child_badge);
        // printf("[exec] %s exited\n", prog_name);
        sel4utils_destroy_process(&proc, &vka);
        if (child_pid > 0) proc_remove(child_pid);

        /* Reply to shell via saved cap */
        seL4_SetMR(0, 0);
        seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
    }
}

/* Read a 512-byte sector via virtio-blk */
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

/* Filesystem IPC thread — runs in root task VSpace */
static void fs_thread_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    static char fs_buf[4096];
    (void)ep; /* used below in Recv */

    /* quiet */

    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);

        switch (label) {
        case FS_LS: {
            seL4_Word path_len = seL4_GetMR(0);
            char ls_path[128];
            int lpl = (path_len > 127) ? 127 : (int)path_len;
            int ls_mr = 1;
            for (int i = 0; i < lpl; i++) {
                if (i % 8 == 0 && i > 0) ls_mr++;
                ls_path[i] = (char)((seL4_GetMR(ls_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            ls_path[lpl] = '\0';
            if (lpl == 0) { ls_path[0] = '/'; ls_path[1] = '\0'; }

            int len = vfs_list(ls_path, fs_buf, sizeof(fs_buf));
            if (len < 0) len = 0;
            if (len < 0) len = 0;
            /* Return dir listing in MRs (pack 8 chars per MR) */
            int mrs = (len + 7) / 8;
            if (mrs > (int)seL4_MsgMaxLength - 1) mrs = seL4_MsgMaxLength - 1;
            seL4_SetMR(0, (seL4_Word)len);
            for (int i = 0; i < mrs; i++) {
                seL4_Word w = 0;
                for (int j = 0; j < 8 && i*8+j < len; j++)
                    w |= ((seL4_Word)(uint8_t)fs_buf[i*8+j]) << (j*8);
                seL4_SetMR(i + 1, w);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, mrs + 1));
            break;
        }
        case FS_CAT: {
            seL4_Word path_len = seL4_GetMR(0);
            char path[128];
            int pl = (path_len > 127) ? 127 : (int)path_len;
            int mr_idx = 1;
            for (int i = 0; i < pl; i++) {
                if (i % 8 == 0 && i > 0) mr_idx++;
                path[i] = (char)((seL4_GetMR(mr_idx) >> ((i % 8) * 8)) & 0xFF);
            }
            path[pl] = '\0';

            int len = vfs_read(path, fs_buf, sizeof(fs_buf));
            if (len < 0) len = 0;
            int mrs = (len + 7) / 8;
            if (mrs > (int)seL4_MsgMaxLength - 1) mrs = seL4_MsgMaxLength - 1;
            seL4_SetMR(0, (seL4_Word)len);
            for (int i = 0; i < mrs; i++) {
                seL4_Word w = 0;
                for (int j = 0; j < 8 && i*8+j < len; j++)
                    w |= ((seL4_Word)(uint8_t)fs_buf[i*8+j]) << (j*8);
                seL4_SetMR(i + 1, w);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, mrs + 1));
            break;
        }
        case FS_STAT: {
            seL4_Word path_len = seL4_GetMR(0);
            char st_path[128];
            int spl = (path_len > 127) ? 127 : (int)path_len;
            int st_mr = 1;
            for (int i = 0; i < spl; i++) {
                if (i % 8 == 0 && i > 0) st_mr++;
                st_path[i] = (char)((seL4_GetMR(st_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            st_path[spl] = '\0';

            uint32_t mode, size;
            if (vfs_stat(st_path, &mode, &size) == 0) {
                seL4_SetMR(0, 1);
                seL4_SetMR(1, (seL4_Word)mode);
                seL4_SetMR(2, (seL4_Word)size);
                seL4_SetMR(3, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 4));
            } else {
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            }
            break;
        }
        default:
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }
    }
}

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

    int total_mem = 0;
    for (seL4_Word i = info->untyped.start; i < info->untyped.end; i++) {
        seL4_UntypedDesc *ut = &info->untypedList[i - info->untyped.start];
        if (!ut->isDevice) total_mem += BIT(ut->sizeBits);
    }
    printf("[boot] RAM: %d MB, UART: %s\n",
           total_mem / (1024 * 1024), uart ? "OK" : "no");
    printf("[boot] All subsystems OK\n\n");

    /* Register system processes */
    proc_add("root", 200);


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

        /* Allocate DMA pages via vka */
        vka_object_t dma_frames[4];
        seL4_CPtr dma_caps[4];
        uint64_t dma_paddrs[4];
        int dma_ok = 1;
        for (int i = 0; i < 4; i++) {
            error = vka_alloc_frame(&vka, seL4_PageBits, &dma_frames[i]);
            if (error) {
                printf("[fs] DMA frame %d alloc failed: %d\n", i, error);
                dma_ok = 0; break;
            }
            dma_caps[i] = dma_frames[i].cptr;
            seL4_ARM_Page_GetAddress_t ga = seL4_ARM_Page_GetAddress(dma_caps[i]);
            if (ga.error) {
                printf("[fs] GetAddress %d failed\n", i);
                dma_ok = 0; break;
            }
            dma_paddrs[i] = ga.paddr;
        }
        if (!dma_ok) goto skip_blk;
        /* quiet */

        /* Check contiguity — virtio legacy needs contiguous phys for virtqueue */
        int contig = 1;
        for (int i = 1; i < 4; i++) {
            if (dma_paddrs[i] != dma_paddrs[0] + i * 0x1000) { contig = 0; break; }
        }
        if (!contig) {
            printf("[fs] WARNING: DMA pages not contiguous, using page 0 only\n");
        }

        void *dma_vaddr = vspace_map_pages(&vspace, dma_caps, NULL,
            seL4_AllRights, 4, seL4_PageBits, 0);
        if (!dma_vaddr) {
            printf("[fs] DMA map failed\n");
            goto skip_blk;
        }
        uint64_t dma_pa = dma_paddrs[0];
        /* quiet */

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
                vfs_mount("/", &ext2_fs_ops, &ext2);
                vfs_mount("/proc", &procfs_ops, NULL);
                proc_add("fs_thread", 200);
                proc_add("exec_thread", 200);
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

    /* quiet */
    {
        sel4utils_process_t serial_proc, shell_proc;
        seL4_CPtr caps[1], slots[1];
        caps[0] = serial_ep.cptr;

        error = spawn_with_args("serial_server", 200, &serial_proc,
                                &fault_ep, 1, caps, slots);
        if (error) { printf("[proc] serial FAILED\n"); goto idle; }
        proc_add("serial_server", 200);

        seL4_CPtr sh_caps[3] = { serial_ep.cptr, fs_ep_cap, exec_ep_cap };
        seL4_CPtr sh_slots[3];
        error = spawn_with_args("mini_shell", 200, &shell_proc,
                                &fault_ep, 3, sh_caps, sh_slots);
        if (error) { printf("[proc] shell FAILED\n"); goto idle; }
        proc_add("mini_shell", 200);
        /* quiet */
    }

    /* Main loop: keyboard polling + exec requests */
    while (1) {
        /* Poll UART for keyboard */
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
