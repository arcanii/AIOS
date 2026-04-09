/* AIOS servers/pipe_server.c -- Pipe, fork, exec, wait, exit server
 *
 * Handles PIPE_CREATE/WRITE/READ/CLOSE/CLOSE_WRITE, PIPE_KILL,
 * PIPE_FORK, PIPE_GETPID, PIPE_WAIT, PIPE_EXIT, PIPE_EXEC.
 * Central IPC hub for process lifecycle on seL4.
 *
 * v0.4.51: Fault delivery on pipe_ep -- child faults arrive as IPC
 * messages with label < PIPE_CREATE (60). No spin loops or NBRecv.
 * Blocking PIPE_READ via SaveCaller when pipe empty + writer alive.
 */
#include <stdio.h>
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
/* Signal fetch: client retrieves and clears pending bits */
#ifndef PIPE_SIG_FETCH
#define PIPE_SIG_FETCH 76
#endif

#include "aios/vfs.h"
#include "aios/procfs.h"

/* ---- Blocked reader table ---- */
static pipe_read_blocked_t pipe_read_blocked[MAX_PIPE_READ_BLOCKED];
static vka_object_t read_reply_objs[MAX_PIPE_READ_BLOCKED];

/* ---- Exec-wait barrier for SMP fork serialization ---- */
static struct {
    int active;
    int child_idx;
    seL4_CPtr reply_cap;
} exec_wait;
static vka_object_t exec_wait_reply_obj;
static int exec_done[MAX_ACTIVE_PROCS];

/* Wake all blocked readers on a pipe with EOF (0 bytes) */
static void wake_blocked_readers_eof(int pipe_id) {
    for (int bi = 0; bi < MAX_PIPE_READ_BLOCKED; bi++) {
        if (!pipe_read_blocked[bi].active) continue;
        if (pipe_read_blocked[bi].pipe_id != pipe_id) continue;
        seL4_SetMR(0, 0);
        seL4_Send(pipe_read_blocked[bi].reply_cap,
                  seL4_MessageInfo_new(0, 0, 0, 1));
        seL4_CNode_Delete(seL4_CapInitThreadCNode,
                          pipe_read_blocked[bi].reply_cap, seL4_WordBits);
        pipe_read_blocked[bi].active = 0;
    }
}

/* Unblock parent waiting for child exec completion */
static void check_exec_wait(int child_idx) {
    if (!exec_wait.active) return;
    if (exec_wait.child_idx != child_idx) return;
    seL4_SetMR(0, 0);
    seL4_Send(exec_wait.reply_cap, seL4_MessageInfo_new(0, 0, 0, 1));
    seL4_CNode_Delete(seL4_CapInitThreadCNode,
                      exec_wait.reply_cap, seL4_WordBits);
    exec_wait.active = 0;
}

/* Serve one blocked reader with available pipe data */
static void wake_one_blocked_reader(int pipe_id) {
    for (int bi = 0; bi < MAX_PIPE_READ_BLOCKED; bi++) {
        if (!pipe_read_blocked[bi].active) continue;
        if (pipe_read_blocked[bi].pipe_id != pipe_id) continue;
        pipe_t *p = &pipes[pipe_id];
        if (p->count == 0) break;
        int max_len = pipe_read_blocked[bi].max_len;
        int rlen = p->count < max_len ? p->count : max_len;
        /* v0.4.66: SHM wake -- copy to xfer page, reply with length only */
        if (pipe_read_blocked[bi].is_shm && p->xfer_valid) {
            if (rlen > PAGE_SIZE) rlen = PAGE_SIZE;
            for (int i = 0; i < rlen; i++)
                p->xfer_buf[i] = p->shm_buf[(p->head + i) % PIPE_BUF_SIZE];
            p->head = (p->head + rlen) % PIPE_BUF_SIZE;
            p->count -= rlen;
            seL4_SetMR(0, (seL4_Word)rlen);
            seL4_Send(pipe_read_blocked[bi].reply_cap,
                      seL4_MessageInfo_new(0, 0, 0, 1));
        } else {
            /* MR-based wake (original path) */
            if (rlen > 900) rlen = 900;
            seL4_SetMR(0, (seL4_Word)rlen);
            int mr = 1;
            seL4_Word w = 0;
            for (int i = 0; i < rlen; i++) {
                char c = p->shm_buf[(p->head + i) % PIPE_BUF_SIZE];
                w |= ((seL4_Word)(uint8_t)c) << ((i % 8) * 8);
                if (i % 8 == 7 || i == rlen - 1) {
                    seL4_SetMR(mr++, w); w = 0;
                }
            }
            p->head = (p->head + rlen) % PIPE_BUF_SIZE;
            p->count -= rlen;
            seL4_Send(pipe_read_blocked[bi].reply_cap,
                      seL4_MessageInfo_new(0, 0, 0, mr));
        }
        seL4_CNode_Delete(seL4_CapInitThreadCNode,
                          pipe_read_blocked[bi].reply_cap, seL4_WordBits);
        pipe_read_blocked[bi].active = 0;
        break;
    }
}

