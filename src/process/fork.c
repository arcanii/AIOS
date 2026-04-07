/* AIOS process/fork.c -- fork() implementation for seL4
 *
 * Implements do_fork() and helper functions for process duplication.
 * Copies parent address space (data segments + stack) into child VSpace,
 * duplicates capability slots, sets AArch64 registers for child return.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sel4/sel4.h>
#include <sel4utils/process.h>
#include <sel4utils/process_config.h>
#include <sel4utils/vspace.h>
#include <sel4utils/api.h>
#include <vka/capops.h>
#include <vka/object.h>
#include <elf/elf.h>
#include <sel4utils/elf.h>
#include <simple/simple.h>
#include "aios/root_shared.h"
#include "aios/vka_audit.h"
#include "aios/vfs.h"
#include "aios/procfs.h"

static int fork_copy_region(vspace_t *parent_vs, sel4utils_process_t *child_proc,
                            uintptr_t start, int num_pages) {
    for (int i = 0; i < num_pages; i++) {
        /* Page-align the start address */
        uintptr_t page_base = (start & ~((uintptr_t)PAGE_SIZE - 1)) + (uintptr_t)i * PAGE_SIZE;
        void *va = (void *)page_base;

        seL4_CPtr parent_cap = vspace_get_cap(parent_vs, va);
        if (parent_cap == seL4_CapNull) continue;

        /* Allocate new frame for child */
        vka_object_t new_frame;
        if (vka_alloc_frame(&vka, seL4_PageBits, &new_frame)) {
            printf("[fork] frame alloc failed at %p\n", va);
            return -1;
        }
        vka_audit_frame(VKA_SUB_FORK, 1);

        /*
         * Cannot map parent_cap directly into root — it belongs to parent's VSpace.
         * Must CNode_Copy to get a fresh cap, map the copy, then delete it.
         */
        cspacepath_t src_path, dup_path;
        vka_cspace_make_path(&vka, parent_cap, &src_path);
        if (vka_cspace_alloc_path(&vka, &dup_path)) {
            printf("[fork] cslot alloc failed at %p\n", va);
            vka_free_object(&vka, &new_frame);
            continue;
        }
        int cerr = seL4_CNode_Copy(dup_path.root, dup_path.capPtr, dup_path.capDepth,
                                   src_path.root, src_path.capPtr, src_path.capDepth,
                                   seL4_AllRights);
        if (cerr) {
            printf("[fork] CNode_Copy failed at %p: %d\n", va, cerr);
            vka_cspace_free(&vka, dup_path.capPtr);
            vka_free_object(&vka, &new_frame);
            continue;
        }

        /* Map duplicated parent cap into root temporarily */
        void *parent_tmp = vspace_map_pages(&vspace, &dup_path.capPtr, NULL,
            seL4_AllRights, 1, seL4_PageBits, 1);
        if (!parent_tmp) {
            printf("[fork] parent map failed at %p\n", va);
            seL4_CNode_Delete(dup_path.root, dup_path.capPtr, dup_path.capDepth);
            vka_cspace_free(&vka, dup_path.capPtr);
            vka_free_object(&vka, &new_frame);
            continue;
        }

        /* Map new frame into root temporarily */
        void *child_tmp = vspace_map_pages(&vspace, &new_frame.cptr, NULL,
            seL4_AllRights, 1, seL4_PageBits, 1);
        if (!child_tmp) {
            printf("[fork] child tmp map failed at %p\n", va);
            vspace_unmap_pages(&vspace, parent_tmp, 1, seL4_PageBits, NULL);
            seL4_CNode_Delete(dup_path.root, dup_path.capPtr, dup_path.capDepth);
            vka_cspace_free(&vka, dup_path.capPtr);
            vka_free_object(&vka, &new_frame);
            continue;
        }

        /* Copy page content */
        char *src = (char *)parent_tmp;
        char *dst = (char *)child_tmp;
        for (int b = 0; b < PAGE_SIZE; b++) dst[b] = src[b];

        /* Unmap both from root */
        vspace_unmap_pages(&vspace, parent_tmp, 1, seL4_PageBits, NULL);
        vspace_unmap_pages(&vspace, child_tmp, 1, seL4_PageBits, NULL);

        /* Delete the duplicated cap */
        seL4_CNode_Delete(dup_path.root, dup_path.capPtr, dup_path.capDepth);
        vka_cspace_free(&vka, dup_path.capPtr);

        /* Reserve + map new frame into child at same vaddr */
        reservation_t res = vspace_reserve_range_at(&child_proc->vspace, va,
            PAGE_SIZE, seL4_AllRights, 1);
        if (res.res == NULL) {
            printf("[fork] reserve failed at %p\n", va);
            vka_free_object(&vka, &new_frame);
            continue;
        }
        int merr = vspace_map_pages_at_vaddr(&child_proc->vspace, &new_frame.cptr, NULL,
            va, 1, seL4_PageBits, res);
        if (merr) {
            printf("[fork] child map_at_vaddr failed at %p: %d\n", va, merr);
        }
        vspace_maybe_call_allocated_object(&child_proc->vspace, new_frame);
    }
    return 0;
}

