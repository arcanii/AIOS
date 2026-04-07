/* AIOS servers/thread_server.c -- Thread creation and management
 *
 * Handles THREAD_CREATE and THREAD_JOIN IPC requests.
 * Creates TCBs in child VSpaces with allocated stack and IPC buffer.
 */
#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4utils/process.h>
#include <sel4utils/api.h>
#include <vka/capops.h>
#include <vka/object.h>
#include "aios/root_shared.h"


/* -- Create a thread inside a child process VSpace -- */
int create_child_thread(int proc_idx, seL4_Word entry, seL4_Word arg,
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

    /* 4. Map IPC buffer into child VSpace */
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

    /* 7. Configure TCB: child CSpace + VSpace */
    /*
     * fault_ep is a RAW CPtr stored in the TCB -- kernel resolves it
     * from the THREAD CSpace at fault time, not root.
     * So copy the fault ep cap into the child CSpace.
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
        child_fault_cap,                      /* fault ep CPtr in CHILD CSpace */
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

    /* 8. Priority -- same as everything else */
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

/* -- Thread server -- handles THREAD_CREATE / THREAD_JOIN IPC -- */
void thread_server_fn(void *arg0, void *arg1, void *ipc_buf) {
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

            /* Save reply cap, block on thread fault ep */
            seL4_CNode_Delete(seL4_CapInitThreadCNode,
                              reply_slot, seL4_WordBits);
            seL4_CNode_SaveCaller(seL4_CapInitThreadCNode,
                                   reply_slot, seL4_WordBits);

            aios_thread_t *t = &ap->threads[tidx];
            seL4_Word child_badge;
            seL4_Recv(t->fault_ep.cptr, &child_badge);

            /* Read x0 from faulted thread (holds return value) */
            seL4_UserContext join_regs;
            int join_nregs = sizeof(join_regs) / sizeof(seL4_Word);
            seL4_TCB_ReadRegisters(t->tcb.cptr, 0, 0,
                                   join_nregs, &join_regs);
            seL4_Word thread_retval = join_regs.x0;

            /* Thread exited -- revoke derived caps, then free objects */
            {
                cspacepath_t p;
                vka_cspace_make_path(&vka, t->tcb.cptr, &p);
                seL4_CNode_Revoke(p.root, p.capPtr, p.capDepth);
                vka_cspace_make_path(&vka, t->fault_ep.cptr, &p);
                seL4_CNode_Revoke(p.root, p.capPtr, p.capDepth);
                vka_cspace_make_path(&vka, t->ipc_frame.cptr, &p);
                seL4_CNode_Revoke(p.root, p.capPtr, p.capDepth);
                for (int i = 0; i < THREAD_STACK_PAGES; i++) {
                    vka_cspace_make_path(&vka, t->stack_frames[i].cptr, &p);
                    seL4_CNode_Revoke(p.root, p.capPtr, p.capDepth);
                }
            }
            vka_free_object(&vka, &t->tcb);
            vka_free_object(&vka, &t->fault_ep);
            vka_free_object(&vka, &t->ipc_frame);
            for (int i = 0; i < THREAD_STACK_PAGES; i++)
                vka_free_object(&vka, &t->stack_frames[i]);
            t->active = 0;
            t->exited = 1;
            ap->num_threads--;

            seL4_SetMR(0, 0);
            seL4_SetMR(1, thread_retval);
            seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 2));
            break;
        }
        default:
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
    }
}
