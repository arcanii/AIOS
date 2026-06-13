/*
 * boot_services.c -- Server thread startup and process spawning
 *
 * Extracted from aios_root.c (v0.4.53 modularization).
 * Creates IPC endpoints, starts internal server threads,
 * and spawns tty_server, auth_server, and getty.
 */
#include "aios/root_shared.h"
#include "aios/vka_audit.h"
#include "aios/vfs.h"
#include "aios/config.h"
#include "plat/net_hal.h"
#include "aios/procfs.h"
#include <sel4utils/thread.h>
#define LOG_MODULE "boot"
#define LOG_LEVEL LOG_LEVEL_DEBUG
#include "aios/aios_log.h"
#include <stdio.h>

/* v0.4.178: pin every root-task thread to core 0.
 * The root servers (pipe/net/fs/exec/thread/crypto/display) plus do_fork all
 * share ONE global allocman/vka (aios_root.c) with NO lock. On an SMP kernel
 * (QEMU KernelMaxNumNodes=4) they run on different cores concurrently, so the
 * lock-free CSpace-slot bitmap RMW in libsel4allocman tears: do_fork's ~49
 * cap-slot ops race net_server's SaveCaller, two callers get the SAME root
 * slot, and a saved reply cap (or minted cap) is silently clobbered -> a live
 * server's next IPC returns garbage. That was the QEMU "second SSH connection
 * fails (key exchange failed)" bug -- sshd stays alive but its server IPC
 * breaks. Pinning all root threads to core 0 serializes every allocator access
 * (the servers are IPC handlers, mostly blocked on Recv, so one core is fine;
 * user processes still use the other cores). RPi4 is uniprocessor
 * (KernelMaxNumNodes=1) so it never hit this -- consistent with the bug being
 * QEMU-only. A finer fix would be a lock around the allocator; pinning is the
 * smallest correct change. */
#define ROOT_CORE 0

/* Start an internal server thread at priority 200, pinned to ROOT_CORE */
static int start_server_thread(sel4utils_thread_entry_fn fn,
                               seL4_CPtr ep_cap) {
    sel4utils_thread_t thread;
    int error = sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
        simple_get_cnode(&simple), seL4_NilData, &thread);
    if (error) return error;
    seL4_TCB_SetPriority(thread.tcb.cptr, simple_get_tcb(&simple), 200);
    #if CONFIG_MAX_NUM_NODES > 1
    seL4_TCB_SetAffinity(thread.tcb.cptr, ROOT_CORE);
    #endif
    return sel4utils_start_thread(&thread, fn,
        (void *)(uintptr_t)ep_cap, NULL, 1);
}

