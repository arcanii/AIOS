/* AIOS process/reap.c -- Child reaping and waitpid support
 *
 * Manages zombie table, wait_pending table, and reap logic.
 * Called from pipe_server and fs_thread for child exit handling.
 */
#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4utils/process.h>
#include <vka/capops.h>
#include <vka/object.h>
#include "aios/root_shared.h"
#include "aios/procfs.h"

/* Variable definitions (extern in root_shared.h) */
wait_pending_t wait_pending[MAX_WAIT_PENDING];
vka_object_t wait_reply_objects[MAX_WAIT_PENDING];
int wait_pending_init = 0;
zombie_t zombies[MAX_ZOMBIES];


void wait_init(void) {
    for (int i = 0; i < MAX_WAIT_PENDING; i++) {
        wait_pending[i].active = 0;
        /* Pre-allocate CSpace slots for SaveCaller */
        cspacepath_t path;
        vka_cspace_alloc_path(&vka, &path);
        wait_reply_objects[i].cptr = path.capPtr;
    }
    for (int i = 0; i < MAX_ZOMBIES; i++) zombies[i].active = 0;
    wait_pending_init = 1;
}

/* Reap a forked child — called when we detect it has exited */
void reap_forked_child(int child_idx) {
    active_proc_t *child = &active_procs[child_idx];
    if (!child->active) return;
    int child_pid = child->pid;
    int parent_pid = child->ppid;
    int estatus = child->exit_status;

    /* Destroy the process */
    sel4utils_destroy_process(&child->proc, &vka);
    vka_free_object(&vka, &child->fault_ep);
    proc_remove(child_pid);
    child->active = 0;

    /* Check if any parent is waiting for this child */
    int delivered = 0;
    for (int w = 0; w < MAX_WAIT_PENDING; w++) {
        if (!wait_pending[w].active) continue;
        if (wait_pending[w].child_pid == child_pid ||
            (wait_pending[w].child_pid == -1 &&
             wait_pending[w].waiting_pid == parent_pid)) {
            /* Reply to the waiting parent */
            seL4_SetMR(0, (seL4_Word)child_pid);
            seL4_SetMR(1, (seL4_Word)estatus);
            seL4_Send(wait_pending[w].reply_cap, seL4_MessageInfo_new(0, 0, 0, 2));
            seL4_CNode_Delete(seL4_CapInitThreadCNode,
                              wait_pending[w].reply_cap, seL4_WordBits);
            wait_pending[w].active = 0;
            delivered = 1;
            break;
        }
    }

    /* No waiter — save as zombie for later waitpid */
    if (!delivered) {
        for (int z = 0; z < MAX_ZOMBIES; z++) {
            if (!zombies[z].active) {
                zombies[z].active = 1;
                zombies[z].pid = child_pid;
                zombies[z].ppid = parent_pid;
                zombies[z].exit_status = estatus;
                break;
            }
        }
    }
}

/* Check all forked children for exit (non-blocking) */
void reap_check(void) {
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
        if (!active_procs[i].active) continue;
        if (active_procs[i].ppid <= 0) continue;  /* not a forked child */
        /* Non-blocking check on fault EP */
        seL4_MessageInfo_t probe = seL4_NBRecv(active_procs[i].fault_ep.cptr, NULL);
        if (seL4_MessageInfo_get_label(probe) == 0) continue;
            i, active_procs[i].pid, active_procs[i].ppid,
            (unsigned long)seL4_MessageInfo_get_label(probe));

        /* Child faulted/exited. Check if parent is waiting. */
        int ppid = active_procs[i].ppid;
        int has_waiter = 0;
        for (int w = 0; w < MAX_WAIT_PENDING; w++) {
            if (!wait_pending[w].active) continue;
            if (wait_pending[w].waiting_pid == ppid &&
                (wait_pending[w].child_pid == active_procs[i].pid ||
                 wait_pending[w].child_pid == -1)) {
                has_waiter = 1;
                break;
            }
        }

        if (has_waiter) {
            /* Parent is waiting — reap and deliver exit status */
            reap_forked_child(i);
        } else {
            /* Check if parent is still alive */
            int parent_alive = 0;
            for (int p = 0; p < MAX_ACTIVE_PROCS; p++) {
                if (active_procs[p].active && active_procs[p].pid == ppid) {
                    parent_alive = 1;
                    break;
                }
            }
            if (parent_alive) {
                /* Parent alive but hasn't called waitpid yet — save as zombie.
                 * Don't consume the fault — put it back by not destroying yet.
                 * Actually we already consumed the NBRecv. Save to zombie and destroy. */
                reap_forked_child(i);
            } else {
                /* Orphan — just clean up */
                reap_forked_child(i);
            }
        }
    }
}

