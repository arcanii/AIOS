/*
 * spawn_netd.c -- provision + spawn + supervise the MMU-isolated netd process.
 *
 * DESIGN_NETD Stage 3 cutover (s3 cap handoff, s8 boot handshake, s10 crash
 * recovery). Compiled into the root task ONLY when AIOS_NETD is ON; the whole
 * file is compiled out otherwise, so flag-OFF builds keep the in-root net stack
 * with zero change. Two entry points, both called under #ifdef AIOS_NETD:
 *
 *   netd_prov()  -- root-side provisioning (DESIGN_NETD s7): call plat_net_prov()
 *      to allocate the DMA region, bind the IRQ, read the MAC, and retain the
 *      device-MMIO + DMA frame caps in a driver_handoff_t. Sets net_hw_present
 *      (which gates the spawn + the boot banner). Called from boot_net_init,
 *      BEFORE the boot banner, so net_hw_present is meaningful at banner time.
 *
 *   spawn_netd(net_ep) -- configure the isolated process, donate net_ep + the
 *      ctrl/fault EP + the IRQHandler + the unbadged RX notification, COPY + MAP
 *      the MMIO and DMA frame sets into the netd vspace non-cacheable, reserve the
 *      SaveCaller reply-slot range, start the fault listener, spawn, and wait
 *      (bounded) for DEVD_READY -> publish net_ep_cap + net_available. On
 *      timeout / DEVD_FAIL / early fault: degrade (net off) and continue boot --
 *      a wedged netd init must never brick the boot.
 *
 * The fault listener (s10) contains a netd crash: it runs the reply-slot sweep
 * (CNode_Move each reserved reply cap out of the netd cnode and Send -EIO, so
 * parked recv/accept/connect callers error out instead of hanging forever),
 * clears the device IRQ, and zeroes net_ep_cap / net_available so children get
 * -ENOTSUP and serverstats renders the net row dead.
 *
 * All root threads here (init, listener) are prio 200 on core 0 -- the
 * load-bearing invariant: every root thread shares ONE lock-free allocman, so
 * netd-adjacent root work must not run on another core.
 */
#ifdef AIOS_NETD

#include "aios/root_shared.h"
#include "aios/netd_ctrl.h"
#include "aios/netd_handoff.h"
#include "aios/netd_stats.h"
#include "aios/net.h"
#include "aios/config.h"
#include "aios/mono_wait.h"
#include "plat/net_hal.h"
#include <sel4utils/process.h>
#include <sel4utils/process_config.h>
#include <sel4utils/thread.h>
#include <sel4utils/vspace.h>
#include <vka/capops.h>
#include <stdio.h>
#define LOG_MODULE "netd"
#define LOG_LEVEL  LOG_LEVEL_DEBUG
#include "aios/aios_log.h"

#define NETD_READY_MS 10000   /* bounded DEVD_READY wait ceiling (non-MCS, s8) */

/* ---- root-side state shared between netd_prov(), spawn_netd(), and the fault
 * listener. All run prio 200 on core 0, so plain globals with no lock are safe. */
static driver_handoff_t  g_ho;                  /* prov result (frame caps, vaddrs, MAC) */
static int               g_prov_ok;             /* 1 once plat_net_prov succeeded */
static sel4utils_process_t netd_proc;           /* retained per DESIGN_NETD s3 */
static seL4_CPtr   netd_net_ep;                 /* root-side net_ep (published on READY) */
static seL4_CPtr   netd_ctrl_ep;                /* root-side ctrl/fault EP */
static seL4_Word   netd_reply_base;             /* base of the reserved reply-slot range (netd cnode) */
static cspacepath_t netd_sweep_scratch;         /* pre-allocated reply-sweep target slot */
static volatile int netd_ready;                 /* set by the listener on DEVD_READY */
static volatile int netd_failed;                /* set on DEVD_FAIL / fault / timeout */
static volatile int netd_fault_count;

/* Start a root helper thread at prio 200 pinned to core 0 (mirrors
 * boot_services.c start_server_thread; arg_cap delivered as entry arg0). */
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

/* ---- netd_prov: root-side provisioning. Runs in boot_net_init BEFORE the boot
 * banner so net_hw_present is set at banner time (DESIGN_NETD s8). ---- */