void boot_start_services(vka_object_t *fault_ep) {
    int error;

    /* v0.4.178: pin the root task's own init thread to core 0 too -- it shares
     * the same global allocman/vka as the servers (see start_server_thread). */
    #if CONFIG_MAX_NUM_NODES > 1
    seL4_TCB_SetAffinity(simple_get_tcb(&simple), ROOT_CORE);
    #endif

    /* v0.4.80: load configuration from /etc/ before starting servers */
    boot_load_config();

    /* Create IPC endpoints for internal servers */
    vka_object_t fs_ep_obj;
    vka_audit_endpoint(VKA_SUB_BOOT);
    vka_alloc_endpoint(&vka, &fs_ep_obj);
    fs_ep_cap = fs_ep_obj.cptr;

    vka_object_t exec_ep_obj;
    vka_audit_endpoint(VKA_SUB_BOOT);
    vka_alloc_endpoint(&vka, &exec_ep_obj);
    seL4_CPtr exec_ep_cap = exec_ep_obj.cptr;

    vka_object_t thread_ep_obj;
    vka_audit_endpoint(VKA_SUB_BOOT);
    vka_alloc_endpoint(&vka, &thread_ep_obj);
    thread_ep_cap = thread_ep_obj.cptr;

    vka_object_t auth_ep_obj;
    vka_audit_endpoint(VKA_SUB_BOOT);
    vka_alloc_endpoint(&vka, &auth_ep_obj);
    auth_ep_cap = auth_ep_obj.cptr;

    vka_object_t pipe_ep_obj;
    vka_audit_endpoint(VKA_SUB_BOOT);
    vka_alloc_endpoint(&vka, &pipe_ep_obj);
    pipe_ep_cap = pipe_ep_obj.cptr;

    /* Start internal server threads */
    start_server_thread((sel4utils_thread_entry_fn)fs_thread_fn, fs_ep_cap);
    start_server_thread((sel4utils_thread_entry_fn)exec_thread_fn, exec_ep_cap);
    LOG_INFO("Thread server started");
    start_server_thread((sel4utils_thread_entry_fn)thread_server_fn, thread_ep_cap);
    start_server_thread((sel4utils_thread_entry_fn)pipe_server_fn, pipe_ep_cap);

    /* v0.4.219: TLBI keepalive (tlbi_probe.c). Steady unmap/map traffic keeps
     * the TLB/DVM path exercised -- empirically suppresses the BCM2711 stall
     * family alongside the no-WFI idle; cost is ~25 unmaps/s on core 0. */
    { extern void tlbi_probe_start(void); tlbi_probe_start(); }

    proc_add("pipe_server", 200);

    /* USB HID keyboard driver thread (if xhci_init enumerated a keyboard).
     * Polls the xHCI event ring for interrupt-transfer completions. */
    {
        extern int xhci_kbd_ok;
        extern void xhci_kbd_driver_fn(void *, void *, void *);
        if (xhci_kbd_ok)
            start_server_thread((sel4utils_thread_entry_fn)xhci_kbd_driver_fn, 0);
    }

    /* Start network threads (if virtio-net detected) */
    if (net_available) {
        vka_object_t net_ep_obj;
        vka_audit_endpoint(VKA_SUB_BOOT);
        vka_alloc_endpoint(&vka, &net_ep_obj);
        net_ep_cap = net_ep_obj.cptr;

        /* Server thread: receives the net_ep as arg0; the RX IRQ notification
         * (net_drv_ntfn) is bound to its TCB so its blocking seL4_Recv wakes on
         * BOTH client IPC and packet arrival -- the bound-notification trick.
         * v0.4.230 (netd Stage 1): the dedicated driver thread is gone;
         * net_server_fn drains the HW ring itself via plat_net_drain(). */
        {
            sel4utils_thread_t net_srv;
            error = sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
                simple_get_cnode(&simple), seL4_NilData, &net_srv);
            if (!error) {
                seL4_TCB_SetPriority(net_srv.tcb.cptr,
                    simple_get_tcb(&simple), 200);
                #if CONFIG_MAX_NUM_NODES > 1
                seL4_TCB_SetAffinity(net_srv.tcb.cptr, ROOT_CORE);
                #endif
                int bind_err = seL4_TCB_BindNotification(
                    net_srv.tcb.cptr, net_drv_ntfn_cap);
                AIOS_LOG_INFO_V("net srv ntfn bind err=", bind_err);
                sel4utils_start_thread(&net_srv,
                    (sel4utils_thread_entry_fn)net_server_fn,
                    (void *)(uintptr_t)net_ep_cap, NULL, 1);
            }
        }
        proc_add("net_server", 200);

        /* v0.4.229 (netd Stage 0): sacrificial net-socket cleanup proxy. Drains
         * pids that handle_child_fault enqueues on child exit and issues the
         * blocking NET_CLEANUP_PID Call, so a slow/wedged net server parks only
         * this thread, never the pipe_server loop. Its wakeup notification must
         * be live before getty (the first faultable child) spawns -- set it
         * here, well before the getty spawn at the end of boot. */
        {
            vka_object_t cleanup_ntfn_obj;
            if (!vka_alloc_notification(&vka, &cleanup_ntfn_obj)) {
                pipe_set_net_cleanup_ntfn(cleanup_ntfn_obj.cptr);
                start_server_thread(
                    (sel4utils_thread_entry_fn)net_cleanup_proxy_fn,
                    cleanup_ntfn_obj.cptr);
                proc_add("net_cleanup", 200);
            } else {
                AIOS_LOG_WARN("net cleanup proxy ntfn alloc failed");
            }
        }
        AIOS_LOG_INFO("Network threads started");
    }

    /* Start display server */
    {
        vka_object_t disp_ep_obj;
        vka_audit_endpoint(VKA_SUB_BOOT);
        vka_alloc_endpoint(&vka, &disp_ep_obj);
        disp_ep_cap = disp_ep_obj.cptr;
        start_server_thread((sel4utils_thread_entry_fn)display_server_fn,
                            disp_ep_cap);
        proc_add("display_server", 200);
    }

    /* Start crypto server (CSPRNG, ChaCha20-based /dev/urandom) */
    {
        vka_object_t crypto_ep_obj;
        vka_audit_endpoint(VKA_SUB_BOOT);
        vka_alloc_endpoint(&vka, &crypto_ep_obj);
        crypto_ep_cap = crypto_ep_obj.cptr;
        start_server_thread(
            (sel4utils_thread_entry_fn)crypto_server_main,
            crypto_ep_cap);
        proc_add("crypto_server", 200);
        AIOS_LOG_INFO("Crypto server started");
    }

    /* v0.4.121: server health probe -- pings each in-process server every
     * few seconds and tracks last-ok / latency. Surfaced via /proc/serverstats. */
    serverstats_init();
    proc_add("serverstats", 180);

    /* v0.4.188: periodic write-back flusher -- bounds the data-loss window of
     * the drive-0 write-back cache by issuing FS_SYNC every 30s. */
    flush_server_init();
    proc_add("flush", 200);

