#ifndef AIOS_NETD_CTRL_H
#define AIOS_NETD_CTRL_H
#include <stdint.h>
/*
 * netd_ctrl.h -- Stage-3 netd <-> root spawn/handshake protocol (DESIGN_NETD
 * s3/s8). Replaces the throwaway Stage-2 netd_skel.h.
 *
 * Shared by src/boot/spawn_netd.c (root, AIOS_NETD) and src/apps/netd.c
 * (NETD_BUILD), so the argv cap-handoff layout and the ctrl/fault EP labels can
 * never skew on the wire.
 *
 * netd is spawned with its ctrl/fault EP also configured as its kernel fault
 * endpoint, so ONE root listener thread receives BOTH netd status messages
 * (DEVD_READY/DEVD_FAIL, labels >6) and kernel-delivered netd faults (labels
 * <=6 on aarch64), discriminated by label.
 */

/* ---- ctrl/fault EP: netd -> root listener (Send, never Call -- a Call would
 * park netd BlockedOnReply where its bound notification cannot wake it). All
 * strictly above the aarch64 seL4 fault-label range (<=6). ---- */
#define DEVD_READY   100   /* device init done + net_available set; ready to serve */
#define DEVD_FAIL    101   /* fatal init failure; errcode in MR0 */

/* ---- bound-ntfn badges, root -> netd (DESIGN_NETD s3 row 3b). The IRQ delivers
 * via a badge-1 mint; the root kick (heartbeat/renew wake, /proc/genet.kick) via
 * a badge-2 mint. The netd wake test ignores badge 0, so every Signal aimed at
 * netd goes through a badged copy. ---- */
#define NETD_IRQ_BADGE   1
#define NETD_KICK_BADGE  2

/* ---- argv cap-handoff layout (decimal strings; 24-char buffers fit 64-bit
 * vaddrs/paddrs). spawn_netd fills these; netd main parses them. Caps [0..3] are
 * child cspace slots (sel4utils_copy_cap_to_process); the MMIO/DMA frames are
 * MAPPED into the netd vspace by root (netd never invokes them), so only their
 * netd vaddr + the device paddr travel here. ---- */
enum {
    NETD_ARGV_NET_EP = 0,    /* net_ep child slot (clients Call this, ABI-stable) */
    NETD_ARGV_CTRL_EP,       /* ctrl/fault EP child slot (DEVD_READY/FAIL Send)   */
    NETD_ARGV_IRQ_HANDLER,   /* IRQHandler child slot (netd Acks per drain)       */
    NETD_ARGV_NTFN,          /* unbadged RX ntfn child slot (netd self-binds it)  */
    NETD_ARGV_MMIO_VADDR,    /* device MMIO window base vaddr in netd             */
    NETD_ARGV_SLOT,          /* QEMU virtio slot index; RPi4 0 (single device)    */
    NETD_ARGV_DMA_VADDR,     /* DMA region vaddr in netd                          */
    NETD_ARGV_DMA_PADDR,     /* DMA region bus/device paddr (device-visible)      */
    NETD_ARGV_MAC,           /* MAC packed 48-bit (RPi4 mailbox; 0 on QEMU)       */
    NETD_ARGV_CFG_IP,        /* static config ip packed 32-bit (DHCP fallback)    */
    NETD_ARGV_CFG_GW,        /* static config gateway packed 32-bit               */
    NETD_ARGV_CFG_MASK,      /* static config netmask packed 32-bit               */
    NETD_ARGV_FLAGS,         /* reserved (platform / irq-mode bits); 0 today      */
    NETD_ARGV_STATS_VADDR,   /* /proc/net stats page vaddr in netd (s6)           */
    NETD_ARGV_REPLY_BASE,    /* base of the reserved SaveCaller slot range        */
    NETD_ARGV_COUNT
};

/* Debug-only crash trigger for the s10 crash-containment demo: a NET_DIAG (103)
 * request with MR0 = this magic makes netd null-deref, so the root fault listener
 * can prove containment + the reply-slot sweep. Sent fire-and-forget by
 * /proc/netd.crash; compiled into net_server ONLY under NETD_BUILD (the in-root
 * flag-OFF server never has a crash op). */
#define NETD_DIAG_CRASH  0xC4A54   /* leetspeak "crash" */

/* NET_DIAG (label 103) active sub-ops, passed in MR0 (DESIGN_NETD s6/s11). The
 * userland /bin/netdiag tool Calls net_ep with one of these; net_server (netd)
 * dispatches to plat_net_diag() against the LIVE device netd owns. The fs thread
 * must NEVER Call net_ep -- a hung netd would wedge every /proc read -- so a
 * sacrificial userland process is the only diag caller, and /proc/genet stays a
 * read-only, UMAC/MDIO-free root view. Small ints, distinct from the
 * NETD_DIAG_CRASH magic. Args travel in MR1..MR3; the reply carries MR0 = status
 * (0 ok, <0 err) and MR1/MR2 = result words. */
enum {
    NETD_DIAG_PEEK = 1,  /* MR1=off                 -> MR1=value (device reg)  */
    NETD_DIAG_POKE,      /* MR1=off MR2=val         -> MR1=readback            */
    NETD_DIAG_MR,        /* MR1=phy MR2=reg         -> MR1=value (MDIO read)   */
    NETD_DIAG_MW,        /* MR1=phy MR2=reg MR3=val -> MR1=readback (MDIO wr)  */
    NETD_DIAG_TX,        /* send one test frame     -> MR1=txprod MR2=txcons   */
    NETD_DIAG_REINIT,    /* re-run ring_init        -> status                  */
    NETD_DIAG_IRQON,     /* unmask + IRQ-driven RX  -> status                  */
    NETD_DIAG_IRQOFF,    /* mask IRQ + kick server  -> status                  */
    NETD_DIAG_MAC,       /* read device MAC -> MR1=mac[0..3] MR2=mac[4..5]     */
};

/* Client helper (libaios_posix, src/lib/posix_net.c): issue one NET_DIAG op as a
 * blocking Call to net_ep, return MR0 (status); out[0]/out[1] receive MR1/MR2.
 * Used by /bin/netdiag. Returns -ENOTSUP if net is unavailable (net_ep == 0). */
int aios_net_diag(int op, uint32_t a, uint32_t b, uint32_t c, uint32_t out[2]);

/* SaveCaller reply slots netd carves out of its own cnode AFTER the cap
 * donations (DESIGN_NETD s3 row 6): 3 per socket (recv / accept / connect park)
 * for MAX_NET_SOCKETS=8 = 24. spawn_netd reserves this many past
 * cspace_next_free; net_server.c addresses them as base + {0,8,16} + sid. A
 * _Static_assert in net_server.c pins this to 3*MAX_NET_SOCKETS. */
#define NETD_REPLY_SLOTS  24

#endif /* AIOS_NETD_CTRL_H */
