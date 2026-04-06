/*
 * boot_services.c -- Server thread startup and process spawning
 *
 * Extracted from aios_root.c (v0.4.53 modularization).
 * Creates IPC endpoints, starts internal server threads,
 * and spawns tty_server, auth_server, and getty.
 */
#include "aios/root_shared.h"
#include "aios/vfs.h"
#include "aios/procfs.h"
#include <sel4utils/thread.h>
#define LOG_MODULE "boot"
#define LOG_LEVEL LOG_LEVEL_DEBUG
#include "aios/aios_log.h"
#include <stdio.h>

/* Start an internal server thread at priority 200 */
static int start_server_thread(sel4utils_thread_entry_fn fn,
                               seL4_CPtr ep_cap) {
    sel4utils_thread_t thread;
    int error = sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
        simple_get_cnode(&simple), seL4_NilData, &thread);
    if (error) return error;
    seL4_TCB_SetPriority(thread.tcb.cptr, simple_get_tcb(&simple), 200);
    return sel4utils_start_thread(&thread, fn,
        (void *)(uintptr_t)ep_cap, NULL, 1);
}

void boot_start_services(vka_object_t *fault_ep) {
    int error;

    /* Create IPC endpoints for internal servers */
    vka_object_t fs_ep_obj;
    vka_alloc_endpoint(&vka, &fs_ep_obj);
    fs_ep_cap = fs_ep_obj.cptr;

    vka_object_t exec_ep_obj;
    vka_alloc_endpoint(&vka, &exec_ep_obj);
    seL4_CPtr exec_ep_cap = exec_ep_obj.cptr;

    vka_object_t thread_ep_obj;
    vka_alloc_endpoint(&vka, &thread_ep_obj);
    thread_ep_cap = thread_ep_obj.cptr;

    vka_object_t auth_ep_obj;
    vka_alloc_endpoint(&vka, &auth_ep_obj);
    auth_ep_cap = auth_ep_obj.cptr;

    vka_object_t pipe_ep_obj;
    vka_alloc_endpoint(&vka, &pipe_ep_obj);
    pipe_ep_cap = pipe_ep_obj.cptr;

    /* Start internal server threads */
    start_server_thread((sel4utils_thread_entry_fn)fs_thread_fn, fs_ep_cap);
    start_server_thread((sel4utils_thread_entry_fn)exec_thread_fn, exec_ep_cap);
    LOG_INFO("Thread server started");
    start_server_thread((sel4utils_thread_entry_fn)thread_server_fn, thread_ep_cap);
    start_server_thread((sel4utils_thread_entry_fn)pipe_server_fn, pipe_ep_cap);
    proc_add("pipe_server", 200);

    /* Spawn tty_server (CPIO, isolated process) */
    sel4utils_process_t serial_proc;
    seL4_CPtr caps[1], slots[1];
    caps[0] = serial_ep.cptr;
    error = spawn_with_args("tty_server", 200, &serial_proc,
                            fault_ep, 1, caps, slots);
    if (error) { printf("[proc] tty FAILED\n"); return; }
    proc_add("tty_server", 200);

    /* Spawn auth_server (CPIO, isolated process) */
    sel4utils_process_t auth_proc;
    seL4_CPtr auth_caps[2] = { serial_ep.cptr, auth_ep_cap };
    seL4_CPtr auth_slots[2];
    error = spawn_with_args("auth_server", 200, &auth_proc,
                            fault_ep, 2, auth_caps, auth_slots);
    if (error) { printf("[proc] auth FAILED\n"); return; }
    proc_add("auth_server", 200);

    /* Send /etc/passwd to auth_server via IPC */
    {
        static char pw_buf[4096];
        int pw_len = vfs_read("/etc/passwd", pw_buf, sizeof(pw_buf) - 1);
        if (pw_len > 0) {
            pw_buf[pw_len] = 0;
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

    /* Spawn getty via exec_thread (fork+exec capable VSpace) */
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
}
