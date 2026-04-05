/* AIOS servers/pipe_server.c -- Pipe, fork, exec, wait, exit server
 *
 * Handles PIPE_CREATE/WRITE/READ/CLOSE, PIPE_KILL,
 * PIPE_FORK, PIPE_GETPID, PIPE_WAIT, PIPE_EXIT, PIPE_EXEC.
 * Central IPC hub for process lifecycle on seL4.
 */
#include <stdio.h>
#include <stdlib.h>
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
#include "aios/vfs.h"
#include "aios/procfs.h"

/* ── waitpid support ── */


/* Zombie table: stores exit status of children that exited before parent called waitpid */

void pipe_server_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    (void)arg1; (void)ipc_buf;

    /* Init pipes */
    for (int i = 0; i < MAX_PIPES; i++) pipes[i].active = 0;
    wait_init();

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
        case PIPE_KILL: {
            int pid = (int)seL4_GetMR(0);
            int result = process_kill(pid);
            seL4_SetMR(0, (seL4_Word)result);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case PIPE_GETPID: {
            /* Return caller's PID based on badge */
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
            /* Child sends exit code before dying.
             * After we reply, child will fault. We need to catch that fault
             * and deliver it to the waiting parent. But pipe_server will be
             * blocked in seL4_Recv. Solution: after reply, busy-wait briefly
             * for the child's fault, then reap immediately. */
            int caller_idx = (int)badge - 1;
            int exit_code = (int)seL4_GetMR(0);
            if (caller_idx >= 0 && caller_idx < MAX_ACTIVE_PROCS
                && active_procs[caller_idx].active) {
                active_procs[caller_idx].exit_status = exit_code;
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));

            /* Brief spin waiting for child to fault after PIPE_EXIT reply */
            if (caller_idx >= 0 && caller_idx < MAX_ACTIVE_PROCS
                && active_procs[caller_idx].active
                && active_procs[caller_idx].ppid > 0) {
                for (int spin = 0; spin < 100; spin++) {
                    seL4_Yield();
                    seL4_MessageInfo_t probe = seL4_NBRecv(
                        active_procs[caller_idx].fault_ep.cptr, NULL);
                    if (seL4_MessageInfo_get_label(probe) != 0) {
                        reap_forked_child(caller_idx);
                        break;
                    }
                }
            }
            break;
        }
        case PIPE_WAIT: {
            /* waitpid(child_pid): MR0 = child_pid (-1 for any) */
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
                    if (active_procs[ci].active && active_procs[ci].pid == target_pid
                        && active_procs[ci].ppid == caller_pid) {
                        already_exited = 0;
                        break;
                    }
                }
            } else {
                /* Wait for any child */
                for (int ci = 0; ci < MAX_ACTIVE_PROCS; ci++) {
                    if (active_procs[ci].active && active_procs[ci].ppid == caller_pid) {
                        already_exited = 0;
                        break;
                    }
                }
            }

            if (already_exited) {
                /* Child already gone — check zombie table for exit status */
                int zstatus = 0;
                int zpid = target_pid;
                for (int z = 0; z < MAX_ZOMBIES; z++) {
                    if (!zombies[z].active) continue;
                    if ((target_pid > 0 && zombies[z].pid == target_pid) ||
                        (target_pid == -1 && zombies[z].ppid == caller_pid)) {
                        zpid = zombies[z].pid;
                        zstatus = zombies[z].exit_status;
                        zombies[z].active = 0;  /* consumed */
                        break;
                    }
                }
                seL4_SetMR(0, (seL4_Word)zpid);
                seL4_SetMR(1, (seL4_Word)zstatus);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
                break;
            }

            /* Child still alive — save reply cap and defer */
            int wi = -1;
            for (int w = 0; w < MAX_WAIT_PENDING; w++) {
                if (!wait_pending[w].active) { wi = w; break; }
            }
            if (wi < 0) {
                /* No wait slots — reply with error */
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            /* SaveCaller: save the reply cap so we can reply later */
            seL4_CNode_Delete(seL4_CapInitThreadCNode,
                              wait_reply_objects[wi].cptr, seL4_WordBits);
            seL4_CNode_SaveCaller(seL4_CapInitThreadCNode,
                                   wait_reply_objects[wi].cptr, seL4_WordBits);

            wait_pending[wi].active = 1;
            wait_pending[wi].waiting_pid = caller_pid;
            wait_pending[wi].child_pid = target_pid;
            wait_pending[wi].reply_cap = wait_reply_objects[wi].cptr;
            /* Do NOT reply — parent blocks until child exits */
            break;
        }
        case PIPE_FORK: {
            /* Badge identifies the calling process (ap_idx + 1) */
            int parent_idx = (int)badge - 1;
            if (parent_idx < 0 || parent_idx >= MAX_ACTIVE_PROCS
                || !active_procs[parent_idx].active) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            int child_pid = do_fork(parent_idx);
            seL4_SetMR(0, (seL4_Word)child_pid);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));

            /* Reap child when it exits (wait on its fault EP) */
            if (child_pid > 0) {
                /* Find child's active_proc entry */
                for (int ci = 0; ci < MAX_ACTIVE_PROCS; ci++) {
                    if (active_procs[ci].active && active_procs[ci].pid == child_pid) {
                        /* Non-blocking check — child may still be running */
                        /* We'll let the normal exec cleanup handle it */
                        break;
                    }
                }
            }
            break;
        }
        case PIPE_EXEC: {
            int ci = (int)badge - 1;
            if (ci < 0 || ci >= MAX_ACTIVE_PROCS || !active_procs[ci].active) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            /* Unpack double-null terminated buffer from MRs:
             * path\0argv[0]\0argv[1]\0...\0\0 */
            int nmrs = seL4_MessageInfo_get_length(msg);
            char exec_buf[900];
            int ebi = 0;
            for (int m = 0; m < nmrs && ebi < 899; m++) {
                seL4_Word w = seL4_GetMR(m);
                for (int b = 0; b < 8 && ebi < 899; b++) {
                    exec_buf[ebi++] = (char)((w >> (b * 8)) & 0xFF);
                }
            }
            exec_buf[ebi] = 0;

            /* First string is the path (already resolved by caller) */
            char *elf_path = exec_buf;

            /* Collect argv strings after the path */
            #define MAX_USER_ARGV 16
            char *exec_argv[MAX_USER_ARGV];
            int exec_argc = 0;
            {
                /* Skip past path null */
                int pos = 0;
                while (pos < ebi && exec_buf[pos] != 0) pos++;
                pos++; /* skip null after path */
                /* Each subsequent null-terminated string is an argv entry */
                while (pos < ebi && exec_buf[pos] != 0 && exec_argc < MAX_USER_ARGV) {
                    exec_argv[exec_argc++] = &exec_buf[pos];
                    while (pos < ebi && exec_buf[pos] != 0) pos++;
                    pos++; /* skip null */
                }
            }

            /* Save old metadata */
            active_proc_t *ap = &active_procs[ci];
            int old_pid = ap->pid;
            int old_ppid = ap->ppid;
            uint32_t old_uid = ap->uid;
            uint32_t old_gid = ap->gid;
            vka_object_t old_fault_ep = ap->fault_ep;

            /* Read + parse ELF (reuses file-scope elf_buf) */
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

            /* Destroy old process */
            sel4utils_destroy_process(&ap->proc, &vka);

            /* Configure new process */
            sel4utils_process_config_t cfg = process_config_new(&simple);
            cfg = process_config_create_cnode(cfg, 12);
            cfg = process_config_create_vspace(cfg, NULL, 0);
            cfg = process_config_priority(cfg, 200);
            cfg = process_config_auth(cfg, simple_get_tcb(&simple));
            cfg = process_config_fault_endpoint(cfg, old_fault_ep);

            sel4utils_process_t *proc = &ap->proc;
            int err = sel4utils_configure_process_custom(proc, &vka, &vspace, cfg);
            if (err) {
                ap->active = 0;
                proc_remove(old_pid);
                break;
            }

            /* Record segments */
            ap->num_segs = 0;
            for (int si = 0; si < elf_getNumProgramHeaders(&elf) && ap->num_segs < MAX_ELF_SEGS; si++) {
                if (elf_getProgramHeaderType(&elf, si) == 1) {
                    elf_seg_info_t *seg = &ap->segs[ap->num_segs++];
                    seg->vaddr = (uintptr_t)elf_getProgramHeaderVaddr(&elf, si);
                    seg->memsz = (size_t)elf_getProgramHeaderMemorySize(&elf, si);
                    seg->flags = (uint32_t)elf_getProgramHeaderFlags(&elf, si);
                }
            }

            /* Load ELF */
            proc->entry_point = sel4utils_elf_load(&proc->vspace, &vspace, &vka, &vka, &elf);
            if (!proc->entry_point) {
                sel4utils_destroy_process(proc, &vka);
                ap->active = 0;
                proc_remove(old_pid);
                break;
            }
            proc->sysinfo = sel4utils_elf_get_vsyscall(&elf);
            proc->num_elf_phdrs = sel4utils_elf_num_phdrs(&elf);
            proc->elf_phdrs = calloc(proc->num_elf_phdrs, sizeof(Elf_Phdr));
            if (proc->elf_phdrs) {
                sel4utils_elf_read_phdrs(&elf, proc->num_elf_phdrs, proc->elf_phdrs);
                for (int i = 0; i < proc->num_elf_phdrs; i++)
                    if (proc->elf_phdrs[i].p_type == PT_PHDR)
                        proc->elf_phdrs[i].p_type = PT_NULL;
            }
            proc->pagesz = PAGE_SIZE_4K;

            /* Copy caps */
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

            /* Build argv + spawn */
            char ss[16], sf[16], st[16], sa[16], sp[16], cwd[64];
            snprintf(ss, 16, "%lu", (unsigned long)cs);
            snprintf(sf, 16, "%lu", (unsigned long)cf);
            snprintf(st, 16, "%lu", (unsigned long)ct);
            snprintf(sa, 16, "%lu", (unsigned long)ca);
            snprintf(sp, 16, "%lu", (unsigned long)cp2);
            snprintf(cwd, 64, "%u:%u:/", old_uid, old_gid);
            /* Build spawn argv: [caps(5), cwd, user_argv[0], user_argv[1], ...] */
            char *spawn_argv[6 + MAX_USER_ARGV];
            int spawn_argc = 0;
            spawn_argv[spawn_argc++] = ss;
            spawn_argv[spawn_argc++] = sf;
            spawn_argv[spawn_argc++] = st;
            spawn_argv[spawn_argc++] = sa;
            spawn_argv[spawn_argc++] = sp;
            spawn_argv[spawn_argc++] = cwd;
            /* If user provided argv, use it; otherwise fall back to elf_path */
            if (exec_argc > 0) {
                for (int ai = 0; ai < exec_argc && spawn_argc < 6 + MAX_USER_ARGV; ai++)
                    spawn_argv[spawn_argc++] = exec_argv[ai];
            } else {
                spawn_argv[spawn_argc++] = elf_path;
            }
            err = sel4utils_spawn_process_v(proc, &vka, &vspace, spawn_argc, spawn_argv, 1);
            if (err) {
                sel4utils_destroy_process(proc, &vka);
                ap->active = 0;
                proc_remove(old_pid);
                break;
            }

            /* Restore metadata */
            ap->active = 1;
            ap->pid = old_pid;
            ap->ppid = old_ppid;
            ap->uid = old_uid;
            ap->gid = old_gid;
            ap->fault_ep = old_fault_ep;
            ap->exit_status = 0;
            ap->num_threads = 0;
            for (int ti = 0; ti < MAX_THREADS_PER_PROC; ti++)
                ap->threads[ti].active = 0;

            /* Update proc name */
            for (int pi = 0; pi < PROC_MAX; pi++) {
                if (proc_table[pi].active && proc_table[pi].pid == old_pid) {
                    int ni = 0;
                    const char *n = elf_path;
                    while (*n && ni < 63) proc_table[pi].name[ni++] = *n++;
                    proc_table[pi].name[ni] = 0;
                    break;
                }
            }
            /* No reply — old process destroyed, new one running */
            break;
        }
        default:
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
    }
}