int netd_prov(void) {
    if (plat_net_prov(&g_ho) != 0) {
        AIOS_LOG_WARN("netd: plat_net_prov failed -- network unavailable");
        net_hw_present = 0;
        return -1;
    }
    g_prov_ok = 1;
    net_hw_present = 1;
    /* s8: net_available is published ONLY on DEVD_READY. The QEMU probe set it 1
     * early; clear it so a child spawned before READY sees child_net=0 (-ENOTSUP)
     * rather than a live-but-unserved net_ep. */
    net_available = 0;
    AIOS_LOG_INFO("netd: provisioned (net_hw_present=1)");
    return 0;
}

/* ---- Copy n device-frame caps into fresh root slots and map the COPIES into
 * the netd vspace NON-CACHEABLE. One frame cap tracks one mapping, so root keeps
 * the originals (still mapped in root for postmortem) and netd gets copies; the
 * non-cacheable attribute is identical at both ends (the A72 dual-mapping rule,
 * DESIGN_NETD s4). Returns the netd vaddr, or NULL on failure. ---- */
static void *netd_map_frames_nc(const seL4_CPtr *src, int n) {
    seL4_CPtr copies[64];
    if (n <= 0 || n > 64) return NULL;
    for (int i = 0; i < n; i++) {
        cspacepath_t s, d;
        vka_cspace_make_path(&vka, src[i], &s);
        if (vka_cspace_alloc_path(&vka, &d)) {
            AIOS_LOG_ERROR_V("netd: frame copy cslot alloc failed at ", i);
            return NULL;
        }
        if (seL4_CNode_Copy(d.root, d.capPtr, d.capDepth,
                            s.root, s.capPtr, s.capDepth, seL4_AllRights)) {
            AIOS_LOG_ERROR_V("netd: frame cap copy failed at ", i);
            return NULL;
        }
        copies[i] = d.capPtr;
    }
    /* cacheable=0 -> device/non-cacheable, matching the root mapping. */
    return vspace_map_pages(&netd_proc.vspace, copies, NULL, seL4_AllRights,
                            (size_t)n, seL4_PageBits, 0);
}

/* ---- dedicated fault/ctrl listener: Recv-s the netd ctrl EP forever. Receives
 * BOTH netd status messages (DEVD_READY / DEVD_FAIL, labels >6) and
 * kernel-delivered netd faults (labels <=6) on the same endpoint, by label. ---- */
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
        if (label <= 6) {
            /* netd FAULT (DESIGN_NETD s10). Decode + log, then CONTAIN + RECOVER:
             *  - zero net_ep_cap + net_available so children get -ENOTSUP and
             *    serverstats renders the net row dead;
             *  - reply-slot SWEEP: a parked recv/accept/connect caller (sshd,
             *    netconsole) lives in blocking accept/recv INSIDE netd; on
             *    non-MCS seL4 only a move-and-Send wakes it (deleting the saved
             *    reply cap does NOT). CNode_Move each reserved slot out of the netd
             *    cnode into a root scratch slot and Send -EIO so they error out
             *    instead of hanging until reboot;
             *  - clear the device IRQ on the root retained IRQHandler, else every
             *    later device IRQ signals a dead TCB and the line stays
             *    masked-and-unacked forever.
             * the netd frames + cnode leak until reboot (accepted v1, s10). */
            seL4_Word pc   = seL4_GetMR(seL4_VMFault_IP);
            seL4_Word addr = seL4_GetMR(seL4_VMFault_Addr);
            netd_fault_count++;
            netd_failed = 1;
            net_ep_cap = 0;
            net_available = 0;
            printf("[netd-listener] FAULT label=%lu pc=0x%lx addr=0x%lx -- net DOWN, sweeping\n",
                   (unsigned long)label, (unsigned long)pc, (unsigned long)addr);

            int woke = 0;
            for (int i = 0; i < NETD_REPLY_SLOTS; i++) {
                int mverr = seL4_CNode_Move(
                    netd_sweep_scratch.root, netd_sweep_scratch.capPtr,
                    netd_sweep_scratch.capDepth,
                    netd_proc.cspace.cptr, netd_reply_base + i, 12);
                if (mverr) continue;   /* slot empty (no parked caller) */
                seL4_SetMR(0, (seL4_Word)(-5));   /* EIO -> the parked call errors out */
                seL4_Send(netd_sweep_scratch.capPtr,
                          seL4_MessageInfo_new(0, 0, 0, 1));
                seL4_CNode_Delete(netd_sweep_scratch.root,
                                  netd_sweep_scratch.capPtr,
                                  netd_sweep_scratch.capDepth);
                woke++;
            }
            if (g_ho.irq_handler)
                seL4_IRQHandler_Clear(g_ho.irq_handler);
            printf("[netd-listener] swept %d parked caller(s); IRQ cleared\n", woke);
            AIOS_LOG_ERROR_V("netd: FAULT contained + swept; parked woken=", woke);
            continue;
        }
        AIOS_LOG_ERROR_V("netd: ctrl unknown label=", (unsigned long)label);
    }
}