/* Auto-free pipe when all refs dropped */
static void pipe_maybe_free(int pi) {
    if (pi < 0 || pi >= MAX_PIPES) return;
    pipe_t *p = &pipes[pi];
    if (p->read_refs <= 0 && p->write_refs <= 0) {
        if (p->xfer_valid) {
            /* v0.4.67: delete cap copies so untyped can be reclaimed */
            for (int ci = 0; ci < p->xfer_copy_count; ci++) {
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                    p->xfer_copies[ci], seL4_WordBits);
                vka_cspace_free(&vka, p->xfer_copies[ci]);
            }
            p->xfer_copy_count = 0;
            /* Revoke any remaining derived caps, then free */
            cspacepath_t xp;
            vka_cspace_make_path(&vka, p->xfer_frame.cptr, &xp);
            seL4_CNode_Revoke(xp.root, xp.capPtr, xp.capDepth);
            vspace_unmap_pages(&vspace, p->xfer_buf, 1, seL4_PageBits, NULL);
            vka_free_object(&vka, &p->xfer_frame);
            p->xfer_buf = NULL;
            p->xfer_valid = 0;
        }
        if (p->shm_valid) {
            vspace_unmap_pages(&vspace, p->shm_buf, 1, seL4_PageBits, NULL);
            vka_free_object(&vka, &p->shm_frame);
            p->shm_buf = p->buf;
            p->shm_valid = 0;
        }
        p->active = 0;
    }
}

/* Handle child fault arriving on pipe_ep */
static void handle_child_fault(int child_idx) {
    active_proc_t *ch = &active_procs[child_idx];

    if (!ch->active || !ch->fault_on_pipe_ep) return;

    /* Clear foreground tracking if this was the fg process */
    if (ch->pid == fg_pid) {
        fg_pid = -1;
        fg_fault_ep = 0;
    }

    /* Auto-close write end if child was writing to a pipe.
     * Guard: if exit_cb already sent PIPE_CLOSE_WRITE, write_closed
     * is set and refs already decremented. Only act on crashes. */
    if (ch->stdout_pipe_id >= 0 && ch->stdout_pipe_id < MAX_PIPES
        && pipes[ch->stdout_pipe_id].active
        && !pipes[ch->stdout_pipe_id].write_closed) {
        pipes[ch->stdout_pipe_id].write_refs--;
        if (pipes[ch->stdout_pipe_id].write_refs <= 0) {
            pipes[ch->stdout_pipe_id].write_closed = 1;

            wake_blocked_readers_eof(ch->stdout_pipe_id);
        }
        pipe_maybe_free(ch->stdout_pipe_id);
    }
    /* Auto-close read end if child was reading from a pipe */
    if (ch->stdin_pipe_id >= 0 && ch->stdin_pipe_id < MAX_PIPES
        && pipes[ch->stdin_pipe_id].active
        && !pipes[ch->stdin_pipe_id].read_closed) {
        pipes[ch->stdin_pipe_id].read_refs--;
        pipes[ch->stdin_pipe_id].read_closed = 1;
        pipe_maybe_free(ch->stdin_pipe_id);
    }
    reap_forked_child(child_idx);
    check_exec_wait(child_idx);
}

