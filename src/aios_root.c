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
#include <elf/elf.h>
#include <sel4utils/elf.h>
#include <sel4utils/api.h>

#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 800)
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

/* ── Thread management ── */
#define THREAD_CREATE    30
#define THREAD_JOIN      31
#define MAX_ACTIVE_PROCS  8
#define MAX_THREADS_PER_PROC 8
#define THREAD_STACK_PAGES   4

typedef struct {
    int active;
    int tid;
    vka_object_t tcb;
    vka_object_t fault_ep;
    vka_object_t ipc_frame;
    vka_object_t stack_frames[THREAD_STACK_PAGES];
    int exited;
} aios_thread_t;

typedef struct {
    int active;
    int pid;
    uint32_t uid;
    uint32_t gid;
    sel4utils_process_t proc;
    vka_object_t fault_ep;
    int num_threads;
    aios_thread_t threads[MAX_THREADS_PER_PROC];
} active_proc_t;

static active_proc_t active_procs[MAX_ACTIVE_PROCS];
static seL4_CPtr thread_ep_cap;
static seL4_CPtr pipe_ep_cap;

/* ── Pipe management ── */
#define PIPE_CREATE  60
#define PIPE_WRITE   61
#define PIPE_READ    62
#define PIPE_CLOSE   63
#define MAX_PIPES     8
#define PIPE_BUF_SIZE 8192

typedef struct {
    int active;
    char buf[PIPE_BUF_SIZE];
    int head;       /* write position */
    int count;      /* bytes in buffer */
    int read_closed;
    int write_closed;
} pipe_t;

static pipe_t pipes[MAX_PIPES];
static seL4_CPtr auth_ep_cap;

/* Filesystem state (shared with fs thread) */
static ext2_ctx_t ext2;
static volatile uint32_t *blk_vio;
static uint8_t *blk_dma;
static uint64_t blk_dma_pa;
static seL4_CPtr fs_ep_cap;
static vka_object_t serial_ep;
uint32_t aios_total_mem = 0;

static int blk_read_sector(uint64_t sector, void *buf);
static int blk_write_sector(uint64_t sector, const void *buf);
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

