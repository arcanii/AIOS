/* AIOS servers/exec_server.c -- ELF loader and exec server
 *
 * Handles EXEC_RUN, EXEC_RUN_BG, EXEC_NICE IPC requests.
 * Reads ELF from disk via VFS, loads into new VSpace, spawns process.
 * Also contains process_kill for PIPE_KILL.
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


/* ── Process kill ── */
int process_kill(int pid) {
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
        if (active_procs[i].active && active_procs[i].pid == pid) {
            /* Destroy the process */
            sel4utils_destroy_process(&active_procs[i].proc, &vka);
            vka_free_object(&vka, &active_procs[i].fault_ep);
            active_procs[i].active = 0;
            /* Update proc table */
            proc_remove(pid);
            return 0;
        }
    }
    return -1;  /* not found */
}

/* Exec thread — spawns processes on behalf of shell */
static vka_object_t exec_reply_cap_obj;

void exec_thread_fn(void *arg0, void *arg1, void *ipc_buf) {
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

        if (label != EXEC_RUN && label != EXEC_NICE && label != EXEC_RUN_BG) {
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
        /* elf_buf is at file scope */
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

        /* Record ELF segments for fork */
        {
            int nph = elf_getNumProgramHeaders(&elf);
            ap->num_segs = 0;
            for (int si = 0; si < nph && ap->num_segs < MAX_ELF_SEGS; si++) {
                if (elf_getProgramHeaderType(&elf, si) == 1 /* PT_LOAD */) {
                    elf_seg_info_t *seg = &ap->segs[ap->num_segs++];
                    seg->vaddr = (uintptr_t)elf_getProgramHeaderVaddr(&elf, si);
                    seg->memsz = (size_t)elf_getProgramHeaderMemorySize(&elf, si);
                    seg->flags = (uint32_t)elf_getProgramHeaderFlags(&elf, si);
                }
            }
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
        ap->child_ser_slot = child_ser;
        /* Mint badged fs_ep (badge = ap_idx + 1) for permission checks */
        cspacepath_t fs_src, fs_dest;
        vka_cspace_make_path(&vka, fs_ep_cap, &fs_src);
        vka_cspace_alloc_path(&vka, &fs_dest);
        seL4_CNode_Mint(fs_dest.root, fs_dest.capPtr, fs_dest.capDepth,
            fs_src.root, fs_src.capPtr, fs_src.capDepth,
            seL4_AllRights, (seL4_Word)(ap_idx + 1));
        seL4_CPtr child_fs = sel4utils_copy_cap_to_process(
            proc, &vka, fs_dest.capPtr);
        ap->child_fs_slot = child_fs;

        /* Mint badged thread_ep (badge = ap_idx + 1) and copy to child */
        cspacepath_t te_src, te_dest;
        vka_cspace_make_path(&vka, thread_ep_cap, &te_src);
        vka_cspace_alloc_path(&vka, &te_dest);
        seL4_CNode_Mint(te_dest.root, te_dest.capPtr, te_dest.capDepth,
            te_src.root, te_src.capPtr, te_src.capDepth,
            seL4_AllRights, (seL4_Word)(ap_idx + 1));
        seL4_CPtr child_thread = sel4utils_copy_cap_to_process(
            proc, &vka, te_dest.capPtr);
        ap->child_thread_slot = child_thread;

        seL4_CPtr child_auth = sel4utils_copy_cap_to_process(
            proc, &vka, auth_ep_cap);
        ap->child_auth_slot = child_auth;
        /* Mint badged pipe_ep (badge = ap_idx + 1) for fork identification */
        cspacepath_t pip_src, pip_dest;
        vka_cspace_make_path(&vka, pipe_ep_cap, &pip_src);
        vka_cspace_alloc_path(&vka, &pip_dest);
        seL4_CNode_Mint(pip_dest.root, pip_dest.capPtr, pip_dest.capDepth,
            pip_src.root, pip_src.capPtr, pip_src.capDepth,
            seL4_AllRights, (seL4_Word)(ap_idx + 1));
        seL4_CPtr child_pipe = sel4utils_copy_cap_to_process(
            proc, &vka, pip_dest.capPtr);
        ap->child_pipe_slot = child_pipe;

        char s_ser[16], s_fs[16], s_thread[16], s_auth[16], s_pipe[16];
        snprintf(s_ser, 16, "%lu", (unsigned long)child_ser);
        snprintf(s_fs, 16, "%lu", (unsigned long)child_fs);
        snprintf(s_thread, 16, "%lu", (unsigned long)child_thread);
        snprintf(s_auth, 16, "%lu", (unsigned long)child_auth);
        snprintf(s_pipe, 16, "%lu", (unsigned long)child_pipe);

        /* Build argv array */
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
        ap->fault_on_pipe_ep = 0;
        ap->stdout_pipe_id = -1;
        ap->stdin_pipe_id = -1;
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

        /* Background exec: reply immediately, don't wait */
        if (label == EXEC_RUN_BG) {
            ap->pid = child_pid;
            seL4_SetMR(0, (seL4_Word)child_pid);
            seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
            /* Store fault_ep in active_procs for later reaping */
            ap->fault_ep = child_fault_ep;
            continue;
        }

        /* Track foreground process for Ctrl-C */
        fg_pid = child_pid;
        fg_fault_ep = child_fault_ep.cptr;

        /* Wait for child to exit */
        seL4_Word child_badge;
        seL4_Recv(child_fault_ep.cptr, &child_badge);

        /* Clear foreground tracking */
        fg_pid = -1;
        fg_fault_ep = 0;

        /* Check if child was killed by Ctrl-C */
        int was_killed = fg_killed;
        fg_killed = 0;

        if (was_killed) {
            /* Ctrl-C killed child — process already destroyed, just clean up metadata */
            vka_free_object(&vka, &child_fault_ep);
            seL4_CNode_Delete(seL4_CapInitThreadCNode, te_dest.capPtr, seL4_WordBits);
            vka_cspace_free(&vka, te_dest.capPtr);
            seL4_CNode_Delete(seL4_CapInitThreadCNode, fs_dest.capPtr, seL4_WordBits);
            vka_cspace_free(&vka, fs_dest.capPtr);
            ap->active = 0;
            if (child_pid > 0) proc_remove(child_pid);
        } else {
            /* Normal exit — full cleanup */
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
            seL4_CNode_Delete(seL4_CapInitThreadCNode, te_dest.capPtr, seL4_WordBits);
            vka_cspace_free(&vka, te_dest.capPtr);
            seL4_CNode_Delete(seL4_CapInitThreadCNode, fs_dest.capPtr, seL4_WordBits);
            vka_cspace_free(&vka, fs_dest.capPtr);
            ap->active = 0;
            if (child_pid > 0) proc_remove(child_pid);
        }

        /* Reply to shell via saved cap */
        seL4_SetMR(0, 0);
        seL4_Send(reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
    }
}

/* Read a 512-byte sector via virtio-blk */
