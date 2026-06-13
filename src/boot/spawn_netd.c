/*
 * spawn_netd.c -- spawn + supervise the MMU-isolated netd skeleton.
 *
 * DESIGN_NETD Stage 2. Compiled into the root task ONLY when AIOS_NETD is ON
 * (the whole file is #ifdef'd out otherwise, so flag-OFF builds are behaviourally
 * identical to today). spawn_netd() is called from boot_services.c, also under
 * #ifdef AIOS_NETD.
 *
 * What this proves, end to end, with NO net code in play:
 *   1. an isolated CPIO process spawns with its own cnode + vspace (prio 200,
 *      core 0 -- the load-bearing invariant: every root thread shares ONE
 *      lock-free allocman, so netd must not run on another core),
 *   2. caps reach it via decimal argv (main/test EP, ctrl/fault EP, ntfn),
 *   3. it self-binds the ntfn and announces DEVD_READY on the ctrl EP, which a
 *      DEDICATED root listener thread receives (it is Recv-ing BEFORE resume so
 *      an early crash is still caught),
 *   4. a deferred reply works two ways: netd parks a caller in its OWN cnode and
 *      (a) self-wakes it on a badge-2 kick, and (b) -- the kernel bet -- lets
 *      ROOT CNode_Move the saved reply cap out of netd's cnode and Send it
 *      (the crash-recovery reply-sweep, DESIGN_NETD s10),
 *   5. a netd fault is delivered to the listener (label <=6), decoded, logged,
 *      and CONTAINED: root does not reply, the faulted thread stays stopped, and
 *      the rest of the system keeps serving.
 *
 * The skeleton runs on its OWN test endpoint; the real net stack still serves
 * net_ep in the root task (the Stage 3 cutover is a separate change), so this is
 * purely additive -- nothing here touches net_available / net_ep_cap.
 */
#ifdef AIOS_NETD

#include "aios/root_shared.h"
#include "aios/netd_skel.h"
#include "aios/mono_wait.h"
#include <sel4utils/process.h>
#include <sel4utils/process_config.h>
#include <sel4utils/thread.h>
#include <vka/capops.h>
#include <stdio.h>
#define LOG_MODULE "netd"
#define LOG_LEVEL  LOG_LEVEL_DEBUG
#include "aios/aios_log.h"

/* /proc/vka live page counter (defined in vka_audit.c) -- for the spawn-cost gate. */
extern int vka_live_frames;

#define NETD_READY_MS 10000   /* bounded DEVD_READY wait ceiling (non-MCS, s8) */

/* ---- root-side state shared between spawn_netd() and its helper threads.
 * All three threads (init, listener, selftest client) are prio 200 on core 0,
 * so plain globals with no lock are safe here (the core-0 invariant). ---- */
static sel4utils_process_t netd_proc;           /* retained per DESIGN_NETD s3 */
static seL4_CPtr   netd_ctrl_ep;                /* root-side ctrl/fault EP */
static seL4_CPtr   netd_test_ep;                /* root-side main/test EP */
static seL4_CPtr   netd_kick_badge2;            /* badge=2 mint of netd's ntfn */
static cspacepath_t netd_sweep_scratch;         /* pre-allocated reply-sweep target slot */
static volatile int netd_ready;                 /* set by the listener on DEVD_READY */
static volatile int netd_failed;                /* set on DEVD_FAIL / fault / timeout */
static volatile int netd_fault_count;

/* Start a root helper thread at prio 200 pinned to core 0 (mirrors
 * boot_services.c start_server_thread; arg_cap is delivered as entry arg0). */
static int netd_start_root_thread(sel4utils_thread_entry_fn fn, seL4_CPtr arg_cap) {
    sel4utils_thread_t th;
    int err = sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
                  simple_get_cnode(&simple), seL4_NilData, &th);
    if (err) return err;
    seL4_TCB_SetPriority(th.tcb.cptr, simple_get_tcb(&simple), 200);
    #if CONFIG_MAX_NUM_NODES > 1
    seL4_TCB_SetAffinity(th.tcb.cptr, 0);   /* ROOT_CORE: shares the global vka */
    #endif
    return sel4utils_start_thread(&th, fn, (void *)(uintptr_t)arg_cap, NULL, 1);
}

