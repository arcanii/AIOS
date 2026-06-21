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
#include "aios/cpuacct.h"
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

/* Start an internal server thread at priority 200, pinned to ROOT_CORE. name is
 * registered for /proc/cpuacct (per-thread CPU accounting). */
static int start_server_thread(const char *name, sel4utils_thread_entry_fn fn,
                               seL4_CPtr ep_cap) {
    sel4utils_thread_t thread;
    int error = sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
        simple_get_cnode(&simple), seL4_NilData, &thread);
    if (error) return error;
    seL4_TCB_SetPriority(thread.tcb.cptr, simple_get_tcb(&simple), 200);
    #if CONFIG_MAX_NUM_NODES > 1
    seL4_TCB_SetAffinity(thread.tcb.cptr, ROOT_CORE);
    #endif
    int rc = sel4utils_start_thread(&thread, fn,
        (void *)(uintptr_t)ep_cap, NULL, 1);
    if (rc == 0) aios_acct_register(name, thread.tcb.cptr);
    return rc;
}

/* v0.4.229 (netd Stage 0): start the sacrificial net-socket cleanup proxy. It
 * drains pids handle_child_fault enqueues on child exit and issues the blocking
 * NET_CLEANUP_PID Call to net_ep_cap, so a slow/wedged net server (in-root OR the
 * isolated netd) parks only this expendable thread, never the pipe_server loop.
 * Re-checks net_ep_cap per dequeue, so it tolerates net_ep_cap being published
 * late (netd DEVD_READY) or zeroed (a netd crash). Must be live before getty (the
 * first faultable child) spawns. */
static void start_net_cleanup_proxy(void) {
    vka_object_t cleanup_ntfn_obj;
    if (!vka_alloc_notification(&vka, &cleanup_ntfn_obj)) {
        pipe_set_net_cleanup_ntfn(cleanup_ntfn_obj.cptr);
        start_server_thread("net_cleanup",
            (sel4utils_thread_entry_fn)net_cleanup_proxy_fn,
            cleanup_ntfn_obj.cptr);
        proc_add("net_cleanup", 200);
    } else {
        AIOS_LOG_WARN("net cleanup proxy ntfn alloc failed");
    }
}

void boot_start_services(vka_object_t *fault_ep) {
    int error;

    /* v0.4.178: pin the root task's own init thread to core 0 too -- it shares
     * the same global allocman/vka as the servers (see start_server_thread). */
    #if CONFIG_MAX_NUM_NODES > 1
    seL4_TCB_SetAffinity(simple_get_tcb(&simple), ROOT_CORE);
    #endif

    /* Register the root task's own thread for /proc/cpuacct (index 0). It runs the
     * boot flow then the no-WFI idle spin (aios_root.c). */
    aios_acct_register("root", seL4_CapInitThreadTCB);

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
    start_server_thread("fs", (sel4utils_thread_entry_fn)fs_thread_fn, fs_ep_cap);
    start_server_thread("exec", (sel4utils_thread_entry_fn)exec_thread_fn, exec_ep_cap);
    LOG_INFO("Thread server started");
    start_server_thread("thread", (sel4utils_thread_entry_fn)thread_server_fn, thread_ep_cap);
    start_server_thread("pipe", (sel4utils_thread_entry_fn)pipe_server_fn, pipe_ep_cap);

    /* v0.4.259: the v0.4.216 tlbi_probe keepalive (steady unmap/map on core 0) was
     * REMOVED -- the BCM2711 32.4s TLBI/DVM stall is cured by the no-WFI idle spin
     * (settings-rpi4.cmake) + KernelMaxNumNodes=4 (cores idle-spin -> SCU stays
     * clocked) + the per-ASID residency-masked TLB shootdown (32dbc39); the probe's
     * traffic was belt-and-braces, not load-bearing. [[project_stall_hunt]]. */

    /* v0.4.257 DIAGNOSTIC (core_warm.c): 3 idle-core warmers pinned to cores 1/2/3,
     * INERT until /proc/corewarm.1 -- tests whether keeping the idle cores active cures
     * the residual remote-TLB-shootdown spawn-storm stall (project_stall_hunt). */
    { extern void core_warm_start(void); core_warm_start(); }

    /* v0.4.266 (fabric_warm.c): SCB-fabric keep-warm thread on core 1, INERT until
     * /proc/fabwarm.1 -- the "Linux approach" to the ~33s idle-teardown DVM freeze
     * (light uncached fabric reads keep the cluster's SCB AXI/snoop link from
     * quiescing, so core 0's post-idle teardown DVM-Sync completes; project_stall_hunt). */
    { extern void fabric_warm_start(void); fabric_warm_start(); }
    /* NOTE: dma_warm_init() (session-8 DRAM keep-warm) now runs EARLY in aios_root.c, right after
     * prealloc_rpi4_devices -- it must grab its sub-1GB scratch region BEFORE boot_net_init/v3d_init
     * exhaust the low untypeds (a late call here failed with "no <1GB region"). */

    /* v0.4.271 (watchdog.c): MVD-1 -- a core-0 liveness heartbeat + a core-1
     * out-of-band watchdog that SURVIVES the ~32.4s teardown freeze (core 1's timer
     * IRQ is masked in the kernel so it never blocks on the BKL) and reports it live
     * over the mini-UART while core 0 is wedged. Default-OFF; /proc/watchdog.1 enables. */
    { extern void watchdog_start(void); watchdog_start(); }

    proc_add("pipe_server", 200);

    /* USB driver thread -- spawn whenever the xHCI controller is up (v0.4.255 Path A), not
     * just when a keyboard enumerated at boot. It polls the event ring for interrupt-transfer
     * completions AND drains Port Status Change Events, so a device HOTPLUGGED after boot (the
     * only workable model for a USB drive -- the RPi4 firmware hangs on an ext2 drive present
     * at boot) is detected, enumerated, and (for mass storage) mounted at runtime. */
    {
        extern int xhci_running;
        extern void xhci_kbd_driver_fn(void *, void *, void *);
        if (xhci_running)
            start_server_thread("xhci", (sel4utils_thread_entry_fn)xhci_kbd_driver_fn, 0);
    }

    /* Start the network path. DESIGN_NETD Stage 3: under AIOS_NETD the net stack
     * runs in the isolated netd process; root owns net_ep (so the client ABI is
     * unchanged) and hands a copy to netd. Flag-OFF keeps the in-root net_server
     * thread. Root compiles BOTH so a rollback is a reconfigure, not a code edit. */
#ifdef AIOS_NETD
    if (net_hw_present) {
        /* Root allocates net_ep (the same endpoint object clients already hold);
         * spawn_netd donates a copy to netd, waits for DEVD_READY, then PUBLISHES
         * net_ep_cap = this object + net_available (s8). net_ep_cap stays 0 until
         * READY, so children spawned meanwhile get child_net=0 -> -ENOTSUP. */
        vka_object_t net_ep_obj;
        vka_audit_endpoint(VKA_SUB_BOOT);
        vka_alloc_endpoint(&vka, &net_ep_obj);
        spawn_netd(net_ep_obj.cptr);
        start_net_cleanup_proxy();
        AIOS_LOG_INFO("netd network path started");
    }
#else
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
                aios_acct_register("net", net_srv.tcb.cptr);
            }
        }
        proc_add("net_server", 200);
        start_net_cleanup_proxy();
        AIOS_LOG_INFO("Network threads started");
    }
