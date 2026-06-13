/*
 * netd.c -- MMU-isolated network daemon (DESIGN_NETD Stage 3 cutover).
 *
 * netd is a single-threaded, MMU-isolated CPIO process that owns the entire net
 * stack: the socket server (net_server.c), the protocol stack (net_stack.c /
 * net_tcp.c / net_dhcp.c) and the platform NIC driver dev half (net_virtio.c /
 * net_genet.c, all compiled in under NETD_BUILD). Root retains every
 * allocator-touching duty (DMA / IRQ / MAC / device frames) and provisions them
 * before spawn; netd receives the results via argv (DESIGN_NETD s3) and the
 * MMIO + DMA frames pre-mapped into its vspace by root.
 *
 * Startup (DESIGN_NETD s2/s8):
 *   1. parse argv (decimal cap slots + vaddrs/paddr/mac/cfg/reply-base),
 *   2. SELF-BIND the RX notification to its own TCB, so one blocking seL4_Recv
 *      in net_server_fn wakes on both client IPC and packet arrival,
 *   3. plat_net_dev_attach() latches the root-provisioned MMIO/DMA/IRQ/MAC,
 *   4. plat_net_init() runs the device-register sequence (dev half),
 *   5. publish net_available + netd_reply_slot_base,
 *   6. announce DEVD_READY on the ctrl EP (Send, never Call) BEFORE DHCP,
 *   7. net_server_fn() -- the merged loop: drain HW ring, process rx, DHCP at
 *      setup, then serve the socket API forever.
 *
 * On any init failure netd Sends DEVD_FAIL(errcode) so the root listener degrades
 * the boot (net off) instead of bricking it. printf reaches serial via muslc ->
 * seL4_DebugPutChar (KernelPrinting on both targets); netd has no log ring.
 */
#include <sel4/sel4.h>
#include <sel4runtime.h>
#include <sel4utils/process.h>   /* SEL4UTILS_TCB_SLOT */
#include <stdio.h>
#include <stdint.h>
#include "aios/netd_ctrl.h"
#include "aios/root_shared.h"    /* net_available, net_ep_cap, net_drv_ntfn_cap, net_server_fn */
#include "aios/net.h"
#include "aios/config.h"         /* net_cfg_ip/gw/mask */
#include "aios/netd_stats.h"     /* struct netd_stats (the /proc/net page) */
#include "plat/net_hal.h"        /* plat_net_dev_attach, plat_net_init */

/* netd-local reply-slot base (netd_shim.c); net_server.c reads it to address its
 * SaveCaller slots in the netd cnode instead of calling vka. */
extern seL4_CPtr netd_reply_slot_base;
/* netd-local /proc/net stats page (netd_shim.c); net_server.c writes the
 * heartbeat + state through it each loop iteration (DESIGN_NETD s6). */
extern struct netd_stats *netd_stats_page;

static seL4_Word aw(const char *s) {
    seL4_Word v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (seL4_Word)(*s++ - '0');
    return v;
}

static void unpack4(seL4_Word w, uint8_t out[4]) {
    out[0] = (uint8_t)(w >> 24); out[1] = (uint8_t)(w >> 16);
    out[2] = (uint8_t)(w >> 8);  out[3] = (uint8_t)(w);
}