/* ---- dedicated fault/ctrl listener: Recv-s netd's ctrl EP forever. It receives
 * BOTH netd status messages (DEVD_READY/FAIL/PARKED, labels >6) and kernel-delivered
 * netd faults (labels <=6) on the same endpoint, discriminated by label. ---- */
static void netd_fault_listener_fn(void *arg, void *b, void *c) {
    (void)b; (void)c;
    seL4_CPtr ctrl = (seL4_CPtr)(uintptr_t)arg;
    for (;;) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t m = seL4_Recv(ctrl, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(m);

        if (label == DEVD_READY) {
            netd_ready = 1;
            AIOS_LOG_INFO("netd: DEVD_READY received");
            continue;
        }
        if (label == DEVD_FAIL) {
            netd_failed = 1;
            AIOS_LOG_ERROR_V("netd: DEVD_FAIL errcode=", (unsigned long)seL4_GetMR(0));
            continue;
        }
        if (label == NETD_PARKED) {
            seL4_Word mode = seL4_GetMR(0);
            seL4_Word slot = seL4_GetMR(1);   /* slot in netd's cnode holding the reply */
            if (mode == NETD_PARK_KICK) {
                /* Tell netd to self-wake its parked caller via the bound ntfn.
                 * A badge-2 Signal (not an unbadged one -- that delivers badge 0
                 * and netd's wake test would drop it, DESIGN_NETD s3 row 3b). */
                if (netd_kick_badge2) seL4_Signal(netd_kick_badge2);
                AIOS_LOG_INFO("netd: PARKED(kick) -> signaled netd ntfn (badge 2)");
            } else {
                /* THE KERNEL BET (DESIGN_NETD s10). Move netd's SaveCaller'd reply
                 * cap OUT of its cnode into a root scratch slot, then Send it. If
                 * the parked caller wakes, the crash-recovery reply-sweep is
                 * viable. depth 12 = netd's cnode radix, raw slot (fork.c form). */
                int mverr = seL4_CNode_Move(
                    netd_sweep_scratch.root, netd_sweep_scratch.capPtr,
                    netd_sweep_scratch.capDepth,
                    netd_proc.cspace.cptr, slot, 12);
                printf("[netd-sweep] CNode_Move slot=%lu err=%d\n",
                       (unsigned long)slot, mverr);
                if (!mverr) {
                    seL4_SetMR(0, (seL4_Word)NETD_SWEEP_TOKEN);
                    seL4_Send(netd_sweep_scratch.capPtr,
                              seL4_MessageInfo_new(0, 0, 0, 1));
                    /* Empty the scratch slot so a later sweep can reuse it. */
                    seL4_CNode_Delete(netd_sweep_scratch.root,
                                      netd_sweep_scratch.capPtr,
                                      netd_sweep_scratch.capDepth);
                    printf("[netd-sweep] reply Send issued (token=0x%lx)\n",
                           NETD_SWEEP_TOKEN);
                }
            }
            continue;
        }
        if (label <= 6) {
            /* netd FAULT. Decode (VMFault IP/Addr for the null-deref crash),
             * log, and CONTAIN by simply not replying: the faulted thread stays
             * stopped and the system runs on. Stage 3 will additionally zero
             * net_ep_cap + clear the device IRQ here (netd is not the net
             * provider yet, so we must NOT touch net globals in Stage 2). */
            seL4_Word pc   = seL4_GetMR(seL4_VMFault_IP);
            seL4_Word addr = seL4_GetMR(seL4_VMFault_Addr);
            netd_fault_count++;
            netd_failed = 1;
            printf("[netd-listener] FAULT label=%lu pc=0x%lx addr=0x%lx -- CONTAINED\n",
                   (unsigned long)label, (unsigned long)pc, (unsigned long)addr);
            AIOS_LOG_ERROR_V("netd: FAULT contained, label=", (unsigned long)label);
            continue;
        }
        AIOS_LOG_ERROR_V("netd: ctrl unknown label=", (unsigned long)label);
    }
}

/* ---- sacrificial selftest client: drives ping -> kick -> sweep -> crash on the
 * test EP and logs each verdict. Runs in its OWN thread so that a FAILED
 * reply-sweep (the kernel bet not paying off) hangs only this thread, never
 * boot. (DESIGN_NETD s9 allows "or root" as the park driver -- this is it.) ---- */