/*
 * Share read-only pages (e.g. .text) from parent to child at same vaddr.
 */
static int fork_share_region(vspace_t *parent_vs, sel4utils_process_t *child_proc,
                             uintptr_t start, int num_pages) {
    /* Page-align start */
    uintptr_t page_base = start & ~((uintptr_t)PAGE_SIZE - 1);
    void *va = (void *)page_base;

    /* Share page-by-page with CNode_Copy to avoid remap errors */
    for (int i = 0; i < num_pages; i++) {
        void *pg = (void *)(page_base + (uintptr_t)i * PAGE_SIZE);
        seL4_CPtr parent_cap = vspace_get_cap(parent_vs, pg);
        if (parent_cap == seL4_CapNull) continue;

        /* Duplicate cap for child mapping */
        cspacepath_t src_path, dup_path;
        vka_cspace_make_path(&vka, parent_cap, &src_path);
        if (vka_cspace_alloc_path(&vka, &dup_path)) continue;
        vka_audit_cslot(VKA_SUB_FORK); /* share */
        int cerr = seL4_CNode_Copy(dup_path.root, dup_path.capPtr, dup_path.capDepth,
                                   src_path.root, src_path.capPtr, src_path.capDepth,
                                   seL4_AllRights);
        if (cerr) {
            vka_cspace_free(&vka, dup_path.capPtr);
            continue;
        }

        reservation_t res = vspace_reserve_range_at(&child_proc->vspace, pg,
            PAGE_SIZE, seL4_CanRead, 1);
        if (res.res == NULL) {
            seL4_CNode_Delete(dup_path.root, dup_path.capPtr, dup_path.capDepth);
            vka_cspace_free(&vka, dup_path.capPtr);
            continue;
        }
        int merr = vspace_map_pages_at_vaddr(&child_proc->vspace, &dup_path.capPtr, NULL,
            pg, 1, seL4_PageBits, res);
        if (merr) {
            printf("[fork] share page failed at %p: %d\n", pg, merr);
        }
    }
    return 0;
}

/*
 * Copy stack pages by probing from SP.
 */
/*
 * Copy stack content from parent into child's EXISTING stack pages.
 * sel4utils already allocated stack for the child at the same addresses.
 * We just need to copy the content via temporary root mappings.
 */