int main(int argc, char **argv) {
    if (argc < NETD_ARGV_COUNT) {
        printf("[netd] FATAL: argc=%d (need %d args)\n", argc, NETD_ARGV_COUNT);
        return 1;
    }

    seL4_CPtr net_ep  = (seL4_CPtr)aw(argv[NETD_ARGV_NET_EP]);
    seL4_CPtr ctrl_ep = (seL4_CPtr)aw(argv[NETD_ARGV_CTRL_EP]);
    seL4_CPtr irqh    = (seL4_CPtr)aw(argv[NETD_ARGV_IRQ_HANDLER]);
    seL4_CPtr ntfn    = (seL4_CPtr)aw(argv[NETD_ARGV_NTFN]);
    uintptr_t mmio_va = (uintptr_t)aw(argv[NETD_ARGV_MMIO_VADDR]);
    int       slot    = (int)aw(argv[NETD_ARGV_SLOT]);
    uintptr_t dma_va  = (uintptr_t)aw(argv[NETD_ARGV_DMA_VADDR]);
    uint64_t  dma_pa  = (uint64_t)aw(argv[NETD_ARGV_DMA_PADDR]);
    seL4_Word macp    = aw(argv[NETD_ARGV_MAC]);
    uintptr_t stats_va= (uintptr_t)aw(argv[NETD_ARGV_STATS_VADDR]);
    seL4_Word rbase   = aw(argv[NETD_ARGV_REPLY_BASE]);

    /* Static IP config (DHCP fallback) -- root parsed /etc/network.conf. */
    unpack4(aw(argv[NETD_ARGV_CFG_IP]),   net_cfg_ip);
    unpack4(aw(argv[NETD_ARGV_CFG_GW]),   net_cfg_gw);
    unpack4(aw(argv[NETD_ARGV_CFG_MASK]), net_cfg_mask);

    /* Self-bind the RX notification to our own TCB. A silently-failed bind is the
     * historical "RX never wakes" signature, so fail the handshake rather than
     * come up half-wired (DESIGN_NETD s3 row 3). */
    int berr = seL4_TCB_BindNotification(SEL4UTILS_TCB_SLOT, ntfn);
    if (berr != seL4_NoError) {
        printf("[netd] FATAL: self-bind ntfn err=%d\n", berr);
        seL4_SetMR(0, (seL4_Word)berr);
        seL4_Send(ctrl_ep, seL4_MessageInfo_new(DEVD_FAIL, 0, 0, 1));
        return 2;
    }
    net_drv_ntfn_cap = ntfn;   /* for completeness; the bind is what matters */

    uint8_t mac[6] = {
        (uint8_t)(macp >> 40), (uint8_t)(macp >> 32), (uint8_t)(macp >> 24),
        (uint8_t)(macp >> 16), (uint8_t)(macp >> 8),  (uint8_t)(macp)
    };

    printf("[netd] up: net_ep=%lu ctrl=%lu irq=%lu ntfn=%lu mmio=0x%lx dma=0x%lx pa=0x%lx rsvd=%lu\n",
           (unsigned long)net_ep, (unsigned long)ctrl_ep, (unsigned long)irqh,
           (unsigned long)ntfn, (unsigned long)mmio_va, (unsigned long)dma_va,
           (unsigned long)dma_pa, (unsigned long)rbase);

    /* Latch the root-provisioned device resources, then program the device. */
    plat_net_dev_attach(mmio_va, slot, dma_va, dma_pa, irqh, mac);

    /* QEMU virtio dev-half plat_net_init requires net_available=1 on entry;
     * GENET sets it itself. Set it first either way (netd-local). */
    net_available = 1;
    int rc = plat_net_init();
    if (rc != 0 || !net_available) {
        printf("[netd] FATAL: plat_net_init rc=%d net_available=%d\n", rc, net_available);
        seL4_SetMR(0, (seL4_Word)(rc ? rc : -1));
        seL4_Send(ctrl_ep, seL4_MessageInfo_new(DEVD_FAIL, 0, 0, 1));
        return 3;
    }

    /* net_server.c addresses its SaveCaller slots as rbase + {0,8,16} + sid. */
    netd_reply_slot_base = (seL4_CPtr)rbase;
    /* /proc/net stats page (s6): net_server_fn writes the heartbeat + state here
     * each loop iteration. Mapped cacheable by root at this vaddr in netd. */
    netd_stats_page = (struct netd_stats *)stats_va;

#ifdef NETD_TEST_NO_READY
    /* Forced-degrade gate (DESIGN_NETD s8): a netd that comes up but never sends
     * DEVD_READY must NOT brick boot -- the spawn_netd cntpct-bounded wait must time
     * out and degrade (net off, boot continues to login). Skip READY + the serve
     * loop and spin so netd stays alive-but-silent (the "spawned but never READY"
     * case). Built only when AIOS_NETD_TEST_DEGRADE is ON; default OFF. */
    printf("[netd] TEST: NETD_TEST_NO_READY set -- skipping DEVD_READY (degrade gate)\n");
    for (;;) seL4_Yield();
#endif

    /* Announce readiness BEFORE DHCP (DESIGN_NETD s8): device init is
     * milliseconds, DHCP can take seconds on a real LAN -- root must publish
     * net_ep_cap now so the getty fork-and-forget netconsole/sshd/sntp capture a
     * live cap. Send (NOT Call): a Call would park us BlockedOnReply where the
     * bound notification cannot wake us. */
    seL4_Send(ctrl_ep, seL4_MessageInfo_new(DEVD_READY, 0, 0, 0));

    /* Hand off to the merged event loop. It does DHCP + gratuitous ARP at setup,
     * then drains the HW ring (plat_net_drain) and serves the socket API forever.
     * Never returns. */
    net_server_fn((void *)(uintptr_t)net_ep, 0, 0);
    return 0;   /* not reached */
}