static void netd_selftest_client_fn(void *arg, void *b, void *c) {
    (void)b; (void)c;
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg;

    /* A. liveness */
    seL4_Call(ep, seL4_MessageInfo_new(NETD_PING, 0, 0, 0));
    printf("[netd-test] PING rc=%ld\n", (long)seL4_GetMR(0));

    /* B. in-netd park + bound-ntfn self-wake (child-cnode SaveCaller+Send+Delete).
     * This Call blocks until the listener signals netd's badge-2 ntfn and netd
     * wakes us with the reply cap it parked in its OWN cnode. */
    seL4_Call(ep, seL4_MessageInfo_new(NETD_BLOCK_KICK, 0, 0, 0));
    printf("[netd-test] KICK woke rc=0x%lx (want 0x%lx)\n",
           (unsigned long)seL4_GetMR(0), NETD_KICK_TOKEN);

    /* C. THE BET: root reply-sweep. netd parks our reply cap; root CNode_Moves it
     * out of netd's cnode and Sends it. If this Call returns, the sweep is
     * viable -- the foundation of crash recovery (DESIGN_NETD s10). */
    seL4_Call(ep, seL4_MessageInfo_new(NETD_BLOCK_SWEEP, 0, 0, 0));
    printf("[netd-test] SWEEP woke rc=0x%lx (want 0x%lx) -- REPLY-SWEEP PROVEN\n",
           (unsigned long)seL4_GetMR(0), NETD_SWEEP_TOKEN);

    /* D. crash containment: netd faults, the listener logs it, the system keeps
     * serving. This Call never returns (netd is dead); that is fine, we are done.
     * If the bet in C had FAILED, we would have hung there and never reached here
     * -- the test detects that as "[netd-sweep] reply Send issued" present but
     * "[netd-test] SWEEP woke" absent. */
    printf("[netd-test] triggering NETD_CRASH (containment probe)\n");
    seL4_Call(ep, seL4_MessageInfo_new(NETD_CRASH, 0, 0, 0));
    /* not reached */
}