/* ── Create a thread inside a child process's VSpace ── */
static int create_child_thread(int proc_idx, seL4_Word entry, seL4_Word arg,
                                int *out_tid) {
    active_proc_t *ap = &active_procs[proc_idx];
    if (!ap->active) return -1;

    int tidx = -1;
    for (int i = 0; i < MAX_THREADS_PER_PROC; i++) {
        if (!ap->threads[i].active) { tidx = i; break; }
    }
    if (tidx < 0) return -1;
    aios_thread_t *t = &ap->threads[tidx];

    /* 1. Allocate TCB */
    if (vka_alloc_tcb(&vka, &t->tcb)) return -1;

    /* 2. Allocate fault endpoint for this thread */
    if (vka_alloc_endpoint(&vka, &t->fault_ep)) goto fail_tcb;

    /* 3. Allocate IPC buffer frame */
    if (vka_alloc_frame(&vka, seL4_PageBits, &t->ipc_frame)) goto fail_fault;

    /* 4. Map IPC buffer into child's VSpace */
    void *ipc_addr = vspace_map_pages(&ap->proc.vspace,
        &t->ipc_frame.cptr, NULL, seL4_AllRights, 1, seL4_PageBits, 0);
    if (!ipc_addr) goto fail_ipc;

    /* 6. Allocate and map stack (16 KB) */
    seL4_CPtr stack_caps[THREAD_STACK_PAGES];
    for (int i = 0; i < THREAD_STACK_PAGES; i++) {
        if (vka_alloc_frame(&vka, seL4_PageBits, &t->stack_frames[i])) {
            for (int j = 0; j < i; j++)
                vka_free_object(&vka, &t->stack_frames[j]);
            goto fail_ipc;
        }
        stack_caps[i] = t->stack_frames[i].cptr;
    }
    void *stack_base = vspace_map_pages(&ap->proc.vspace,
        stack_caps, NULL, seL4_AllRights,
        THREAD_STACK_PAGES, seL4_PageBits, 0);
    if (!stack_base) {
        for (int i = 0; i < THREAD_STACK_PAGES; i++)
            vka_free_object(&vka, &t->stack_frames[i]);
        goto fail_ipc;
    }

    /* 7. Configure TCB: child's CSpace + VSpace */
    /*
     * fault_ep is a RAW CPtr stored in the TCB — kernel resolves it
     * from the THREAD's CSpace at fault time, not root's.
     * So copy the fault ep cap into the child's CSpace.
     * We keep the root-side cap for Recv in thread_server.
     */
    seL4_CPtr child_fault_cap = sel4utils_copy_cap_to_process(
        &ap->proc, &vka, t->fault_ep.cptr);
    if (child_fault_cap == 0) {
        printf("[thread] Failed to copy fault ep to child\n");
        goto fail_stack;
    }
    seL4_Word cspace_data = api_make_guard_skip_word(seL4_WordBits - 12);
    int err = seL4_TCB_Configure(t->tcb.cptr,
        child_fault_cap,                      /* fault ep CPtr in CHILD's CSpace */
        ap->proc.cspace.cptr,                /* child CNode cap */
        cspace_data,                          /* 12-bit CNode guard */
        vspace_get_root(&ap->proc.vspace),    /* child PGD */
        seL4_NilData,
        (seL4_Word)ipc_addr,                  /* IPC buf vaddr in child */
        t->ipc_frame.cptr);                   /* IPC buf frame (kernel resolves from caller) */
    if (err) {
        printf("[thread] TCB_Configure failed: %d\n", err);
        goto fail_stack;
    }

    /* 8. Priority — same as everything else */
    seL4_TCB_SetPriority(t->tcb.cptr, seL4_CapInitThreadTCB, 200);

    /* 9. Set registers and start */
    seL4_UserContext regs;
    int nregs = sizeof(regs) / sizeof(seL4_Word);
    for (int i = 0; i < nregs; i++) ((seL4_Word *)&regs)[i] = 0;
    regs.pc  = entry;
    regs.sp  = (seL4_Word)stack_base + THREAD_STACK_PAGES * BIT(seL4_PageBits);
    regs.x0  = arg;
    regs.x30 = 0;   /* LR=0: return from fn triggers VM fault (clean exit) */
    err = seL4_TCB_WriteRegisters(t->tcb.cptr, 1 /* resume */,
                                   0, nregs, &regs);
    if (err) {
        printf("[thread] WriteRegisters failed: %d\n", err);
        goto fail_stack;
    }

    /* 10. Record */
    t->active = 1;
    t->tid = tidx + 1;  /* 1-based */
    t->exited = 0;
    ap->num_threads++;
    *out_tid = t->tid;
    return 0;

fail_stack:
    for (int i = 0; i < THREAD_STACK_PAGES; i++)
        vka_free_object(&vka, &t->stack_frames[i]);
fail_ipc:
    vka_free_object(&vka, &t->ipc_frame);
fail_fault:
    vka_free_object(&vka, &t->fault_ep);
fail_tcb:
    vka_free_object(&vka, &t->tcb);
    return -1;
}

