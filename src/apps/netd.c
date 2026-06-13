/*
 * netd.c -- MMU-isolated network daemon (DESIGN_NETD).
 *
 * Stage 2 = SKELETON ONLY. No network logic yet (that arrives in Stage 3). The
 * job here is to prove the *mechanism* a leaf driver-process needs before any
 * net code is trusted to it:
 *   - spawn as an isolated CPIO process (own cnode + vspace, prio 200),
 *   - parse caps/values from argv (decimal strings, tty_server precedent),
 *   - SELF-BIND its notification to its own TCB so one blocking seL4_Recv wakes
 *     on both client IPC and a Signal (the bound-notification trick netd's RX
 *     IRQ will use),
 *   - announce readiness on the ctrl/fault EP (Send, never Call -- a Call would
 *     park us BlockedOnReply where the bound notification cannot wake us),
 *   - DEFER a reply by SaveCaller-ing the client into a reserved slot of its OWN
 *     cnode, then complete it later -- both the in-netd way (netd Sends to the
 *     saved slot on a badge-2 kick) and the root-driven way (root CNode_Moves
 *     the saved reply cap out of netd's cnode and Sends it: the reply-sweep
 *     that a crash recovery would use; DESIGN_NETD s10 -- the kernel bet),
 *   - and crash on demand so the root-side fault listener can prove containment.
 *
 * printf reaches the serial console via muslc -> seL4_DebugPutChar (KernelPrinting
 * is on for both targets), so no log-ring / serial_ep is handed to netd.
 */
#include <sel4/sel4.h>
#include <sel4runtime.h>
#include <sel4utils/process.h>   /* SEL4UTILS_TCB_SLOT, SEL4UTILS_CNODE_SLOT */
#include <stdio.h>
#include "aios/netd_skel.h"

static seL4_Word argv_word(const char *s) {
    seL4_Word v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (seL4_Word)(*s++ - '0');
    return v;
}

int main(int argc, char **argv) {
    /* argv: [0]=main/test EP slot, [1]=ctrl/fault EP slot, [2]=notification slot,
     * [3]=base of the reserved SaveCaller-slot range in our own cnode. */
    if (argc < 4) {
        printf("[netd] FATAL: argc=%d (need 4 args)\n", argc);
        return 1;
    }
    seL4_CPtr net_ep  = (seL4_CPtr)argv_word(argv[0]);
    seL4_CPtr ctrl_ep = (seL4_CPtr)argv_word(argv[1]);
    seL4_CPtr ntfn    = (seL4_CPtr)argv_word(argv[2]);
    seL4_Word rsvd    = argv_word(argv[3]);

    /* Self-bind the notification to our own TCB. A silently-failed bind is the
     * historical "RX never wakes" signature, so assert it loudly and fail the
     * ctrl handshake rather than come up half-wired. */
    int berr = seL4_TCB_BindNotification(SEL4UTILS_TCB_SLOT, ntfn);
    if (berr != seL4_NoError) {
        printf("[netd] FATAL: self-bind ntfn err=%d\n", berr);
        seL4_SetMR(0, (seL4_Word)berr);
        seL4_Send(ctrl_ep, seL4_MessageInfo_new(DEVD_FAIL, 0, 0, 1));
        return 2;
    }

    printf("[netd] skeleton up: net_ep=%lu ctrl_ep=%lu ntfn=%lu rsvd=%lu (bound)\n",
           (unsigned long)net_ep, (unsigned long)ctrl_ep,
           (unsigned long)ntfn, (unsigned long)rsvd);

    /* Tell root we are ready (BEFORE any blocking loop). */
    seL4_Send(ctrl_ep, seL4_MessageInfo_new(DEVD_READY, 0, 0, 0));

    /* A NETD_BLOCK_KICK caller, if any, is parked in slot rsvd+0. */
    int kick_parked = 0;

    /* Single-threaded event loop: the bound notification means this Recv also
     * wakes on a Signal to ntfn (badge in `badge`). */
    while (1) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t msg = seL4_Recv(net_ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);

        /* ---- notification wake (bound ntfn) ---- */
        if (label == 0 && badge != 0) {
            /* badge 2 = root kick: self-wake the parked KICK caller via the reply
             * cap we saved in our OWN cnode -- the in-netd analog of net_server's
             * blocked-recv wake (child-cnode SaveCaller + Send + Delete). */
            if (badge == NETD_KICK_BADGE && kick_parked) {
                seL4_SetMR(0, (seL4_Word)NETD_KICK_TOKEN);
                seL4_Send(rsvd + 0, seL4_MessageInfo_new(0, 0, 0, 1));
                seL4_CNode_Delete(SEL4UTILS_CNODE_SLOT, rsvd + 0, seL4_WordBits);
                kick_parked = 0;
                printf("[netd] kick: woke parked caller in slot %lu\n",
                       (unsigned long)(rsvd + 0));
            }
            continue;
        }

        if (label == NETD_PING) {
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));

        } else if (label == NETD_BLOCK_KICK) {
            /* Defer: save the caller into our own cnode, tell root, loop. Root
             * Signals badge 2; we wake the caller above. */
            int rc = seL4_CNode_SaveCaller(SEL4UTILS_CNODE_SLOT,
                                           rsvd + 0, seL4_WordBits);
            if (rc != seL4_NoError) {
                printf("[netd] BLOCK_KICK SaveCaller err=%d\n", rc);
                seL4_SetMR(0, (seL4_Word)-5);   /* EIO */
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            } else {
                kick_parked = 1;
                seL4_SetMR(0, NETD_PARK_KICK);
                seL4_SetMR(1, rsvd + 0);
                seL4_Send(ctrl_ep, seL4_MessageInfo_new(NETD_PARKED, 0, 0, 2));
            }

        } else if (label == NETD_BLOCK_SWEEP) {
            /* Defer into our cnode, then ask root to reply-sweep it: root will
             * CNode_Move the saved reply cap OUT of our cnode and Send it. We
             * never touch slot rsvd+1 again -- root owns the wake from here. */
            int rc = seL4_CNode_SaveCaller(SEL4UTILS_CNODE_SLOT,
                                           rsvd + 1, seL4_WordBits);
            if (rc != seL4_NoError) {
                printf("[netd] BLOCK_SWEEP SaveCaller err=%d\n", rc);
                seL4_SetMR(0, (seL4_Word)-5);   /* EIO */
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            } else {
                seL4_SetMR(0, NETD_PARK_SWEEP);
                seL4_SetMR(1, rsvd + 1);
                seL4_Send(ctrl_ep, seL4_MessageInfo_new(NETD_PARKED, 0, 0, 2));
            }

        } else if (label == NETD_CRASH) {
            printf("[netd] NETD_CRASH: dereferencing NULL\n");
            *(volatile int *)0 = 0;             /* fault -> root fault listener */
            /* not reached */

        } else if (label != 0) {
            seL4_SetMR(0, (seL4_Word)-1);        /* unknown op */
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
    }
}