/* Mint badged pipe_ep as fault endpoint for a child process */
static int mint_pipe_fault_ep(int child_idx, vka_object_t *out) {
    cspacepath_t src, dest;
    vka_cspace_make_path(&vka, pipe_ep_cap, &src);
    if (vka_cspace_alloc_path(&vka, &dest)) return -1;
    int err = seL4_CNode_Mint(dest.root, dest.capPtr, dest.capDepth,
        src.root, src.capPtr, src.capDepth,
        seL4_AllRights, (seL4_Word)(child_idx + 1));
    if (err) {
        vka_cspace_free(&vka, dest.capPtr);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->cptr = dest.capPtr;
    return 0;
}

void pipe_server_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    (void)arg1; (void)ipc_buf;

    /* Init pipes */
    for (int i = 0; i < MAX_PIPES; i++) pipes[i].active = 0;
    wait_init();

    /* Init blocked reader table */
    for (int i = 0; i < MAX_PIPE_READ_BLOCKED; i++) {
        pipe_read_blocked[i].active = 0;
        cspacepath_t path;
        vka_cspace_alloc_path(&vka, &path);
        read_reply_objs[i].cptr = path.capPtr;
    }

    /* Init exec-wait barrier */
    exec_wait.active = 0;
    {
        cspacepath_t path;
        vka_cspace_alloc_path(&vka, &path);
        exec_wait_reply_obj.cptr = path.capPtr;
    }
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++) exec_done[i] = 0;

    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);

        /* ---- Fault detection: labels below PIPE_CREATE are seL4 faults ---- */
        if (label < PIPE_CREATE && badge > 0 && badge <= MAX_ACTIVE_PROCS) {
            int ci = (int)badge - 1;
            if (ci >= 0 && ci < MAX_ACTIVE_PROCS
                && active_procs[ci].active
                && active_procs[ci].fault_on_pipe_ep) {
                handle_child_fault(ci);
                continue;  /* No reply for faults */
            }
        }

        switch (label) {
        case PIPE_CREATE: {
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
            pipes[pi].read_refs = 1;
            pipes[pi].write_refs = 1;
            /* v0.4.65: allocate shared frame for pipe buffer */
            pipes[pi].shm_valid = 0;
            pipes[pi].shm_buf = pipes[pi].buf;
            vka_audit_frame(VKA_SUB_PIPE, 1);
            if (vka_alloc_frame(&vka, seL4_PageBits, &pipes[pi].shm_frame) == 0) {
                void *mapped = vspace_map_pages(&vspace,
                    &pipes[pi].shm_frame.cptr, NULL,
                    seL4_AllRights, 1, seL4_PageBits, 1);
                if (mapped) {
                    pipes[pi].shm_buf = (char *)mapped;
                    pipes[pi].shm_valid = 1;
                } else {
                    vka_free_object(&vka, &pipes[pi].shm_frame);
                }
            }
            /* v0.4.66: xfer page allocated lazily on first PIPE_MAP_SHM */
            pipes[pi].xfer_valid = 0;
            pipes[pi].xfer_buf = NULL;
            pipes[pi].xfer_copy_count = 0;
            seL4_SetMR(0, (seL4_Word)pi);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_WRITE: {
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
                p->shm_buf[(p->head + p->count) % PIPE_BUF_SIZE] = c;
                p->count++;
                written++;
            }
            seL4_SetMR(0, (seL4_Word)written);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            /* Wake any blocked reader now that data is available */
            if (written > 0) wake_one_blocked_reader(pi);
            break;
        }
        case PIPE_READ: {
            int pi = (int)seL4_GetMR(0);
            int max_len = (int)seL4_GetMR(1);
            if (pi < 0 || pi >= MAX_PIPES || !pipes[pi].active) {
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            pipe_t *p = &pipes[pi];

            /* Pipe empty -- check if writer is done */
            if (p->count == 0) {
                if (p->write_closed) {
                    /* EOF: writer closed, no more data */
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                /* Writer alive -- block reader via SaveCaller */
                int bi = -1;
                for (int b = 0; b < MAX_PIPE_READ_BLOCKED; b++) {
                    if (!pipe_read_blocked[b].active) { bi = b; break; }
                }
                if (bi < 0) {
                    /* No slots -- return 0 (caller retries) */
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                    read_reply_objs[bi].cptr, seL4_WordBits);
                seL4_CNode_SaveCaller(seL4_CapInitThreadCNode,
                    read_reply_objs[bi].cptr, seL4_WordBits);
                pipe_read_blocked[bi].active = 1;
                pipe_read_blocked[bi].pipe_id = pi;
                pipe_read_blocked[bi].max_len = max_len;
                pipe_read_blocked[bi].is_shm = 0;
                pipe_read_blocked[bi].reply_cap = read_reply_objs[bi].cptr;
                /* Do NOT reply -- reader blocks until data or EOF */
                break;
            }

            /* Data available -- serve immediately */
            int rlen = p->count < max_len ? p->count : max_len;
            if (rlen > 900) rlen = 900;
            seL4_SetMR(0, (seL4_Word)rlen);
            int mr = 1;
            seL4_Word w = 0;
            for (int i = 0; i < rlen; i++) {
                char c = p->shm_buf[(p->head + i) % PIPE_BUF_SIZE];
                w |= ((seL4_Word)(uint8_t)c) << ((i % 8) * 8);
                if (i % 8 == 7 || i == rlen - 1) {
                    seL4_SetMR(mr++, w); w = 0;
                }
            }
            p->head = (p->head + rlen) % PIPE_BUF_SIZE;
            p->count -= rlen;
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, mr));
            break;
        }
        case PIPE_CLOSE: {
            int pi = (int)seL4_GetMR(0);
            if (pi >= 0 && pi < MAX_PIPES && pipes[pi].active) {
                pipes[pi].read_refs = 0;
                pipes[pi].write_refs = 0;
                pipes[pi].active = 0;
                /* Wake any blocked readers with EOF */
                wake_blocked_readers_eof(pi);
                /* shm frame freed via pipe_maybe_free refcount path */
                pipe_maybe_free(pi);
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_CLOSE_WRITE: {
            int pi = (int)seL4_GetMR(0);
            if (pi >= 0 && pi < MAX_PIPES && pipes[pi].active) {
                pipes[pi].write_refs--;
                if (pipes[pi].write_refs <= 0) {
                    pipes[pi].write_closed = 1;
                    wake_blocked_readers_eof(pi);
                }
                pipe_maybe_free(pi);
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_CLOSE_READ: {
            int pi = (int)seL4_GetMR(0);
            if (pi >= 0 && pi < MAX_PIPES && pipes[pi].active) {
                pipes[pi].read_refs--;
                pipes[pi].read_closed = 1;
                pipe_maybe_free(pi);
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_KILL: {
            /* PIPE_KILL: reap path for proper zombie creation.
             * find active_proc index, then handle_child_fault
             * which does pipe cleanup + reap_forked_child. */
            int pid = (int)seL4_GetMR(0);
            int ki = -1;
            for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
                if (active_procs[i].active && active_procs[i].pid == pid) {
                    ki = i; break;
                }
            }
            if (ki < 0) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            active_procs[ki].exit_status = 9; /* SIGKILL */
            if (active_procs[ki].fault_on_pipe_ep) {
                handle_child_fault(ki);
            } else {
                /* Non-forked process: use direct destroy + reap */
                reap_forked_child(ki);
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_GETPID: {
            int caller_idx = (int)badge - 1;
            int pid = -1;
            if (caller_idx >= 0 && caller_idx < MAX_ACTIVE_PROCS
                && active_procs[caller_idx].active) {
                pid = active_procs[caller_idx].pid;
            }
            seL4_SetMR(0, (seL4_Word)pid);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_EXIT: {
            /* Child sends exit code before dying via NULL deref.
             * After reply, child faults. The fault arrives on pipe_ep
             * as a normal IPC message (label < PIPE_CREATE) on a
             * future iteration. No spinning needed. */
            int caller_idx = (int)badge - 1;
            int exit_code = (int)seL4_GetMR(0);
            if (caller_idx >= 0 && caller_idx < MAX_ACTIVE_PROCS
                && active_procs[caller_idx].active) {
                active_procs[caller_idx].exit_status = exit_code;
                /* Clear fg tracking so Ctrl-C state is clean */
                if (active_procs[caller_idx].pid == fg_pid) {
                    fg_pid = -1;
                    fg_fault_ep = 0;
                }
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }
        case PIPE_WAIT: {
            int caller_idx = (int)badge - 1;
            int target_pid = (int)seL4_GetMR(0);

            if (caller_idx < 0 || caller_idx >= MAX_ACTIVE_PROCS
                || !active_procs[caller_idx].active) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int caller_pid = active_procs[caller_idx].pid;

            /* Check if child already exited */
            int already_exited = 1;
            if (target_pid > 0) {
                for (int ci = 0; ci < MAX_ACTIVE_PROCS; ci++) {
                    if (active_procs[ci].active
                        && active_procs[ci].pid == target_pid
                        && active_procs[ci].ppid == caller_pid) {
                        already_exited = 0;
                        break;
                    }
                }
            } else {
                for (int ci = 0; ci < MAX_ACTIVE_PROCS; ci++) {
                    if (active_procs[ci].active
                        && active_procs[ci].ppid == caller_pid) {
                        already_exited = 0;
                        break;
                    }
                }
            }

            if (already_exited) {
                int zstatus = 0;
                int zpid = target_pid;
                for (int z = 0; z < MAX_ZOMBIES; z++) {
                    if (!zombies[z].active) continue;
                    if ((target_pid > 0 && zombies[z].pid == target_pid) ||
                        (target_pid == -1 && zombies[z].ppid == caller_pid)) {
                        zpid = zombies[z].pid;
                        zstatus = zombies[z].exit_status;
                        zombies[z].active = 0;
                        break;
                    }
                }
                seL4_SetMR(0, (seL4_Word)zpid);
                seL4_SetMR(1, (seL4_Word)zstatus);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
                break;
            }

            /* Child still alive -- defer reply via SaveCaller */
            int wi = -1;
            for (int w = 0; w < MAX_WAIT_PENDING; w++) {
                if (!wait_pending[w].active) { wi = w; break; }
            }
            if (wi < 0) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            seL4_CNode_Delete(seL4_CapInitThreadCNode,
                              wait_reply_objects[wi].cptr, seL4_WordBits);
            seL4_CNode_SaveCaller(seL4_CapInitThreadCNode,
                                   wait_reply_objects[wi].cptr, seL4_WordBits);
            wait_pending[wi].active = 1;
            wait_pending[wi].waiting_pid = caller_pid;
            wait_pending[wi].child_pid = target_pid;
            wait_pending[wi].reply_cap = wait_reply_objects[wi].cptr;
            break;
        }
        case PIPE_FORK: {
            int parent_idx = (int)badge - 1;
            if (parent_idx < 0 || parent_idx >= MAX_ACTIVE_PROCS
                || !active_procs[parent_idx].active) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int child_pid = do_fork(parent_idx);
            /* Clear exec_done for the new child so PIPE_EXEC_WAIT works */
            if (child_pid > 0) {
                for (int fi = 0; fi < MAX_ACTIVE_PROCS; fi++) {
                    if (active_procs[fi].active
                        && active_procs[fi].pid == child_pid) {
                        exec_done[fi] = 0;
                        break;
                    }
                }
            }
            seL4_SetMR(0, (seL4_Word)child_pid);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_EXEC: {
            int ci = (int)badge - 1;
            if (ci < 0 || ci >= MAX_ACTIVE_PROCS || !active_procs[ci].active) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int nmrs = seL4_MessageInfo_get_length(msg);
            seL4_Word pipe_meta_word = seL4_GetMR(0);
            int exec_stdout_pipe = (int)((pipe_meta_word >> 16) & 0xFFFF) - 1;
            int exec_stdin_pipe = (int)(pipe_meta_word & 0xFFFF) - 1;

            char exec_buf[900];
            int ebi = 0;
            for (int m = 1; m < nmrs && ebi < 899; m++) {
                seL4_Word w = seL4_GetMR(m);
                for (int b = 0; b < 8 && ebi < 899; b++) {
                    exec_buf[ebi++] = (char)((w >> (b * 8)) & 0xFF);
                }
            }
            exec_buf[ebi] = 0;

            char *elf_path = exec_buf;

            #define MAX_USER_ARGV 16
            char *exec_argv[MAX_USER_ARGV];
            int exec_argc = 0;
            char child_cwd[128];
            child_cwd[0] = '/'; child_cwd[1] = 0;
            {
                int pos = 0;
                /* Skip path */
                while (pos < ebi && exec_buf[pos] != 0) pos++;
                pos++;
                /* Parse argv entries */
                while (pos < ebi && exec_buf[pos] != 0 && exec_argc < MAX_USER_ARGV) {
                    exec_argv[exec_argc++] = &exec_buf[pos];
                    while (pos < ebi && exec_buf[pos] != 0) pos++;
                    pos++;
                }
                /* Skip past double-null */
                if (pos < ebi) pos++;
                /* Extract CWD if present */
                if (pos < ebi && exec_buf[pos] != 0) {
                    int ci = 0;
                    while (pos < ebi && exec_buf[pos] != 0 && ci < 127) {
                        child_cwd[ci++] = exec_buf[pos++];
                    }
                    child_cwd[ci] = 0;
                }
            }

            active_proc_t *ap = &active_procs[ci];
            int old_pid = ap->pid;
            int old_ppid = ap->ppid;
            uint32_t old_uid = ap->uid;
            uint32_t old_gid = ap->gid;
            vka_object_t old_fault_ep = ap->fault_ep;
            int old_fope = ap->fault_on_pipe_ep;

            int esz = vfs_read(elf_path, elf_buf, sizeof(elf_buf));
            if (esz <= 0) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            elf_t elf;
            if (elf_newFile(elf_buf, esz, &elf) != 0) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            sel4utils_destroy_process(&ap->proc, &vka);

            /* Free old fault ep -- minted cap vs allocated endpoint */
            if (old_fope) {
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                    old_fault_ep.cptr, seL4_WordBits);
                vka_cspace_free(&vka, old_fault_ep.cptr);
            } else {
                vka_free_object(&vka, &old_fault_ep);
            }

            /* Mint fresh pipe_ep as fault endpoint */
            vka_object_t new_fault_ep;
            if (mint_pipe_fault_ep(ci, &new_fault_ep)) {
                ap->active = 0;
                proc_remove(old_pid);
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            sel4utils_process_config_t cfg = process_config_new(&simple);
            cfg = process_config_create_cnode(cfg, 12);
            cfg = process_config_create_vspace(cfg, NULL, 0);
            cfg = process_config_priority(cfg, 200);
            cfg = process_config_auth(cfg, simple_get_tcb(&simple));
            cfg = process_config_fault_endpoint(cfg, new_fault_ep);

            sel4utils_process_t *proc = &ap->proc;
            int err = sel4utils_configure_process_custom(proc, &vka, &vspace, cfg);
            if (err) {
                ap->active = 0;
                proc_remove(old_pid);
                break;
            }

            ap->num_segs = 0;
            for (int si = 0; si < elf_getNumProgramHeaders(&elf)
                 && ap->num_segs < MAX_ELF_SEGS; si++) {
                if (elf_getProgramHeaderType(&elf, si) == 1) {
                    elf_seg_info_t *seg = &ap->segs[ap->num_segs++];
                    seg->vaddr = (uintptr_t)elf_getProgramHeaderVaddr(&elf, si);
                    seg->memsz = (size_t)elf_getProgramHeaderMemorySize(&elf, si);
                    seg->flags = (uint32_t)elf_getProgramHeaderFlags(&elf, si);
                }
            }

            proc->entry_point = sel4utils_elf_load(
                &proc->vspace, &vspace, &vka, &vka, &elf);
            if (!proc->entry_point) {
                sel4utils_destroy_process(proc, &vka);
                ap->active = 0;
                proc_remove(old_pid);
                break;
            }
            proc->sysinfo = sel4utils_elf_get_vsyscall(&elf);

            seL4_CPtr cs = sel4utils_copy_cap_to_process(proc, &vka, serial_ep.cptr);
            ap->child_ser_slot = cs;

            cspacepath_t fs_s, fs_d;
            vka_cspace_make_path(&vka, fs_ep_cap, &fs_s);
            vka_cspace_alloc_path(&vka, &fs_d);
            seL4_CNode_Mint(fs_d.root, fs_d.capPtr, fs_d.capDepth,
                fs_s.root, fs_s.capPtr, fs_s.capDepth,
                seL4_AllRights, (seL4_Word)(ci + 1));
            seL4_CPtr cf = sel4utils_copy_cap_to_process(proc, &vka, fs_d.capPtr);
            ap->child_fs_slot = cf;

            cspacepath_t te_s, te_d;
            vka_cspace_make_path(&vka, thread_ep_cap, &te_s);
            vka_cspace_alloc_path(&vka, &te_d);
            seL4_CNode_Mint(te_d.root, te_d.capPtr, te_d.capDepth,
                te_s.root, te_s.capPtr, te_s.capDepth,
                seL4_AllRights, (seL4_Word)(ci + 1));
            seL4_CPtr ct = sel4utils_copy_cap_to_process(proc, &vka, te_d.capPtr);
            ap->child_thread_slot = ct;

            seL4_CPtr ca = sel4utils_copy_cap_to_process(proc, &vka, auth_ep_cap);
            ap->child_auth_slot = ca;

            cspacepath_t pi_s, pi_d;
            vka_cspace_make_path(&vka, pipe_ep_cap, &pi_s);
            vka_cspace_alloc_path(&vka, &pi_d);
            seL4_CNode_Mint(pi_d.root, pi_d.capPtr, pi_d.capDepth,
                pi_s.root, pi_s.capPtr, pi_s.capDepth,
                seL4_AllRights, (seL4_Word)(ci + 1));
            seL4_CPtr cp2 = sel4utils_copy_cap_to_process(proc, &vka, pi_d.capPtr);
            ap->child_pipe_slot = cp2;

            char s_ser[16], s_fs[16], s_thr[16], s_auth[16], s_pip[16];
            char cwd[64];
            snprintf(s_ser, 16, "%lu", (unsigned long)cs);
            snprintf(s_fs, 16, "%lu", (unsigned long)cf);
            snprintf(s_thr, 16, "%lu", (unsigned long)ct);
            snprintf(s_auth, 16, "%lu", (unsigned long)ca);
            snprintf(s_pip, 16, "%lu", (unsigned long)cp2);

            if (exec_stdout_pipe >= 0 || exec_stdin_pipe >= 0) {
                int sp_id = exec_stdout_pipe < 0 ? 99 : exec_stdout_pipe;
                int rp_id = exec_stdin_pipe < 0 ? 99 : exec_stdin_pipe;
                snprintf(cwd, 64, "%u:%u:%d:%d:%s",
                         old_uid, old_gid, sp_id, rp_id, child_cwd);
            } else {
                snprintf(cwd, 64, "%u:%u:%s", old_uid, old_gid, child_cwd);
            }

            char *spawn_argv[6 + MAX_USER_ARGV];
            int spawn_argc = 0;
            spawn_argv[spawn_argc++] = s_ser;
            spawn_argv[spawn_argc++] = s_fs;
            spawn_argv[spawn_argc++] = s_thr;
            spawn_argv[spawn_argc++] = s_auth;
            spawn_argv[spawn_argc++] = s_pip;
            spawn_argv[spawn_argc++] = cwd;
            if (exec_argc > 0) {
                for (int ai = 0; ai < exec_argc
                     && spawn_argc < 6 + MAX_USER_ARGV; ai++)
                    spawn_argv[spawn_argc++] = exec_argv[ai];
            } else {
                spawn_argv[spawn_argc++] = elf_path;
            }
            err = sel4utils_spawn_process_v(proc, &vka, &vspace,
                                            spawn_argc, spawn_argv, 1);
            if (err) {
                sel4utils_destroy_process(proc, &vka);
                ap->active = 0;
                proc_remove(old_pid);
                break;
            }

            ap->active = 1;
            ap->pid = old_pid;
            ap->ppid = old_ppid;
            ap->uid = old_uid;
            ap->gid = old_gid;
            ap->fault_ep = new_fault_ep;
            ap->fault_on_pipe_ep = 1;
            ap->stdout_pipe_id = exec_stdout_pipe;
            ap->stdin_pipe_id = exec_stdin_pipe;
            ap->exit_status = 0;
            ap->num_threads = 0;
            for (int ti = 0; ti < MAX_THREADS_PER_PROC; ti++)
                ap->threads[ti].active = 0;

            for (int pi = 0; pi < PROC_MAX; pi++) {
                if (proc_table[pi].active && proc_table[pi].pid == old_pid) {
                    int ni = 0;
                    const char *n = elf_path;
                    while (*n && ni < 63) proc_table[pi].name[ni++] = *n++;
                    proc_table[pi].name[ni] = 0;
                    break;
                }
            }
            exec_done[ci] = 1;
            check_exec_wait(ci);

            /* Track as foreground process for Ctrl-C */
            fg_pid = old_pid;
            fg_fault_ep = new_fault_ep.cptr;

            /* No reply -- old process destroyed, new one running */
            break;
        }
        case PIPE_EXEC_WAIT: {
            int target_pid = (int)seL4_GetMR(0);
            int target_idx = -1;
            for (int ti = 0; ti < MAX_ACTIVE_PROCS; ti++) {
                if (active_procs[ti].active
                    && active_procs[ti].pid == target_pid) {
                    target_idx = ti;
                    break;
                }
            }
            if (target_idx < 0 || exec_done[target_idx]) {
                /* Child already exec-ed or not found -- unblock now */
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            /* Defer reply until child PIPE_EXEC completes */
            seL4_CNode_Delete(seL4_CapInitThreadCNode,
                exec_wait_reply_obj.cptr, seL4_WordBits);
            seL4_CNode_SaveCaller(seL4_CapInitThreadCNode,
                exec_wait_reply_obj.cptr, seL4_WordBits);
            exec_wait.active = 1;
            exec_wait.child_idx = target_idx;
            exec_wait.reply_cap = exec_wait_reply_obj.cptr;
            break;
        }
        case PIPE_MAP_SHM: {
            /* v0.4.66: lazy xfer */
            int pi = (int)seL4_GetMR(0);
            int ci = (int)badge - 1;
            if (pi < 0 || pi >= MAX_PIPES || !pipes[pi].active
                || ci < 0 || ci >= MAX_ACTIVE_PROCS
                || !active_procs[ci].active) {
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            /* Lazy alloc: create xfer frame on first request */
            if (!pipes[pi].xfer_valid) {
                vka_audit_frame(VKA_SUB_PIPE, 1);
                if (vka_alloc_frame(&vka, seL4_PageBits,
                        &pipes[pi].xfer_frame) != 0) {
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                void *xm = vspace_map_pages(&vspace,
                    &pipes[pi].xfer_frame.cptr, NULL,
                    seL4_AllRights, 1, seL4_PageBits, 1);
                if (!xm) {
                    vka_free_object(&vka, &pipes[pi].xfer_frame);
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                pipes[pi].xfer_buf = (char *)xm;
                pipes[pi].xfer_valid = 1;
            }
            /* Copy frame cap to fresh slot */
            cspacepath_t xsrc, xdest;
            vka_cspace_make_path(&vka, pipes[pi].xfer_frame.cptr, &xsrc);
            if (vka_cspace_alloc_path(&vka, &xdest) != 0) {
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            if (seL4_CNode_Copy(xdest.root, xdest.capPtr, xdest.capDepth,
                    xsrc.root, xsrc.capPtr, xsrc.capDepth,
                    seL4_AllRights) != 0) {
                vka_cspace_free(&vka, xdest.capPtr);
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            /* Map the COPY into child VSpace */
            void *child_vaddr = vspace_map_pages(
                &active_procs[ci].proc.vspace,
                &xdest.capPtr, NULL,
                seL4_AllRights, 1, seL4_PageBits, 0);
            if (!child_vaddr) {
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                    xdest.capPtr, seL4_WordBits);
                vka_cspace_free(&vka, xdest.capPtr);
            } else if (pipes[pi].xfer_copy_count < 2) {
                pipes[pi].xfer_copies[pipes[pi].xfer_copy_count++] =
                    xdest.capPtr;
            }
            seL4_SetMR(0, (seL4_Word)child_vaddr);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_WRITE_SHM: {
            /* v0.4.66: writer put data in xfer page, copy to ring buffer */
            int pi = (int)seL4_GetMR(0);
            int wlen = (int)seL4_GetMR(1);
            if (pi < 0 || pi >= MAX_PIPES || !pipes[pi].active
                || !pipes[pi].xfer_valid) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            pipe_t *p = &pipes[pi];
            if (wlen > PAGE_SIZE) wlen = PAGE_SIZE;
            int written = 0;
            for (int i = 0; i < wlen && p->count < PIPE_BUF_SIZE; i++) {
                p->shm_buf[(p->head + p->count) % PIPE_BUF_SIZE] =
                    p->xfer_buf[i];
                p->count++;
                written++;
            }
            seL4_SetMR(0, (seL4_Word)written);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            if (written > 0) wake_one_blocked_reader(pi);
            break;
        }
        case PIPE_READ_SHM: {
            /* v0.4.66: copy ring buffer to xfer page, reply with length */
            int pi = (int)seL4_GetMR(0);
            int max_len = (int)seL4_GetMR(1);
            if (pi < 0 || pi >= MAX_PIPES || !pipes[pi].active
                || !pipes[pi].xfer_valid) {
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            pipe_t *p = &pipes[pi];
            if (max_len > PAGE_SIZE) max_len = PAGE_SIZE;
            if (p->count == 0) {
                if (p->write_closed) {
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                /* Block reader -- same as PIPE_READ but with is_shm flag */
                int bi = -1;
                for (int b = 0; b < MAX_PIPE_READ_BLOCKED; b++) {
                    if (!pipe_read_blocked[b].active) { bi = b; break; }
                }
                if (bi < 0) {
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                    read_reply_objs[bi].cptr, seL4_WordBits);
                seL4_CNode_SaveCaller(seL4_CapInitThreadCNode,
                    read_reply_objs[bi].cptr, seL4_WordBits);
                pipe_read_blocked[bi].active = 1;
                pipe_read_blocked[bi].pipe_id = pi;
                pipe_read_blocked[bi].max_len = max_len;
                pipe_read_blocked[bi].is_shm = 1;
                pipe_read_blocked[bi].reply_cap = read_reply_objs[bi].cptr;
                break;
            }
            int rlen = p->count < max_len ? p->count : max_len;
            for (int i = 0; i < rlen; i++)
                p->xfer_buf[i] = p->shm_buf[(p->head + i) % PIPE_BUF_SIZE];
            p->head = (p->head + rlen) % PIPE_BUF_SIZE;
            p->count -= rlen;
            seL4_SetMR(0, (seL4_Word)rlen);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_SET_PIPES: {
            /* v0.4.67: update stdout/stdin pipe IDs in active_proc.
             * Sent by dup2 so server knows pipe assignments even
             * for forked children that never call exec. */
            int ci = (int)badge - 1;
            if (ci >= 0 && ci < MAX_ACTIVE_PROCS
                && active_procs[ci].active) {
                int new_stdout = (int)seL4_GetMR(0);
                int new_stdin  = (int)seL4_GetMR(1);

                if (new_stdout >= -1)
                    active_procs[ci].stdout_pipe_id = new_stdout;
                if (new_stdin >= -1)
                    active_procs[ci].stdin_pipe_id = new_stdin;
                seL4_SetMR(0, 0);
            } else {
                seL4_SetMR(0, (seL4_Word)-1);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_SET_IDENTITY: {
            /* Update calling process uid/gid in active_procs
             * (used by getty after login so fork+exec inherits identity) */
            int ci = (int)badge - 1;
            if (ci >= 0 && ci < MAX_ACTIVE_PROCS && active_procs[ci].active) {
                active_procs[ci].uid = (uint32_t)seL4_GetMR(0);
                active_procs[ci].gid = (uint32_t)seL4_GetMR(1);
                seL4_SetMR(0, 0);
            } else {
                seL4_SetMR(0, (seL4_Word)-1);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_SIGNAL: {
            /* kill(pid, sig) -- POSIX signal delivery
             * MR0 = target pid, MR1 = signal number
             * SIGKILL(9)/SIGTERM(15): immediate process destruction
             * sig 0: existence check only (POSIX)
             * Others: set pending bit in target active_proc */
            int target_pid = (int)seL4_GetMR(0);
            int signum     = (int)seL4_GetMR(1);

            /* Find target process */
            int ti = -1;
            for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
                if (active_procs[i].active && active_procs[i].pid == target_pid) {
                    ti = i;
                    break;
                }
            }

            if (ti < 0) {
                /* pid not found */
                seL4_SetMR(0, (seL4_Word)(-3)); /* -ESRCH */
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            if (signum == 0) {
                /* Existence check only */
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            if (signum == 9 || signum == 15) {
                /* SIGKILL / SIGTERM: destroy via reap path so
                 * zombie is created and waitpid unblocks.
                 * POSIX wait status: signal death = raw signum. */
                active_procs[ti].exit_status = signum;
                handle_child_fault(ti);
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            /* Other signals: set pending bit */
            if (signum >= 1 && signum < 32) {
                active_procs[ti].sig_pending |= (1U << (signum - 1));
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_DEBUG: {
            /* Dump active_procs and pipe status via serial */
            printf("[DEBUG] active_procs:\n");
            for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
                if (active_procs[i].active) {
                    printf("  [%d] pid=%d ppid=%d fope=%d spipe=%d\n",
                        i, active_procs[i].pid, active_procs[i].ppid,
                        active_procs[i].fault_on_pipe_ep,
                        active_procs[i].stdout_pipe_id);
                }
            }
            printf("[DEBUG] pipes:\n");
            for (int i = 0; i < MAX_PIPES; i++) {
                if (pipes[i].active) {
                    printf("  [%d] count=%d wc=%d rc=%d wr=%d rr=%d shm=%d\n",
                        i, pipes[i].count,
                        pipes[i].write_closed, pipes[i].read_closed,
                        pipes[i].write_refs, pipes[i].read_refs,
                        pipes[i].shm_valid);
                }
            }
            vka_audit_dump();
            printf("[DEBUG] wait_pending:\n");
            for (int i = 0; i < MAX_WAIT_PENDING; i++) {
                if (wait_pending[i].active) {
                    printf("  [%d] waiter=%d child=%d\n",
                        i, wait_pending[i].waiting_pid,
                        wait_pending[i].child_pid);
                }
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_SIG_FETCH: {
            /* Return pending signal mask for caller, then clear it.
             * Client merges into local sigstate and dispatches handlers. */
            int caller_idx = (int)badge - 1;
            uint32_t pending = 0;
            if (caller_idx >= 0 && caller_idx < MAX_ACTIVE_PROCS
                && active_procs[caller_idx].active) {
                pending = active_procs[caller_idx].sig_pending;
                active_procs[caller_idx].sig_pending = 0;
            }
            seL4_SetMR(0, (seL4_Word)pending);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_SHUTDOWN: {
            /* Only root (badge 0 or uid 0) can shut down */
            int shut_idx = (int)badge - 1;
            int allowed = (badge == 0);
            if (!allowed && shut_idx >= 0 && shut_idx < MAX_ACTIVE_PROCS
                && active_procs[shut_idx].active
                && active_procs[shut_idx].uid == 0) {
                allowed = 1;
            }
            if (!allowed) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            printf("[shutdown] AIOS powering off...\n");
            aios_system_shutdown();
            break;
        }
        default:
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
    }
}
