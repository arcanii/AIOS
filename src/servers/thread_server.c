#define LOG_MODULE "thread"
#define LOG_LEVEL  LOG_LEVEL_INFO
#include "aios/aios_log.h"
#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4utils/process.h>
#include <sel4utils/api.h>
#include <vka/capops.h>
#include <vka/object.h>
#include "aios/root_shared.h"
#include "aios/vka_audit.h"


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
    vka_audit_tcb(VKA_SUB_THREAD);
    if (vka_alloc_tcb(&vka, &t->tcb)) return -1;

    /* 2. Mint a badged copy of the thread_server endpoint as this thread's
     * fault endpoint, so the thread's exit fault arrives on the shared loop
     * (badge >= THREAD_FAULT_BASE) instead of a dedicated per-thread endpoint
     * the server would have to block on -- that block was the head-of-line
     * stall (v0.4.191). */
    vka_audit_cslot(VKA_SUB_THREAD);
    cspacepath_t ep_src;
    vka_cspace_make_path(&vka, thread_ep_cap, &ep_src);
    if (vka_cspace_alloc_path(&vka, &t->fault_mint)) goto fail_tcb;
    {
        seL4_Word fbadge = (seL4_Word)(THREAD_FAULT_BASE
                            + proc_idx * MAX_THREADS_PER_PROC + tidx);
        if (seL4_CNode_Mint(t->fault_mint.root, t->fault_mint.capPtr,
                            t->fault_mint.capDepth,
                            ep_src.root, ep_src.capPtr, ep_src.capDepth,
                            seL4_AllRights, fbadge)) {
            vka_cspace_free(&vka, t->fault_mint.capPtr);
            goto fail_tcb;
        }
    }

    /* 3. Allocate IPC buffer frame */
    vka_audit_frame(VKA_SUB_THREAD, 1);
    if (vka_alloc_frame(&vka, seL4_PageBits, &t->ipc_frame)) goto fail_fault;

    /* 4. Map IPC buffer into child VSpace */
    void *ipc_addr = vspace_map_pages(&ap->proc.vspace,
        &t->ipc_frame.cptr, NULL, seL4_AllRights, 1, seL4_PageBits, 0);
    if (!ipc_addr) goto fail_ipc;

    /* 6. Allocate and map stack (16 KB) */
    seL4_CPtr stack_caps[THREAD_STACK_PAGES];
    for (int i = 0; i < THREAD_STACK_PAGES; i++) {
        vka_audit_frame(VKA_SUB_THREAD, 1); /* stack */
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
        &ap->proc, &vka, t->fault_mint.capPtr);
    if (child_fault_cap == 0) {
        AIOS_LOG_ERROR("Failed to copy fault ep to child");
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
        AIOS_LOG_ERROR_V("TCB_Configure failed: ", (unsigned long)err);
        goto fail_stack;
    }

    /* 8. Priority -- same as everything else */
    seL4_TCB_SetPriority(t->tcb.cptr, seL4_CapInitThreadTCB, 200);

    /* 8b. Pre-allocate the deferred-join reply slot NOW, before the thread
     * starts (v0.4.191): the THREAD_JOIN handler must SaveCaller as its first
     * action after seL4_Recv. Allocating the slot inside that handler could
     * trigger an allocman watermark refill (a nested seL4_Call / Untyped_Retype)
     * between Recv and SaveCaller, clobbering the saved reply cap on non-MCS
     * seL4. Pre-allocating here keeps the join handler invocation-free until the
     * caller is safely saved. */
    if (vka_cspace_alloc_path(&vka, &t->join_reply)) goto fail_stack;

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
        AIOS_LOG_ERROR_V("WriteRegisters failed: ", (unsigned long)err);
        goto fail_joinslot;
    }

    /* 10. Record */
    t->active = 1;
    t->tid = tidx + 1;  /* 1-based */
    t->exited = 0;
    t->retval = 0;
    t->join_pending = 0;
    ap->num_threads++;
    *out_tid = t->tid;
    return 0;

fail_joinslot:
    vka_cspace_free(&vka, t->join_reply.capPtr);
fail_stack:
    for (int i = 0; i < THREAD_STACK_PAGES; i++)
        vka_free_object(&vka, &t->stack_frames[i]);
fail_ipc:
    vka_free_object(&vka, &t->ipc_frame);
fail_fault:
    seL4_CNode_Delete(t->fault_mint.root, t->fault_mint.capPtr,
                      t->fault_mint.capDepth);
    vka_cspace_free(&vka, t->fault_mint.capPtr);
fail_tcb:
    vka_free_object(&vka, &t->tcb);
    return -1;
}

/* -- Free a thread's caps (TCB, mint, IPC frame, stacks). Revoke first to drop
 *    the child-side copies, then free. Used at exit and on create failure. -- */