static int fork_copy_into_existing(vspace_t *parent_vs, vspace_t *child_vs, uintptr_t page) {
    seL4_CPtr parent_cap = vspace_get_cap(parent_vs, (void *)page);
    seL4_CPtr child_cap = vspace_get_cap(child_vs, (void *)page);
    if (parent_cap == seL4_CapNull) return -1;
    if (child_cap == seL4_CapNull) return -2;  /* child doesn't have this page */

    /* Dup both caps for root mapping */
    cspacepath_t psrc, pdup, csrc, cdup;
    vka_cspace_make_path(&vka, parent_cap, &psrc);
    vka_cspace_make_path(&vka, child_cap, &csrc);
    if (vka_cspace_alloc_path(&vka, &pdup)) return -3;
    vka_audit_cslot(VKA_SUB_FORK); /* existing */
    if (vka_cspace_alloc_path(&vka, &cdup)) {
        vka_cspace_free(&vka, pdup.capPtr);
        return -3;
    }
    if (seL4_CNode_Copy(pdup.root, pdup.capPtr, pdup.capDepth,
                        psrc.root, psrc.capPtr, psrc.capDepth, seL4_AllRights)) {
        vka_cspace_free(&vka, pdup.capPtr);
        vka_cspace_free(&vka, cdup.capPtr);
        return -4;
    }
    if (seL4_CNode_Copy(cdup.root, cdup.capPtr, cdup.capDepth,
                        csrc.root, csrc.capPtr, csrc.capDepth, seL4_AllRights)) {
        seL4_CNode_Delete(pdup.root, pdup.capPtr, pdup.capDepth);
        vka_cspace_free(&vka, pdup.capPtr);
        vka_cspace_free(&vka, cdup.capPtr);
        return -4;
    }

    void *parent_tmp = vspace_map_pages(&vspace, &pdup.capPtr, NULL,
        seL4_AllRights, 1, seL4_PageBits, 1);
    void *child_tmp = vspace_map_pages(&vspace, &cdup.capPtr, NULL,
        seL4_AllRights, 1, seL4_PageBits, 1);

    if (parent_tmp && child_tmp) {
        char *src = (char *)parent_tmp;
        char *dst = (char *)child_tmp;
        for (int b = 0; b < PAGE_SIZE; b++) dst[b] = src[b];
    }

    if (parent_tmp) vspace_unmap_pages(&vspace, parent_tmp, 1, seL4_PageBits, NULL);
    if (child_tmp) vspace_unmap_pages(&vspace, child_tmp, 1, seL4_PageBits, NULL);
    seL4_CNode_Delete(pdup.root, pdup.capPtr, pdup.capDepth);
    seL4_CNode_Delete(cdup.root, cdup.capPtr, cdup.capDepth);
    vka_cspace_free(&vka, pdup.capPtr);
    vka_cspace_free(&vka, cdup.capPtr);
    return 0;
}

static int fork_copy_stack(active_proc_t *parent, sel4utils_process_t *child_proc,
                           uintptr_t sp) {
    uintptr_t page = sp & ~((uintptr_t)PAGE_SIZE - 1);
    int copied = 0;

    /* Probe upward from SP page */
    for (uintptr_t p = page; p < page + 64 * PAGE_SIZE; p += PAGE_SIZE) {
        seL4_CPtr cap = vspace_get_cap(&parent->proc.vspace, (void *)p);
        if (cap == seL4_CapNull) break;
        /* Try copying into child's existing page first (stack/IPC buf) */
        if (fork_copy_into_existing(&parent->proc.vspace, &child_proc->vspace, p) == 0) {
            copied++;
        } else {
            /* Child doesn't have this page — allocate and copy */
            fork_copy_region(&parent->proc.vspace, child_proc, p, 1);
            copied++;
        }
    }
    /* Probe downward */
    for (uintptr_t p = page - PAGE_SIZE; ; p -= PAGE_SIZE) {
        seL4_CPtr cap = vspace_get_cap(&parent->proc.vspace, (void *)p);
        if (cap == seL4_CapNull) break;
        if (fork_copy_into_existing(&parent->proc.vspace, &child_proc->vspace, p) == 0) {
            copied++;
        } else {
            fork_copy_region(&parent->proc.vspace, child_proc, p, 1);
            copied++;
        }
        if (p < PAGE_SIZE) break;
    }
    return copied;
}

/*
 * Full fork: create child process duplicating parent's address space.
 * Returns child PID on success, -1 on failure.
 */