void spawn_netd(seL4_CPtr net_ep) {
    int err;
    uint64_t t0 = mono_ticks();

    if (!g_prov_ok) {
        AIOS_LOG_ERROR("netd: spawn called without successful prov -- skipping");
        return;
    }
    netd_net_ep = net_ep;

    /* 1. ctrl/fault EP (root owns; also the netd kernel fault endpoint). */
    vka_object_t ctrl_ep_obj;
    if (vka_alloc_endpoint(&vka, &ctrl_ep_obj)) {
        AIOS_LOG_ERROR("netd: ctrl EP alloc failed -- skipping spawn");
        return;
    }
    netd_ctrl_ep = ctrl_ep_obj.cptr;

    /* 2. badge=2 kick mint of the netd RX ntfn -- the root-side kick that wakes
     * the netd loop for the heartbeat/renew (3c) and /proc/genet.kick. GENET prov
     * already minted net_kick_ntfn_cap; QEMU prov did not, so mint it here. */
    if (!net_kick_ntfn_cap) {
        cspacepath_t ksrc, kdst;
        vka_cspace_make_path(&vka, g_ho.irq_ntfn, &ksrc);
        if (!vka_cspace_alloc_path(&vka, &kdst) &&
            !seL4_CNode_Mint(kdst.root, kdst.capPtr, kdst.capDepth,
                             ksrc.root, ksrc.capPtr, ksrc.capDepth,
                             seL4_AllRights, (seL4_Word)NETD_KICK_BADGE))
            net_kick_ntfn_cap = kdst.capPtr;
        else
            AIOS_LOG_WARN("netd: kick badge mint failed (idle heartbeat/renew may stall)");
    }

    /* 3. Pre-allocate the reply-sweep CNode_Move target slot ONCE (reused per
     * swept slot) so the listener never allocates from the global vka at fault
     * time (DESIGN_NETD s10). */
    if (vka_cspace_alloc_path(&vka, &netd_sweep_scratch)) {
        AIOS_LOG_ERROR("netd: sweep scratch alloc failed -- skipping spawn");
        return;
    }

    /* 4. Configure the isolated process: own 12-bit cnode, fresh vspace, CPIO ELF
     * "netd", prio 200, fault endpoint = the ctrl EP (so the listener gets the netd
     * faults AND its DEVD_READY/FAIL on one endpoint). */
    sel4utils_process_config_t config = process_config_new(&simple);
    config = process_config_elf(config, "netd", true);
    config = process_config_create_cnode(config, 12);
    config = process_config_create_vspace(config, NULL, 0);
    config = process_config_priority(config, 200);
    config = process_config_auth(config, simple_get_tcb(&simple));
    config = process_config_fault_endpoint(config, ctrl_ep_obj);
    err = sel4utils_configure_process_custom(&netd_proc, &vka, &vspace, config);
    if (err) { AIOS_LOG_ERROR_V("netd: configure failed err=", err); return; }

    /* 5. Donate caps into netd (decimal argv slots, tty_server precedent).
     *  - net_ep / ctrl_ep: copies of the root endpoint objects.
     *  - IRQHandler: a copy (netd Acks per drain; root keeps the original for
     *    the s10 IRQ clear + respawn).
     *  - RX ntfn: a copy of the UNBADGED notification (netd self-binds it to its
     *    own TCB; the IRQ keeps signaling the root badge-1 mint, which signals the
     *    same object -> the netd bound Recv wakes badge 1). DESIGN_NETD s3. */
    seL4_CPtr s_net  = sel4utils_copy_cap_to_process(&netd_proc, &vka, net_ep);
    seL4_CPtr s_ctrl = sel4utils_copy_cap_to_process(&netd_proc, &vka, ctrl_ep_obj.cptr);
    seL4_CPtr s_irqh = sel4utils_copy_cap_to_process(&netd_proc, &vka, g_ho.irq_handler);
    seL4_CPtr s_ntfn = sel4utils_copy_cap_to_process(&netd_proc, &vka, g_ho.irq_ntfn);

    /* 6. Copy + map the device MMIO + DMA frame sets into the netd vspace
     * non-cacheable. Root chooses the netd vaddrs; netd uses what we tell it. */
    void *mmio_va = netd_map_frames_nc(g_ho.mmio_frames, g_ho.mmio_nframes);
    void *dma_va  = netd_map_frames_nc(g_ho.dma_frames,  g_ho.dma_nframes);
    if (!mmio_va || !dma_va) {
        AIOS_LOG_ERROR("netd: MMIO/DMA frame map into netd failed -- skipping spawn");
        return;
    }

    /* 6b. Stats page (DESIGN_NETD s6): one CACHEABLE-BOTH single-writer frame. Map
     * the original cacheable in root (the fs thread renders /proc/net + serverstats
     * reads the heartbeat IPC-free), copy the cap, map the copy CACHEABLE in netd
     * (netd is the sole writer). Same memory type both ends per the v0.4.165
     * pipe-SHM rule; root + netd both run on core 0 so a volatile read is coherent. */
    void *stats_netd_va = NULL;
    {
        vka_object_t stf;
        if (vka_alloc_frame(&vka, seL4_PageBits, &stf)) {
            AIOS_LOG_ERROR("netd: stats frame alloc failed -- skipping spawn");
            return;
        }
        void *root_va = vspace_map_pages(&vspace, &stf.cptr, NULL,
                                         seL4_AllRights, 1, seL4_PageBits, 1 /*cacheable*/);
        if (!root_va) { AIOS_LOG_ERROR("netd: stats root map failed"); return; }
        for (size_t i = 0; i < sizeof(struct netd_stats); i++)
            ((volatile uint8_t *)root_va)[i] = 0;
        netd_stats_root = (struct netd_stats *)root_va;

        cspacepath_t ssrc, sdst;
        vka_cspace_make_path(&vka, stf.cptr, &ssrc);
        if (vka_cspace_alloc_path(&vka, &sdst) ||
            seL4_CNode_Copy(sdst.root, sdst.capPtr, sdst.capDepth,
                            ssrc.root, ssrc.capPtr, ssrc.capDepth, seL4_AllRights)) {
            AIOS_LOG_ERROR("netd: stats frame copy failed -- skipping spawn");
            return;
        }
        stats_netd_va = vspace_map_pages(&netd_proc.vspace, &sdst.capPtr, NULL,
                                         seL4_AllRights, 1, seL4_PageBits, 1 /*cacheable*/);
        if (!stats_netd_va) { AIOS_LOG_ERROR("netd: stats netd map failed"); return; }
    }

    /* 7. Reserve the SaveCaller reply-slot range AFTER all donations: take the
     * current cspace cursor as the base, then bump it past the range so the spawn
     * never reuses these slots. net_server.c (in netd) parks reply caps here; the
     * fault listener CNode_Moves them out for the s10 sweep. DESIGN_NETD s3 row 6. */
    netd_reply_base = netd_proc.cspace_next_free;
    netd_proc.cspace_next_free += NETD_REPLY_SLOTS;

    /* 8. Start the fault/ctrl listener BEFORE resuming netd, so an early crash
     * (even pre-printf) is still caught + logged. */
    if (netd_start_root_thread(
            (sel4utils_thread_entry_fn)netd_fault_listener_fn, netd_ctrl_ep)) {
        AIOS_LOG_ERROR("netd: listener thread start failed -- skipping spawn");
        return;
    }

    /* 9. argv (NETD_ARGV_* layout, netd_ctrl.h). Pack the MAC 48-bit and the
     * static IP config 32-bit each; netd unpacks them. */
    uint64_t macp = ((uint64_t)g_ho.mac[0] << 40) | ((uint64_t)g_ho.mac[1] << 32) |
                    ((uint64_t)g_ho.mac[2] << 24) | ((uint64_t)g_ho.mac[3] << 16) |
                    ((uint64_t)g_ho.mac[4] << 8)  | ((uint64_t)g_ho.mac[5]);
    uint32_t ipp   = ((uint32_t)net_cfg_ip[0]   << 24) | ((uint32_t)net_cfg_ip[1]   << 16) |
                     ((uint32_t)net_cfg_ip[2]   << 8)  | net_cfg_ip[3];
    uint32_t gwp   = ((uint32_t)net_cfg_gw[0]   << 24) | ((uint32_t)net_cfg_gw[1]   << 16) |
                     ((uint32_t)net_cfg_gw[2]   << 8)  | net_cfg_gw[3];
    uint32_t maskp = ((uint32_t)net_cfg_mask[0] << 24) | ((uint32_t)net_cfg_mask[1] << 16) |
                     ((uint32_t)net_cfg_mask[2] << 8)  | net_cfg_mask[3];

    char a[NETD_ARGV_COUNT][24];
    char *child_argv[NETD_ARGV_COUNT];
    seL4_Word vals[NETD_ARGV_COUNT];
    vals[NETD_ARGV_NET_EP]      = (seL4_Word)s_net;
    vals[NETD_ARGV_CTRL_EP]     = (seL4_Word)s_ctrl;
    vals[NETD_ARGV_IRQ_HANDLER] = (seL4_Word)s_irqh;
    vals[NETD_ARGV_NTFN]        = (seL4_Word)s_ntfn;
    vals[NETD_ARGV_MMIO_VADDR]  = (seL4_Word)(uintptr_t)mmio_va;
    vals[NETD_ARGV_SLOT]        = (seL4_Word)g_ho.slot;
    vals[NETD_ARGV_DMA_VADDR]   = (seL4_Word)(uintptr_t)dma_va;
    vals[NETD_ARGV_DMA_PADDR]   = (seL4_Word)g_ho.dma_paddr;
    vals[NETD_ARGV_MAC]         = (seL4_Word)macp;
    vals[NETD_ARGV_CFG_IP]      = (seL4_Word)ipp;
    vals[NETD_ARGV_CFG_GW]      = (seL4_Word)gwp;
    vals[NETD_ARGV_CFG_MASK]    = (seL4_Word)maskp;
    vals[NETD_ARGV_FLAGS]       = 0;
    vals[NETD_ARGV_STATS_VADDR] = (seL4_Word)(uintptr_t)stats_netd_va;
    vals[NETD_ARGV_REPLY_BASE]  = (seL4_Word)netd_reply_base;
    for (int i = 0; i < NETD_ARGV_COUNT; i++) {
        snprintf(a[i], sizeof a[i], "%lu", (unsigned long)vals[i]);
        child_argv[i] = a[i];
    }

    err = sel4utils_spawn_process_v(&netd_proc, &vka, &vspace,
                                     NETD_ARGV_COUNT, child_argv, 1);
    if (err) { AIOS_LOG_ERROR_V("netd: spawn failed err=", err); return; }

    printf("[netd] spawned in %lu cntpct ticks (mmio_va=%p dma_va=%p reply_base=%lu)\n",
           (unsigned long)(mono_ticks() - t0), mmio_va, dma_va,
           (unsigned long)netd_reply_base);

    /* 10. Bounded wait for DEVD_READY (non-MCS has no timed Recv; cntpct ceiling
     * + Yield is the sanctioned boot-time bounded poll). On READY: PUBLISH the
     * client-visible net_ep_cap + net_available (s8). On timeout / DEVD_FAIL /
     * early fault: degrade -- net stays off, boot continues. */
    uint64_t dl = mono_deadline_ms(NETD_READY_MS);
    while (!netd_ready && !netd_failed && mono_before(dl)) seL4_Yield();
    if (netd_ready && !netd_failed) {
        net_ep_cap = netd_net_ep;
        net_available = 1;
        AIOS_LOG_INFO("netd: READY -- net_ep published, network up");
    } else {
        net_ep_cap = 0;
        net_available = 0;
        AIOS_LOG_ERROR("netd: READY timeout/fail -> degrade (network off, boot continues)");
    }
}

#endif /* AIOS_NETD */