static void free_thread_caps(aios_thread_t *t) {
    cspacepath_t p;
    vka_cspace_make_path(&vka, t->tcb.cptr, &p);
    seL4_CNode_Revoke(p.root, p.capPtr, p.capDepth);
    vka_cspace_make_path(&vka, t->ipc_frame.cptr, &p);
    seL4_CNode_Revoke(p.root, p.capPtr, p.capDepth);
    for (int i = 0; i < THREAD_STACK_PAGES; i++) {
        vka_cspace_make_path(&vka, t->stack_frames[i].cptr, &p);
        seL4_CNode_Revoke(p.root, p.capPtr, p.capDepth);
    }
    /* fault_mint: revoke the child's fault-cap copy, delete the mint, free the
     * root slot (does NOT touch the parent thread_ep). */
    seL4_CNode_Revoke(t->fault_mint.root, t->fault_mint.capPtr, t->fault_mint.capDepth);
    seL4_CNode_Delete(t->fault_mint.root, t->fault_mint.capPtr, t->fault_mint.capDepth);
    vka_cspace_free(&vka, t->fault_mint.capPtr);
    vka_free_object(&vka, &t->tcb);
    vka_free_object(&vka, &t->ipc_frame);
    for (int i = 0; i < THREAD_STACK_PAGES; i++)
        vka_free_object(&vka, &t->stack_frames[i]);
}

/* -- A thread faulted (exited). Capture its return value, free its caps, and
 *    either reply to a waiting joiner or leave it as a zombie for a later
 *    join. Runs on the server event loop -- never blocks. -- */
static void handle_thread_exit(int pidx, int tidx) {
    if (pidx < 0 || pidx >= MAX_ACTIVE_PROCS || !active_procs[pidx].active)
        return;
    if (tidx < 0 || tidx >= MAX_THREADS_PER_PROC) return;
    aios_thread_t *t = &active_procs[pidx].threads[tidx];
    if (!t->active || t->exited) return;   /* already handled */

    /* Suspend the TCB before freeing anything (v0.4.191 security): the per-thread
     * fault cap is a send-capable badged mint the child process holds, so a
     * process could seL4_Send on its own thread's fault cap to forge an "exit"
     * of a still-RUNNING thread. Suspending first guarantees the thread is
     * stopped before its TCB/stack/IPC frames are retyped, closing the
     * use-after-free; the worst a forged exit can then do is kill the caller's
     * own thread early (self-inflicted, not a cross-process hazard). On a
     * genuine fault the thread is already blocked, so this is a no-op. */
    seL4_TCB_Suspend(t->tcb.cptr);

    /* Read x0 (pthread return value) before the TCB is freed. */
    seL4_UserContext regs;
    int nregs = sizeof(regs) / sizeof(seL4_Word);
    for (int i = 0; i < nregs; i++) ((seL4_Word *)&regs)[i] = 0;
    if (seL4_TCB_ReadRegisters(t->tcb.cptr, 0, 0, nregs, &regs))
        AIOS_LOG_WARN("thread exit: ReadRegisters failed");
    t->retval = regs.x0;

    free_thread_caps(t);
    t->exited = 1;
    active_procs[pidx].num_threads--;

    if (t->join_pending) {
        seL4_SetMR(0, 0);
        seL4_SetMR(1, t->retval);
        seL4_Send(t->join_reply.capPtr, seL4_MessageInfo_new(0, 0, 0, 2));
        seL4_CNode_Delete(t->join_reply.root, t->join_reply.capPtr,
                          t->join_reply.capDepth);
        vka_cspace_free(&vka, t->join_reply.capPtr);
        t->join_pending = 0;
        t->active = 0;   /* reaped */
    }
    /* else: zombie -- active stays 1, a later THREAD_JOIN reads retval + reaps */
}

/* -- Thread server -- handles THREAD_CREATE / THREAD_JOIN IPC -- */
void thread_server_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    (void)arg1; (void)ipc_buf;

    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);

        /* ---- Thread-exit fault? (badge >= THREAD_FAULT_BASE) ----
         * The faulting thread is suspended in the fault; we tear it down and do
         * NOT reply (no resume). Decode (proc_idx, tidx) from the badge. */
        if (badge >= THREAD_FAULT_BASE) {
            seL4_Word off = badge - THREAD_FAULT_BASE;
            handle_thread_exit((int)(off / MAX_THREADS_PER_PROC),
                               (int)(off % MAX_THREADS_PER_PROC));
            continue;
        }

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
            aios_thread_t *t = &ap->threads[tidx];

            if (t->exited) {
                /* Already exited (zombie): reply immediately, free the
                 * pre-allocated (unused) join slot, and reap. */
                seL4_SetMR(0, 0);
                seL4_SetMR(1, t->retval);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
                vka_cspace_free(&vka, t->join_reply.capPtr);
                t->active = 0;
            } else if (t->join_pending) {
                /* A joiner is already waiting on this thread -- reject the
                 * second concurrent join (undefined in POSIX; deny cleanly). */
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            } else {
                /* Not exited yet: SaveCaller into the PRE-ALLOCATED slot as the
                 * first invocation after Recv (no alloc in between -- avoids the
                 * nested-Call reply-cap clobber) and return to the loop
                 * (DEFERRED). handle_thread_exit replies when the thread faults;
                 * other clients are served meanwhile (no head-of-line block). */
                seL4_CNode_SaveCaller(t->join_reply.root, t->join_reply.capPtr,
                                      t->join_reply.capDepth);
                t->join_pending = 1;
                /* no reply now */
            }
            break;
        }
        default:
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
    }
}