int do_fork(int parent_idx) {
    active_proc_t *parent = &active_procs[parent_idx];
    if (!parent->active) return -1;

    /* 1. Find free proc slot */
    int child_idx = -1;
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
        if (!active_procs[i].active) { child_idx = i; break; }
    }
    if (child_idx < 0) return -1;

    /* 2. Mint badged pipe_ep as child fault endpoint.
     * Faults arrive on pipe_ep with badge = child_idx + 1,
     * distinguishable from PIPE_* IPC by label < PIPE_CREATE. */
    cspacepath_t pfe_src, pfe_dest;
    vka_cspace_make_path(&vka, pipe_ep_cap, &pfe_src);
    if (vka_cspace_alloc_path(&vka, &pfe_dest)) return -1;
    vka_audit_cslot(VKA_SUB_FORK);
    if (seL4_CNode_Mint(pfe_dest.root, pfe_dest.capPtr, pfe_dest.capDepth,
            pfe_src.root, pfe_src.capPtr, pfe_src.capDepth,
            seL4_AllRights, (seL4_Word)(child_idx + 1))) {
        vka_cspace_free(&vka, pfe_dest.capPtr);
        return -1;
    }
    vka_object_t child_fault_ep;
    memset(&child_fault_ep, 0, sizeof(child_fault_ep));
    child_fault_ep.cptr = pfe_dest.capPtr;

    /* 3. Find parent's ELF path and reload into child */
    active_proc_t *child = &active_procs[child_idx];
    sel4utils_process_t *cp = &child->proc;

    char fork_elf_path[160];
    {
        const char *pname = NULL;
        for (int pi = 0; pi < PROC_MAX; pi++) {
            if (proc_table[pi].active && proc_table[pi].pid == parent->pid) {
                pname = proc_table[pi].name; break;
            }
        }
        if (!pname) { vka_free_object(&vka, &child_fault_ep); return -1; }
        if (pname[0] == '/') {
            int ei = 0;
            while (pname[ei] && ei < 158) { fork_elf_path[ei] = pname[ei]; ei++; }
            fork_elf_path[ei] = 0;
        } else {
            int ei = 0;
            const char *pfx = "/bin/";
            while (*pfx) fork_elf_path[ei++] = *pfx++;
            while (*pname && ei < 158) fork_elf_path[ei++] = *pname++;
            fork_elf_path[ei] = 0;
        }
    }

    /* uses file-scope elf_buf */
    int elf_size = vfs_read(fork_elf_path, elf_buf, sizeof(elf_buf));
    if (elf_size <= 0) {
        seL4_CNode_Delete(pfe_dest.root, pfe_dest.capPtr, pfe_dest.capDepth);
        vka_cspace_free(&vka, pfe_dest.capPtr);
        return -1;
    }

    elf_t fork_elf;
    if (elf_newFile(elf_buf, elf_size, &fork_elf) != 0) {
        seL4_CNode_Delete(pfe_dest.root, pfe_dest.capPtr, pfe_dest.capDepth);
        vka_cspace_free(&vka, pfe_dest.capPtr);
        return -1;
    }

    /* 4. Configure child process + load ELF */
    sel4utils_process_config_t cfg = process_config_new(&simple);
    cfg = process_config_create_cnode(cfg, 12);
    cfg = process_config_create_vspace(cfg, NULL, 0);
    cfg = process_config_priority(cfg, 200);
    cfg = process_config_auth(cfg, simple_get_tcb(&simple));
    cfg = process_config_fault_endpoint(cfg, child_fault_ep);

    int err = sel4utils_configure_process_custom(cp, &vka, &vspace, cfg);
    if (err) {
        seL4_CNode_Delete(pfe_dest.root, pfe_dest.capPtr, pfe_dest.capDepth);
        vka_cspace_free(&vka, pfe_dest.capPtr);
        return -1;
    }

    cp->entry_point = sel4utils_elf_load(&cp->vspace, &vspace, &vka, &vka, &fork_elf);
    if (!cp->entry_point) {
        sel4utils_destroy_process(cp, &vka);
        seL4_CNode_Delete(pfe_dest.root, pfe_dest.capPtr, pfe_dest.capDepth);
        vka_cspace_free(&vka, pfe_dest.capPtr);
        return -1;
    }
    cp->sysinfo = sel4utils_elf_get_vsyscall(&fork_elf);

    /* 5. Read parent registers */
    seL4_UserContext parent_regs;
    seL4_TCB_ReadRegisters(parent->proc.thread.tcb.cptr, 0, 0,
                           sizeof(parent_regs) / sizeof(seL4_Word), &parent_regs);

    /* 6. Overwrite child's .data with parent's content */
    for (int s = 0; s < parent->num_segs; s++) {
        elf_seg_info_t *seg = &parent->segs[s];
        if (!(seg->flags & 2)) continue;
        uintptr_t base = seg->vaddr & ~((uintptr_t)PAGE_SIZE - 1);
        uintptr_t end = seg->vaddr + seg->memsz;
        int np = (int)((end - base + PAGE_SIZE - 1) / PAGE_SIZE);
        for (int pi = 0; pi < np; pi++) {
            fork_copy_into_existing(&parent->proc.vspace, &cp->vspace,
                                    base + (uintptr_t)pi * PAGE_SIZE);
        }
    }


    /* 7. Copy parent's stack (skip IPC buffer page) */
    {
        uintptr_t ipc_page = cp->thread.ipc_buffer_addr & ~((uintptr_t)PAGE_SIZE - 1);
        uintptr_t sp_page = parent_regs.sp & ~((uintptr_t)PAGE_SIZE - 1);
        for (uintptr_t p = sp_page; p < sp_page + 64 * PAGE_SIZE; p += PAGE_SIZE) {
            if (p == ipc_page) continue;
            seL4_CPtr pcap = vspace_get_cap(&parent->proc.vspace, (void *)p);
            if (pcap == seL4_CapNull) break;
            fork_copy_into_existing(&parent->proc.vspace, &cp->vspace, p);
        }
        for (uintptr_t p = sp_page - PAGE_SIZE; ; p -= PAGE_SIZE) {
            if (p == ipc_page) { if (p < PAGE_SIZE) break; continue; }
            seL4_CPtr pcap = vspace_get_cap(&parent->proc.vspace, (void *)p);
            if (pcap == seL4_CapNull) break;
            fork_copy_into_existing(&parent->proc.vspace, &cp->vspace, p);
            if (p < PAGE_SIZE) break;
        }
    }

    /* 8. Copy endpoint caps from parent's CSpace to child's */
    {
        seL4_CPtr child_cnode = cp->cspace.cptr;
        seL4_CPtr parent_cnode = parent->proc.cspace.cptr;
        seL4_Word depth = 12;
        seL4_CPtr slots[] = {
            parent->child_ser_slot, parent->child_fs_slot,
            parent->child_thread_slot, parent->child_auth_slot,
            parent->child_pipe_slot,
        };
        for (int ci = 0; ci < 5; ci++) {
            if (slots[ci] == 0) continue;
            seL4_CNode_Copy(child_cnode, slots[ci], depth,
                            parent_cnode, slots[ci], depth, seL4_AllRights);
        }
        child->child_ser_slot = parent->child_ser_slot;
        child->child_fs_slot = parent->child_fs_slot;
        child->child_thread_slot = parent->child_thread_slot;
        child->child_auth_slot = parent->child_auth_slot;
        child->child_pipe_slot = parent->child_pipe_slot;
    }

    /* 8b. Mint child's own pipe_ep with child's badge (child_idx + 1) */
    {
        seL4_CPtr child_cnode = cp->cspace.cptr;
        seL4_Word depth = 12;
        seL4_CPtr pipe_slot = child->child_pipe_slot;

        /* Delete the parent's pipe cap from child's slot */
        seL4_CNode_Delete(child_cnode, pipe_slot, depth);

        /* Mint a new one with child's badge */
        cspacepath_t pipe_src;
        vka_cspace_make_path(&vka, pipe_ep_cap, &pipe_src);
        int merr = seL4_CNode_Mint(child_cnode, pipe_slot, depth,
                        pipe_src.root, pipe_src.capPtr, pipe_src.capDepth,
                        seL4_AllRights, (seL4_Word)(child_idx + 1));
        if (merr) {
            printf("[fork] pipe_ep re-mint failed: %d\n", merr);
        }
    }

    /* 9. Register child in proc table (need PID before setting IPC buffer) */
    child->active = 1;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->ppid = parent->pid;
    child->fault_ep = child_fault_ep;
    child->fault_on_pipe_ep = 1;
    child->stdout_pipe_id = -1;
    child->stdin_pipe_id = -1;
    child->num_threads = 0;
    child->exit_status = 0;
    child->num_segs = parent->num_segs;
    for (int s = 0; s < parent->num_segs; s++)
        child->segs[s] = parent->segs[s];
    for (int ti = 0; ti < MAX_THREADS_PER_PROC; ti++)
        child->threads[ti].active = 0;

    int child_pid = proc_add("(forked)", 200);
    child->pid = child_pid;

    /* 10. Set child registers: AArch64 seL4 return ABI */
    seL4_UserContext child_regs = parent_regs;
    child_regs.pc += 4;
    child_regs.x0 = 0;                                       /* badge */
    seL4_MessageInfo_t fake_reply = seL4_MessageInfo_new(0, 0, 0, 2);
    child_regs.x1 = fake_reply.words[0];                      /* reply msginfo (2 MRs) */
    child_regs.x2 = 0;                                       /* MR0 = 0 (fork returns 0) */
    child_regs.x3 = (seL4_Word)child_pid;                    /* MR1 = child's PID */
    child_regs.x4 = 0;
    child_regs.x5 = 0;

    seL4_TCB_WriteRegisters(cp->thread.tcb.cptr, 0, 0,
                            sizeof(child_regs) / sizeof(seL4_Word), &child_regs);

    /* 11. Write MR0=0 and MR1=child_pid to child's IPC buffer */
    {
        seL4_CPtr ipc_cap = vspace_get_cap(&cp->vspace,
            (void *)(uintptr_t)cp->thread.ipc_buffer_addr);
        if (ipc_cap != seL4_CapNull) {
            cspacepath_t isrc, idup;
            vka_cspace_make_path(&vka, ipc_cap, &isrc);
            if (vka_cspace_alloc_path(&vka, &idup) == 0) {
                if (seL4_CNode_Copy(idup.root, idup.capPtr, idup.capDepth,
                                    isrc.root, isrc.capPtr, isrc.capDepth,
                                    seL4_AllRights) == 0) {
                    void *ipc_tmp = vspace_map_pages(&vspace, &idup.capPtr, NULL,
                        seL4_AllRights, 1, seL4_PageBits, 1);
                    if (ipc_tmp) {
                        uintptr_t page_off = cp->thread.ipc_buffer_addr & (PAGE_SIZE - 1);
                        seL4_Word *ipc_words = (seL4_Word *)((char *)ipc_tmp + page_off);
                        ipc_words[1] = 0;                  /* MR0 = 0 */
                        ipc_words[2] = (seL4_Word)child_pid; /* MR1 = pid */
                        vspace_unmap_pages(&vspace, ipc_tmp, 1, seL4_PageBits, NULL);
                    }
                    seL4_CNode_Delete(idup.root, idup.capPtr, idup.capDepth);
                }
                vka_cspace_free(&vka, idup.capPtr);
            }
        }
    }

    /* 12. Resume child */
    seL4_TCB_SetPriority(cp->thread.tcb.cptr, simple_get_tcb(&simple), 200);
    seL4_TCB_Resume(cp->thread.tcb.cptr);

    return child_pid;
}