/* ── Thread server — handles THREAD_CREATE / THREAD_JOIN IPC ── */
static void thread_server_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    (void)arg1; (void)ipc_buf;

    cspacepath_t reply_path;
    int err = vka_cspace_alloc_path(&vka, &reply_path);
    if (err) { printf("[thread_srv] FATAL: no reply slot\n"); return; }
    seL4_CPtr reply_slot = reply_path.capPtr;

    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);
        int proc_idx = (int)badge - 1;  /* badge = proc_idx + 1 */

        if (proc_idx < 0 || proc_idx >= MAX_ACTIVE_PROCS
            || !active_procs[proc_idx].active) {
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            continue;
        }

        switch (label) {
        case THREAD_CREATE: {
            seL4_Word entry_addr = seL4_GetMR(0);
            seL4_Word thread_arg = seL4_GetMR(1);
            int tid = 0;
            int ret = create_child_thread(proc_idx, entry_addr,
                                           thread_arg, &tid);
            seL4_SetMR(0, ret == 0 ? (seL4_Word)tid : (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case THREAD_JOIN: {
            seL4_Word tid = seL4_GetMR(0);
            active_proc_t *ap = &active_procs[proc_idx];
            int tidx = (int)tid - 1;
            if (tidx < 0 || tidx >= MAX_THREADS_PER_PROC
                || !ap->threads[tidx].active) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            /* Save reply cap, block on thread's fault ep */
            seL4_CNode_Delete(seL4_CapInitThreadCNode,
                              reply_slot, seL4_WordBits);
            seL4_CNode_SaveCaller(seL4_CapInitThreadCNode,
                                   reply_slot, seL4_WordBits);

            aios_thread_t *t = &ap->threads[tidx];
            seL4_Word child_badge;
            seL4_Recv(t->fault_ep.cptr, &child_badge);

            /* Thread exited — clean up kernel objects */
            vka_free_object(&vka, &t->tcb);
            vka_free_object(&vka, &t->fault_ep);
            vka_free_object(&vka, &t->ipc_frame);
            for (int i = 0; i < THREAD_STACK_PAGES; i++)
                vka_free_object(&vka, &t->stack_frames[i]);
            t->active = 0;
            t->exited = 1;
            ap->num_threads--;

            seL4_SetMR(0, 0);
            seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        default:
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
    }
}

/* ── Pipe server — manages pipe buffers ── */
static void pipe_server_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    (void)arg1; (void)ipc_buf;

    /* Init pipes */
    for (int i = 0; i < MAX_PIPES; i++) pipes[i].active = 0;

    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);

        switch (label) {
        case PIPE_CREATE: {
            /* Find free pipe slot */
            int pi = -1;
            for (int i = 0; i < MAX_PIPES; i++) {
                if (!pipes[i].active) { pi = i; break; }
            }
            if (pi < 0) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            pipes[pi].active = 1;
            pipes[pi].head = 0;
            pipes[pi].count = 0;
            pipes[pi].read_closed = 0;
            pipes[pi].write_closed = 0;
            seL4_SetMR(0, (seL4_Word)pi);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_WRITE: {
            /* MR0=pipe_id, MR1=len, MR2..=data */
            int pi = (int)seL4_GetMR(0);
            int wlen = (int)seL4_GetMR(1);
            if (pi < 0 || pi >= MAX_PIPES || !pipes[pi].active) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            pipe_t *p = &pipes[pi];
            int mr = 2;
            int written = 0;
            for (int i = 0; i < wlen && p->count < PIPE_BUF_SIZE; i++) {
                if (i % 8 == 0 && i > 0) mr++;
                char c = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
                p->buf[(p->head + p->count) % PIPE_BUF_SIZE] = c;
                p->count++;
                written++;
            }
            seL4_SetMR(0, (seL4_Word)written);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_READ: {
            /* MR0=pipe_id, MR1=max_len → MR0=len, MR1..=data */
            int pi = (int)seL4_GetMR(0);
            int max_len = (int)seL4_GetMR(1);
            if (pi < 0 || pi >= MAX_PIPES || !pipes[pi].active) {
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            pipe_t *p = &pipes[pi];
            int rlen = p->count < max_len ? p->count : max_len;
            /* Cap at what fits in MRs (~900 bytes) */
            if (rlen > 900) rlen = 900;
            seL4_SetMR(0, (seL4_Word)rlen);
            int mr = 1;
            seL4_Word w = 0;
            for (int i = 0; i < rlen; i++) {
                char c = p->buf[(p->head + i) % PIPE_BUF_SIZE];
                w |= ((seL4_Word)(uint8_t)c) << ((i % 8) * 8);
                if (i % 8 == 7 || i == rlen - 1) { seL4_SetMR(mr++, w); w = 0; }
            }
            p->head = (p->head + rlen) % PIPE_BUF_SIZE;
            p->count -= rlen;
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, mr));
            break;
        }
        case PIPE_CLOSE: {
            int pi = (int)seL4_GetMR(0);
            if (pi >= 0 && pi < MAX_PIPES && pipes[pi].active) {
                pipes[pi].active = 0;
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        default:
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
    }
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

        /* Debug: show received command */
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

        /* Allocate active_procs slot */
        int ap_idx = -1;
        for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
            if (!active_procs[i].active) { ap_idx = i; break; }
        }
        if (ap_idx < 0) {
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
            vka_free_object(&vka, &child_fault_ep);
            continue;
        }
        active_proc_t *ap = &active_procs[ap_idx];
        sel4utils_process_t *proc = &ap->proc;

        /* Read ELF from disk — shell sends full path */
        static char elf_buf[1024 * 1024];
        char elf_path[160];
        int epi = 0;
        const char *pn = prog_name;
        while (*pn && epi < 158) elf_path[epi++] = *pn++;
        elf_path[epi] = '\0';

        int elf_size = vfs_read(elf_path, elf_buf, sizeof(elf_buf));
        if (elf_size <= 0) {
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
            vka_free_object(&vka, &child_fault_ep);
            continue;
        }

        /* Parse ELF */
        elf_t elf;
        if (elf_newFile(elf_buf, elf_size, &elf) != 0) {
            printf("[exec] Invalid ELF: %s\n", elf_path);
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
            vka_free_object(&vka, &child_fault_ep);
            continue;
        }

        /* Configure process WITHOUT is_elf — we load manually */
        sel4utils_process_config_t pconfig = process_config_new(&simple);
        pconfig = process_config_create_cnode(pconfig, 12);
        pconfig = process_config_create_vspace(pconfig, NULL, 0);
        pconfig = process_config_priority(pconfig, 200);
        pconfig = process_config_auth(pconfig, simple_get_tcb(&simple));
        pconfig = process_config_fault_endpoint(pconfig, child_fault_ep);

        err = sel4utils_configure_process_custom(proc, &vka, &vspace, pconfig);
        if (err) {
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
            vka_free_object(&vka, &child_fault_ep);
            continue;
        }

        /* Load ELF into child's VSpace */
        proc->entry_point = sel4utils_elf_load(
            &proc->vspace, &vspace, &vka, &vka, &elf);
        if (proc->entry_point == NULL) {
            printf("[exec] ELF load failed: %s\n", elf_path);
            sel4utils_destroy_process(proc, &vka);
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
            vka_free_object(&vka, &child_fault_ep);
            continue;
        }
        proc->sysinfo = sel4utils_elf_get_vsyscall(&elf);

        seL4_CPtr child_ser = sel4utils_copy_cap_to_process(proc, &vka, serial_ep.cptr);
        /* Mint badged fs_ep (badge = ap_idx + 1) for permission checks */
        cspacepath_t fs_src, fs_dest;
        vka_cspace_make_path(&vka, fs_ep_cap, &fs_src);
        vka_cspace_alloc_path(&vka, &fs_dest);
        seL4_CNode_Mint(fs_dest.root, fs_dest.capPtr, fs_dest.capDepth,
            fs_src.root, fs_src.capPtr, fs_src.capDepth,
            seL4_AllRights, (seL4_Word)(ap_idx + 1));
        seL4_CPtr child_fs = sel4utils_copy_cap_to_process(
            proc, &vka, fs_dest.capPtr);

        /* Mint badged thread_ep (badge = ap_idx + 1) and copy to child */
        cspacepath_t te_src, te_dest;
        vka_cspace_make_path(&vka, thread_ep_cap, &te_src);
        vka_cspace_alloc_path(&vka, &te_dest);
        seL4_CNode_Mint(te_dest.root, te_dest.capPtr, te_dest.capDepth,
            te_src.root, te_src.capPtr, te_src.capDepth,
            seL4_AllRights, (seL4_Word)(ap_idx + 1));
        seL4_CPtr child_thread = sel4utils_copy_cap_to_process(
            proc, &vka, te_dest.capPtr);

        seL4_CPtr child_auth = sel4utils_copy_cap_to_process(
            proc, &vka, auth_ep_cap);
        seL4_CPtr child_pipe = sel4utils_copy_cap_to_process(
            proc, &vka, pipe_ep_cap);

        char s_ser[16], s_fs[16], s_thread[16], s_auth[16], s_pipe[16];
        snprintf(s_ser, 16, "%lu", (unsigned long)child_ser);
        snprintf(s_fs, 16, "%lu", (unsigned long)child_fs);
        snprintf(s_thread, 16, "%lu", (unsigned long)child_thread);
        snprintf(s_auth, 16, "%lu", (unsigned long)child_auth);
        snprintf(s_pipe, 16, "%lu", (unsigned long)child_pipe);

        /* Build argv array */
        #define MAX_EXEC_ARGS 12
        char *child_argv[MAX_EXEC_ARGS];
        int child_argc = 0;
        child_argv[child_argc++] = s_ser;
        child_argv[child_argc++] = s_fs;
        child_argv[child_argc++] = s_thread;
        child_argv[child_argc++] = s_auth;
        child_argv[child_argc++] = s_pipe;
        /* Pass CWD from shell (encoded in exec_args after \x01 separator) */
        static char cwd_buf[256];
        cwd_buf[0] = '/'; cwd_buf[1] = 0;
        /* Check if last arg is CWD=xxx */
        if (exec_args) {
            char *cwd_marker = exec_args;
            char *last_space = 0;
            for (char *p = exec_args; *p; p++)
                if (*p == ' ') last_space = p;
            char *check = last_space ? last_space + 1 : exec_args;
            if (check[0] == 'C' && check[1] == 'W' && check[2] == 'D' && check[3] == '=') {
                char *v = check + 4;
                /* Parse uid:gid for active_procs, but keep full string for child */
                uint32_t _cwd_uid = 0, _cwd_gid = 0;
                {
                    const char *p = v;
                    if (*p >= '0' && *p <= '9') {
                        while (*p >= '0' && *p <= '9') { _cwd_uid = _cwd_uid * 10 + (*p - '0'); p++; }
                        if (*p == ':') p++;
                        while (*p >= '0' && *p <= '9') { _cwd_gid = _cwd_gid * 10 + (*p - '0'); p++; }
                    }
                }
                /* Store uid/gid for active_procs lookup */
                cwd_buf[252] = (char)(_cwd_uid & 0xFF);
                cwd_buf[253] = (char)((_cwd_uid >> 8) & 0xFF);
                cwd_buf[254] = (char)(_cwd_gid & 0xFF);
                cwd_buf[255] = (char)((_cwd_gid >> 8) & 0xFF);
                /* Copy FULL string (uid:gid:/path) so child can parse it too */
                int ci = 0;
                while (*v && ci < 251) cwd_buf[ci++] = *v++;
                cwd_buf[ci] = 0;
                if (last_space) *last_space = 0; /* strip CWD from args */
                else exec_args = 0;
            }
        }
        child_argv[child_argc++] = cwd_buf;
        child_argv[child_argc++] = prog_name;

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

        err = sel4utils_spawn_process_v(proc, &vka, &vspace,
                                         child_argc, child_argv, 1);
        if (err) {
            /* spawn failed silently */
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
            continue;
        }

        /* Register in active_procs + process table */
        ap->active = 1;
        ap->uid = (uint32_t)(uint8_t)cwd_buf[252] | ((uint32_t)(uint8_t)cwd_buf[253] << 8);
        ap->gid = (uint32_t)(uint8_t)cwd_buf[254] | ((uint32_t)(uint8_t)cwd_buf[255] << 8);
        ap->fault_ep = child_fault_ep;
        ap->num_threads = 0;
        for (int ti = 0; ti < MAX_THREADS_PER_PROC; ti++)
            ap->threads[ti].active = 0;
        int child_pid = proc_add(prog_name, 200);
        ap->pid = child_pid;

        /* Apply nice value if EXEC_NICE */
        if (label == EXEC_NICE) {
            seL4_Word nice_mr = seL4_MessageInfo_get_length(msg);
            /* Nice value was stored in last MR by shell */
        }

        /* Wait for child to exit */
        seL4_Word child_badge;
        seL4_Recv(child_fault_ep.cptr, &child_badge);

        /* Clean up any remaining threads */
        for (int ti = 0; ti < MAX_THREADS_PER_PROC; ti++) {
            aios_thread_t *ct = &ap->threads[ti];
            if (ct->active) {
                vka_free_object(&vka, &ct->tcb);
                vka_free_object(&vka, &ct->fault_ep);
                vka_free_object(&vka, &ct->ipc_frame);
                for (int si = 0; si < THREAD_STACK_PAGES; si++)
                    vka_free_object(&vka, &ct->stack_frames[si]);
                ct->active = 0;
            }
        }
        sel4utils_destroy_process(proc, &vka);
        vka_free_object(&vka, &child_fault_ep);
        /* Free badged thread_ep — must delete cap before freeing slot */
        seL4_CNode_Delete(seL4_CapInitThreadCNode, te_dest.capPtr, seL4_WordBits);
        vka_cspace_free(&vka, te_dest.capPtr);
        ap->active = 0;
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
static int fs_check_write_perm(int badge) {
    if (badge == 0) return 1;  /* unbadged = internal (root) */
    int idx = badge - 1;
    if (idx < 0 || idx >= MAX_ACTIVE_PROCS) return 0;
    if (!active_procs[idx].active) return 0;
    return (active_procs[idx].uid == 0);  /* only root can write */
}

static int fs_check_path_write(int badge, const char *path) {
    /* DEBUG PERM */
    if (badge > 0) {
        int idx = badge - 1;
        printf("[fs] PERM CHECK: badge=%d uid=%u path=%s\n",
               badge, active_procs[idx].active ? active_procs[idx].uid : 9999, path);
    }
    if (fs_check_write_perm(badge)) return 1;
    /* Non-root: deny writes to /etc/ */
    if (path[0] == '/' && path[1] == 'e' && path[2] == 't' &&
        path[3] == 'c' && (path[4] == '/' || path[4] == '\0'))
        return 0;
    /* Non-root: deny writes to /bin/ */
    if (path[0] == '/' && path[1] == 'b' && path[2] == 'i' &&
        path[3] == 'n' && (path[4] == '/' || path[4] == '\0'))
        return 0;
    /* Allow other writes */
    return 1;
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
        int fs_badge = (int)badge;

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
        case FS_MKDIR: {
            seL4_Word path_len = seL4_GetMR(0);
            char mk_path[128];
            int mkpl = (path_len > 127) ? 127 : (int)path_len;
            int mk_mr = 1;
            for (int i = 0; i < mkpl; i++) {
                if (i % 8 == 0 && i > 0) mk_mr++;
                mk_path[i] = (char)((seL4_GetMR(mk_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            mk_path[mkpl] = '\0';
            if (!fs_check_path_write(fs_badge, mk_path)) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int ret = vfs_mkdir(mk_path);
            seL4_SetMR(0, (seL4_Word)(ret >= 0 ? 0 : -1));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case FS_WRITE_FILE: {
            /* MR0=path_len, MR1..=path, then data_len + data */
            seL4_Word path_len = seL4_GetMR(0);
            char wr_path_pre[128];
            /* Peek at path for permission check */
            int wrpl_pre = (path_len > 127) ? 127 : (int)path_len;
            int wr_mr_pre = 1;
            for (int i = 0; i < wrpl_pre; i++) {
                if (i % 8 == 0 && i > 0) wr_mr_pre++;
                wr_path_pre[i] = (char)((seL4_GetMR(wr_mr_pre) >> ((i % 8) * 8)) & 0xFF);
            }
            wr_path_pre[wrpl_pre] = '\0';
            if (!fs_check_path_write(fs_badge, wr_path_pre)) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            char wr_path[128];
            int wrpl = (path_len > 127) ? 127 : (int)path_len;
            int wr_mr = 1;
            for (int i = 0; i < wrpl; i++) {
                if (i % 8 == 0 && i > 0) wr_mr++;
                wr_path[i] = (char)((seL4_GetMR(wr_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            wr_path[wrpl] = '\0';
            wr_mr++;
            seL4_Word data_len = seL4_GetMR(wr_mr++);
            char wr_data[512];
            int dl = (data_len > 511) ? 511 : (int)data_len;
            for (int i = 0; i < dl; i++) {
                if (i % 8 == 0 && i > 0) wr_mr++;
                wr_data[i] = (char)((seL4_GetMR(wr_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            wr_data[dl] = '\0';
            int ret = vfs_create(wr_path, wr_data, dl);
            seL4_SetMR(0, (seL4_Word)(ret >= 0 ? 0 : -1));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case FS_UNLINK: {
            seL4_Word path_len = seL4_GetMR(0);
            char rm_path[128];
            int rmpl = (path_len > 127) ? 127 : (int)path_len;
            int rm_mr = 1;
            for (int i = 0; i < rmpl; i++) {
                if (i % 8 == 0 && i > 0) rm_mr++;
                rm_path[i] = (char)((seL4_GetMR(rm_mr) >> ((i % 8) * 8)) & 0xFF);
            }
            rm_path[rmpl] = '\0';
            if (!fs_check_path_write(fs_badge, rm_path)) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int ret = vfs_unlink(rm_path);
            seL4_SetMR(0, (seL4_Word)(ret >= 0 ? 0 : -1));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case FS_UNAME: {
            /* Return system info packed in MRs:
             * MR0-1: sysname (16 bytes)
             * MR2-3: nodename (16 bytes)  
             * MR4-5: release (16 bytes)
             * MR6-7: version (16 bytes)
             * MR8-9: machine (16 bytes) */
            char info[80];
            for (int i = 0; i < 80; i++) info[i] = 0;
            
            /* sysname */
            const char *s = "AIOS";
            for (int i = 0; s[i] && i < 15; i++) info[i] = s[i];
            
            /* nodename — read from ext2 /etc/hostname */
            char hname[16];
            for (int i = 0; i < 16; i++) hname[i] = 0;
            int hlen = vfs_read("/etc/hostname", hname, 15);
            if (hlen > 0) {
                /* Strip trailing newline */
                if (hname[hlen-1] == '\n') hname[hlen-1] = 0;
            } else {
                hname[0] = 'a'; hname[1] = 'i'; hname[2] = 'o'; hname[3] = 's';
            }
            for (int i = 0; i < 16; i++) info[16 + i] = hname[i];
            
            /* release */
            s = AIOS_VERSION_STR;
            for (int i = 0; s[i] && i < 15; i++) info[32 + i] = s[i];
            
            /* version — seL4 + build info */
            const char *ver = "seL4 15.0.0 SMP #" _AIOS_XSTR(AIOS_BUILD_NUMBER);
            for (int i = 0; ver[i] && i < 15; i++) info[48 + i] = ver[i];
            
            /* machine */
            s = "aarch64";
            for (int i = 0; s[i] && i < 15; i++) info[64 + i] = s[i];
            
            /* Pack into MRs (8 bytes per MR) */
            for (int i = 0; i < 10; i++) {
                seL4_Word w = 0;
                for (int j = 0; j < 8; j++)
                    w |= ((seL4_Word)(uint8_t)info[i*8 + j]) << (j * 8);
                seL4_SetMR(i, w);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 10));
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
        sel4utils_process_t serial_proc, shell_proc;
        seL4_CPtr caps[1], slots[1];
        caps[0] = serial_ep.cptr;

        error = spawn_with_args("serial_server", 200, &serial_proc,
                                &fault_ep, 1, caps, slots);
        if (error) { printf("[proc] serial FAILED\n"); goto idle; }
        proc_add("serial_server", 200);

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

        seL4_CPtr sh_caps[5] = { serial_ep.cptr, fs_ep_cap, exec_ep_cap, auth_ep_cap, pipe_ep_cap };
        seL4_CPtr sh_slots[5];
        error = spawn_with_args("mini_shell", 200, &shell_proc,
                                &fault_ep, 5, sh_caps, sh_slots);
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