void spawn_netd(void) {
    int err;
    uint64_t t0 = mono_ticks();

    /* 1. Endpoints netd needs: main/test EP, ctrl/fault EP, and the ntfn it
     * self-binds. */
    vka_object_t test_ep_obj, ctrl_ep_obj, ntfn_obj;
    if (vka_alloc_endpoint(&vka, &test_ep_obj) ||
        vka_alloc_endpoint(&vka, &ctrl_ep_obj) ||
        vka_alloc_notification(&vka, &ntfn_obj)) {
        AIOS_LOG_ERROR("netd: endpoint/ntfn alloc failed -- skipping spawn");
        return;
    }
    netd_test_ep = test_ep_obj.cptr;
    netd_ctrl_ep = ctrl_ep_obj.cptr;

    /* 2. Badge=2 mint of the ntfn for the root-side kick. netd's bound-ntfn wake
     * test ignores badge 0, so only a BADGED Signal wakes it (net_virtio.c form). */
    {
        cspacepath_t ksrc, kdst;
        vka_cspace_make_path(&vka, ntfn_obj.cptr, &ksrc);
        if (!vka_cspace_alloc_path(&vka, &kdst) &&
            !seL4_CNode_Mint(kdst.root, kdst.capPtr, kdst.capDepth,
                             ksrc.root, ksrc.capPtr, ksrc.capDepth,
                             seL4_AllRights, (seL4_Word)NETD_KICK_BADGE))
            netd_kick_badge2 = kdst.capPtr;
        else
            AIOS_LOG_ERROR("netd: kick badge mint failed (kick selftest will no-op)");
    }

    /* 3. Pre-allocate the reply-sweep CNode_Move target slot ONCE (reused per
     * sweep) so the listener never allocates from the global vka at runtime. */
    if (vka_cspace_alloc_path(&vka, &netd_sweep_scratch)) {
        AIOS_LOG_ERROR("netd: sweep scratch alloc failed -- skipping spawn");
        return;
    }

    /* 4. Configure the isolated process: own 12-bit cnode, fresh vspace, CPIO ELF
     * "netd", prio 200, fault endpoint = the ctrl EP (so the listener below gets
     * netd's faults AND its DEVD_READY/FAIL/PARKED messages on one endpoint). */
    sel4utils_process_config_t config = process_config_new(&simple);
    config = process_config_elf(config, "netd", true);
    config = process_config_create_cnode(config, 12);
    config = process_config_create_vspace(config, NULL, 0);
    config = process_config_priority(config, 200);
    config = process_config_auth(config, simple_get_tcb(&simple));
    config = process_config_fault_endpoint(config, ctrl_ep_obj);
    err = sel4utils_configure_process_custom(&netd_proc, &vka, &vspace, config);
    if (err) { AIOS_LOG_ERROR_V("netd: configure failed err=", err); return; }

    /* 5. Donate caps into netd (decimal argv slots, tty_server precedent). */
    seL4_CPtr s_test = sel4utils_copy_cap_to_process(&netd_proc, &vka, test_ep_obj.cptr);
    seL4_CPtr s_ctrl = sel4utils_copy_cap_to_process(&netd_proc, &vka, ctrl_ep_obj.cptr);
    seL4_CPtr s_ntfn = sel4utils_copy_cap_to_process(&netd_proc, &vka, ntfn_obj.cptr);

    /* 6. Reserve the SaveCaller slots AFTER all donations: take the current
     * cspace cursor as the base, then bump it past the reserved range so the
     * spawn never reuses these slots. netd parks reply caps here; the listener
     * CNode_Moves them out for the reply-sweep. (DESIGN_NETD s3 row 6.) */
    seL4_Word reserved_base = netd_proc.cspace_next_free;
    netd_proc.cspace_next_free += NETD_RESERVED_SLOTS;

    /* 7. Start the fault/ctrl listener BEFORE resuming netd, so an early crash
     * (even pre-printf) is still caught and logged. */
    if (netd_start_root_thread(
            (sel4utils_thread_entry_fn)netd_fault_listener_fn, netd_ctrl_ep)) {
        AIOS_LOG_ERROR("netd: listener thread start failed -- skipping spawn");
        return;
    }

    /* 8. argv: decimal cap slots + the reserved SaveCaller base. */
    char a0[24], a1[24], a2[24], a3[24];
    snprintf(a0, sizeof a0, "%lu", (unsigned long)s_test);
    snprintf(a1, sizeof a1, "%lu", (unsigned long)s_ctrl);
    snprintf(a2, sizeof a2, "%lu", (unsigned long)s_ntfn);
    snprintf(a3, sizeof a3, "%lu", (unsigned long)reserved_base);
    char *child_argv[4] = { a0, a1, a2, a3 };
    err = sel4utils_spawn_process_v(&netd_proc, &vka, &vspace, 4, child_argv, 1);
    if (err) { AIOS_LOG_ERROR_V("netd: spawn failed err=", err); return; }

    /* Spawn-cost note (DESIGN_NETD s4: capacity is gated by the 8000-page
     * allocman pool, not RAM). We log the spawn WALL TIME but deliberately do
     * NOT report a vka_live_frames delta: that counter only tracks AIOS's own
     * audited frame allocs, while a sel4utils CPIO spawn maps the ~6MB ELF image
     * through its own vspace path, so the real ~1.6k-page cost lands in the
     * allocman pool invisibly to it (measured delta is 0, which would mislead).
     * The true capacity check is the >=30-parallel-pipeline regression, deferred
     * until the gate actually fires (s4). */
    printf("[netd] spawned in %lu cntpct ticks (audited live frames now %d)\n",
           (unsigned long)(mono_ticks() - t0), vka_live_frames);

    /* 9. Bounded wait for DEVD_READY. Non-MCS has no timed Recv, so poll the flag
     * (set by the listener) with a cntpct ceiling + Yield -- the sanctioned
     * boot-time bounded-poll exception. Degrade-and-continue on timeout/FAIL:
     * netd is OPTIONAL; a wedged init must never brick boot. */
    {
        uint64_t dl = mono_deadline_ms(NETD_READY_MS);
        while (!netd_ready && !netd_failed && mono_before(dl)) seL4_Yield();
        if (netd_ready)
            AIOS_LOG_INFO("netd: skeleton READY");
        else
            AIOS_LOG_ERROR("netd: READY timeout/fail -> degrade (boot continues)");
    }

    /* 10. Kick off the selftest client (sacrificial -- see its comment). */
    if (netd_ready)
        netd_start_root_thread(
            (sel4utils_thread_entry_fn)netd_selftest_client_fn, netd_test_ep);
}

#endif /* AIOS_NETD */