#endif

    /* Start display server. Configured manually (not via start_server_thread) so we
     * can bind a notification to its TCB: the V3D kick (/proc/v3d.test) runs on this
     * thread, posted by the fs thread via a non-blocking seL4_Signal on disp_wake_ntfn
     * (a Call would clobber the fs reply cap on non-MCS). The bound notification makes
     * its blocking seL4_Recv wake on the signal -- the net_server pattern. */
    {
        vka_object_t disp_ep_obj;
        vka_audit_endpoint(VKA_SUB_BOOT);
        vka_alloc_endpoint(&vka, &disp_ep_obj);
        disp_ep_cap = disp_ep_obj.cptr;

        /* wake notification: bind the BASE cap to the TCB; the fs thread signals a
         * BADGED mint so the Recv-side badge is nonzero (net_drv/net_kick split). */
        vka_object_t disp_wake_obj;
        seL4_CPtr disp_wake_base = 0;
        if (!vka_alloc_notification(&vka, &disp_wake_obj)) {
            disp_wake_base = disp_wake_obj.cptr;
            cspacepath_t src, dst;
            vka_cspace_make_path(&vka, disp_wake_base, &src);
            if (!vka_cspace_alloc_path(&vka, &dst) &&
                !seL4_CNode_Mint(dst.root, dst.capPtr, dst.capDepth,
                                 src.root, src.capPtr, src.capDepth,
                                 seL4_AllRights, 0x1D /* nonzero wake badge */)) {
                disp_wake_ntfn_cap = dst.capPtr;
            } else {
                AIOS_LOG_WARN("display wake ntfn mint failed (.test wake disabled)");
            }
        } else {
            AIOS_LOG_WARN("display wake ntfn alloc failed (.test wake disabled)");
        }

        sel4utils_thread_t disp_srv;
        int derr = sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
            simple_get_cnode(&simple), seL4_NilData, &disp_srv);
        if (!derr) {
            seL4_TCB_SetPriority(disp_srv.tcb.cptr, simple_get_tcb(&simple), 200);
            #if CONFIG_MAX_NUM_NODES > 1
            seL4_TCB_SetAffinity(disp_srv.tcb.cptr, ROOT_CORE);
            #endif
            if (disp_wake_base) {
                int berr = seL4_TCB_BindNotification(disp_srv.tcb.cptr, disp_wake_base);
                if (berr) {
                    /* bind failed -- clear the cap so v3d_test_post skips the Signal
                     * (an unbound notification would never wake the Recv). */
                    AIOS_LOG_WARN_V("display wake ntfn bind err=", berr);
                    disp_wake_ntfn_cap = 0;
                }
            }
            sel4utils_start_thread(&disp_srv,
                (sel4utils_thread_entry_fn)display_server_fn,
                (void *)(uintptr_t)disp_ep_cap, NULL, 1);
            aios_acct_register("display", disp_srv.tcb.cptr);
        } else {
            AIOS_LOG_ERROR_V("display thread config failed err=", derr);
        }
        proc_add("display_server", 200);
    }

    /* Start crypto server (CSPRNG, ChaCha20-based /dev/urandom) */
    {
        vka_object_t crypto_ep_obj;
        vka_audit_endpoint(VKA_SUB_BOOT);
        vka_alloc_endpoint(&vka, &crypto_ep_obj);
        crypto_ep_cap = crypto_ep_obj.cptr;
        start_server_thread("crypto",
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