#ifdef AIOS_NETD
    /* netd de-monolithization (DESIGN_NETD Stage 2): spawn the MMU-isolated netd
     * skeleton and drive its spawn / fault-listener / reply-sweep selftests. The
     * skeleton runs on its OWN test endpoint -- the real net stack still serves
     * net_ep in the root task (the Stage 3 cutover is a separate change) -- so
     * this is purely additive and touches no net globals. Placed before the
     * tty/auth/getty spawn so the skeleton's crash-containment fault is taken
     * mid-boot, demonstrating the system still comes all the way up to login. */
    {
        extern void spawn_netd(void);
        spawn_netd();
    }
#endif

    /* Spawn tty_server (CPIO, isolated process). Pass the display_server endpoint as
     * a 2nd cap (argv[1]) so tty output mirrors to the HDMI fb_console -- the shell is
     * then visible on HDMI for standalone USB-keyboard use. Omitted if no display. */
    sel4utils_process_t serial_proc;
    seL4_CPtr caps[2], slots[2];
    caps[0] = serial_ep.cptr;
    int tty_ncaps = 1;
    if (disp_ep_cap && gpu_available) { caps[1] = disp_ep_cap; tty_ncaps = 2; }
    error = spawn_with_args("tty_server", 200, &serial_proc,
                            fault_ep, tty_ncaps, caps, slots);
    if (error) { AIOS_LOG_ERROR_V("tty_server spawn FAILED err=", error); return; }
    proc_add("tty_server", 200);

    /* Spawn auth_server (CPIO, isolated process) */
    sel4utils_process_t auth_proc;
    seL4_CPtr auth_caps[2] = { serial_ep.cptr, auth_ep_cap };
    seL4_CPtr auth_slots[2];
    error = spawn_with_args("auth_server", 200, &auth_proc,
                            fault_ep, 2, auth_caps, auth_slots);
    if (error) { AIOS_LOG_ERROR_V("auth_server spawn FAILED err=", error); return; }
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
            AIOS_LOG_WARN("/etc/passwd not found, using defaults");
        }
    }
    AIOS_LOG_INFO("Auth: isolated process");

    /* Spawn getty via exec_thread (fork+exec capable VSpace).
     *
     * v0.4.102: In DEGRADED mode (no disk fs), getty cannot be loaded
     * because it lives on disk. Print a recovery message instead and
     * leave boot_services in idle. The user should reboot with a
     * working disk image. */
    if (aios_boot_status != BOOT_FS_OK) {
        AIOS_LOG_ERROR("Cannot spawn getty: filesystem unavailable");
        printf("\n");
        printf("============================================\n");
        printf("  AIOS RECOVERY MODE\n");
        printf("============================================\n");
        printf("  Boot status: %s\n",
               aios_boot_status == BOOT_FS_DEGRADED ? "DEGRADED" : "FATAL");
        printf("  No disk filesystem available.\n");
        printf("  Check /proc/log via dmesg-equivalent below.\n");
        printf("  Reboot with a valid disk image to recover.\n");
        printf("============================================\n");
        return;
    }

    /* v0.4.114: pre-load the most common files into the block cache
     * before showing the login prompt. Drops first-use disk latency. */
    extern void boot_warmup_prefetch(void);
    boot_warmup_prefetch();

    {
        const char *sh_cmd = "/bin/aios/getty CWD=0:0:/";
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
