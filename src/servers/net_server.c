/*
 * net_server.c -- Network server thread
 *
 * M4: TCP state machine with listen/accept/recv/send via SaveCaller.
 * Socket table supports both SOCK_DGRAM (UDP) and SOCK_STREAM (TCP).
 */
#include "aios/root_shared.h"
#include "aios/net.h"
#include "aios/config.h"
#include <stdio.h>

#define MAX_NET_SOCKETS  8
#define SOCK_RX_BUF_SZ   32768   /* v0.4.171: 32KB rx window (was 4KB) -- 8x fewer
                                  * window-reopen round-trips on a large push */

struct net_socket {
    int      in_use;
    int      type;              /* 2=SOCK_DGRAM, 1=SOCK_STREAM */
    int      state;             /* TCP state (TCP_CLOSED..TCP_FIN_WAIT) */
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t  remote_ip[4];
    uint8_t  remote_mac[6];
    uint32_t snd_nxt;
    uint32_t rcv_nxt;

    /* RX buffer */
    uint8_t  rxbuf[SOCK_RX_BUF_SZ];
    uint16_t rxlen;
    uint16_t rx_head;    /* TCP ring write pos */
    uint16_t rx_tail;    /* TCP ring read pos */
    uint16_t rx_ack_tail;/* v0.4.226: rx_tail at last window-reopen ACK (coalescing) */
    uint8_t  rx_eof;     /* FIN received */
    uint8_t  rx_src_ip[4];
    uint16_t rx_src_port;

    /* Blocked recv reader */
    int       has_blocked;
    seL4_CPtr blocked_cap;
    int       blocked_max;

    /* Blocked accept (TCP LISTEN sockets only) */
    int       has_accept_blocked;
    seL4_CPtr accept_blocked_cap;

    /* Parent listen socket (for finding accept waiter) */
    int       listen_parent;

    /* v0.4.86: owner PID for cleanup on process exit */
    int       owner_pid;

    /* v0.4.86: blocked connect (client-side TCP) */
    int       has_connect_blocked;
    seL4_CPtr connect_blocked_cap;

    /* v0.4.225 corruption-hunt: cumulative bytes stored/read on this conn
     * (= sequential stream offset, which for netconsole __put == file off). */
    uint32_t  dbg_store_off;
    uint32_t  dbg_read_off;
};

static struct net_socket sockets[MAX_NET_SOCKETS];
static seL4_CPtr blocked_slots[MAX_NET_SOCKETS];
static seL4_CPtr accept_slots[MAX_NET_SOCKETS];
static seL4_CPtr connect_slots[MAX_NET_SOCKETS];

/* v0.4.229 (netd Stage 0): reply-slot poisoning fix (DESIGN_NETD F4).
 *
 * A SaveCaller'd reply cap parked in blocked/accept/connect_slots[] must be
 * accounted for on EVERY path that retires a socket, or the slot is reused by a
 * later park while still holding the stale cap: seL4_CNode_SaveCaller into an
 * occupied slot fails, the server keeps pointing at the old cap, and the next
 * delivery replies to the wrong (or dead) caller while the real one hangs.
 *
 * Two flavours of retirement:
 *  - reset (RST): the peer is gone but the caller is a LIVE process -- wake it
 *    with an error (seL4_Send) so its blocking call returns, THEN delete the
 *    spent cap. On non-MCS seL4 deleting a reply cap does NOT wake a caller --
 *    only the Send does (DESIGN_NETD s5 reply-cap ground truth).
 *  - drop (close / pid-exit): the owner is closing the fd or being reaped, so
 *    there is nobody to wake -- just delete each cap to free the slot. */
static void net_sock_wake_reset(struct net_socket *sk) {
    if (sk->has_blocked) {
        seL4_SetMR(0, (seL4_Word)(-104));   /* ECONNRESET -> blocking recv */
        seL4_Send(sk->blocked_cap, seL4_MessageInfo_new(0, 0, 0, 1));
        seL4_CNode_Delete(seL4_CapInitThreadCNode,
                          sk->blocked_cap, seL4_WordBits);
        sk->has_blocked = 0;
    }
    if (sk->has_accept_blocked) {
        seL4_SetMR(0, (seL4_Word)(-1));     /* -> accept (maps to ECONNREFUSED) */
        seL4_Send(sk->accept_blocked_cap, seL4_MessageInfo_new(0, 0, 0, 1));
        seL4_CNode_Delete(seL4_CapInitThreadCNode,
                          sk->accept_blocked_cap, seL4_WordBits);
        sk->has_accept_blocked = 0;
    }
    if (sk->has_connect_blocked) {
        seL4_SetMR(0, (seL4_Word)(-111));   /* ECONNREFUSED -> connect */
        seL4_Send(sk->connect_blocked_cap, seL4_MessageInfo_new(0, 0, 0, 1));
        seL4_CNode_Delete(seL4_CapInitThreadCNode,
                          sk->connect_blocked_cap, seL4_WordBits);
        sk->has_connect_blocked = 0;
    }
}

static void net_sock_drop_parked(struct net_socket *sk) {
    if (sk->has_blocked) {
        seL4_CNode_Delete(seL4_CapInitThreadCNode,
                          sk->blocked_cap, seL4_WordBits);
        sk->has_blocked = 0;
    }
    if (sk->has_accept_blocked) {
        seL4_CNode_Delete(seL4_CapInitThreadCNode,
                          sk->accept_blocked_cap, seL4_WordBits);
        sk->has_accept_blocked = 0;
    }
    if (sk->has_connect_blocked) {
        seL4_CNode_Delete(seL4_CapInitThreadCNode,
                          sk->connect_blocked_cap, seL4_WordBits);
        sk->has_connect_blocked = 0;
    }
}

/* v0.4.229 (netd Stage 0): park the current caller's reply cap in `slot`.
 * Deletes any stale cap left in the slot first (deleting an empty slot is a
 * no-op) so an async RST/close that raced this park cannot leave SaveCaller
 * staring at an occupied slot, and checks the rc: on failure the caller is
 * replied -EIO instead of being silently parked against a bogus cap (it would
 * otherwise hang forever). Returns 0 if the caller is now parked, -1 if it was
 * already replied (do not set the has_*_blocked bookkeeping). */
static int net_park_caller(seL4_CPtr slot) {
    seL4_CNode_Delete(seL4_CapInitThreadCNode, slot, seL4_WordBits);
    int rc = seL4_CNode_SaveCaller(seL4_CapInitThreadCNode, slot, seL4_WordBits);
    if (rc != seL4_NoError) {
        seL4_SetMR(0, (seL4_Word)(-5));   /* EIO: could not save reply cap */
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        return -1;
    }
    return 0;
}

/* ---- UDP delivery (unchanged from M3) ---- */
void net_udp_deliver(uint16_t dst_port, uint16_t src_port,
                     const uint8_t *src_ip,
                     const uint8_t *data, uint32_t len) {
    for (int i = 0; i < MAX_NET_SOCKETS; i++) {
        struct net_socket *s = &sockets[i];
        if (!s->in_use || s->type != 2) continue;
        if (s->local_port != dst_port) continue;

        if (s->has_blocked) {
            int n = (int)len;
            if (n > s->blocked_max) n = s->blocked_max;
            uint32_t ip_word = ((uint32_t)src_ip[0] << 24) |
                               ((uint32_t)src_ip[1] << 16) |
                               ((uint32_t)src_ip[2] << 8) | src_ip[3];
            seL4_SetMR(0, (seL4_Word)n);
            seL4_SetMR(1, (seL4_Word)ip_word);
            seL4_SetMR(2, (seL4_Word)src_port);
            int mr = 3;
            seL4_Word w = 0;
            for (int j = 0; j < n; j++) {
                w |= ((seL4_Word)data[j]) << ((j % 8) * 8);
                if (j % 8 == 7 || j == n - 1) { seL4_SetMR(mr++, w); w = 0; }
            }
            seL4_Send(s->blocked_cap, seL4_MessageInfo_new(0, 0, 0, mr));
            seL4_CNode_Delete(seL4_CapInitThreadCNode,
                              s->blocked_cap, seL4_WordBits);
            s->has_blocked = 0;
            return;
        }
        if (s->rxlen == 0 && len <= SOCK_RX_BUF_SZ) {
            for (uint32_t j = 0; j < len; j++) s->rxbuf[j] = data[j];
            s->rxlen = (uint16_t)len;
            for (int j = 0; j < 4; j++) s->rx_src_ip[j] = src_ip[j];
            s->rx_src_port = src_port;
        }
        return;
    }
}

/* ---- TCP delivery (called from handle_tcp in net_tcp.c) ---- */
void net_tcp_deliver(const uint8_t *src_ip, const uint8_t *src_mac,
                     uint16_t src_port, uint16_t dst_port,
                     uint32_t seq, uint32_t ack_val, uint8_t flags,
                     const uint8_t *data, int data_len) {

    /* RST: reset any matching connection */
    if (flags & TCP_RST) {
        for (int i = 0; i < MAX_NET_SOCKETS; i++) {
            struct net_socket *s = &sockets[i];
            if (s->in_use && s->type == 1 && s->local_port == dst_port &&
                s->state >= TCP_SYN_RCVD) {
                /* v0.4.229: wake EVERY parked caller and delete its reply cap,
                 * not just connect() (v0.4.86 only did connect). A parked recv
                 * left behind here hung forever AND poisoned its reply slot for
                 * the next socket to reuse that index. Matched states are
                 * SYN_RCVD/ESTAB/FIN_WAIT (recv parks) and SYN_SENT (connect
                 * parks); LISTEN is excluded, so accept is handled by the
                 * close/exit drop path instead. */
                net_sock_wake_reset(s);
                s->state = TCP_CLOSED;
                s->in_use = 0;
            }
        }
        return;
    }

    /* SYN on a LISTEN socket: create new connection socket */
    if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
        int listen_idx = -1;
        for (int i = 0; i < MAX_NET_SOCKETS; i++) {
            if (sockets[i].in_use && sockets[i].type == 1 &&
                sockets[i].state == TCP_LISTEN &&
                sockets[i].local_port == dst_port) {
                listen_idx = i;
                break;
            }
        }
        if (listen_idx < 0) return; /* no listener */

        /* Allocate new socket for this connection */
        int ci = -1;
        for (int i = 0; i < MAX_NET_SOCKETS; i++) {
            if (!sockets[i].in_use) { ci = i; break; }
        }
        if (ci < 0) return; /* no slots */

        struct net_socket *conn = &sockets[ci];
        conn->in_use = 1;
        conn->type = 1;
        conn->state = TCP_SYN_RCVD;
        conn->local_port = dst_port;
        conn->remote_port = src_port;
        for (int j = 0; j < 4; j++) conn->remote_ip[j] = src_ip[j];
        for (int j = 0; j < 6; j++) conn->remote_mac[j] = src_mac[j];
        conn->snd_nxt = 1000;
        conn->rcv_nxt = seq + 1;
        conn->rxlen = 0;
        conn->rx_head = 0;
        conn->rx_tail = 0;
        conn->rx_ack_tail = 0;
        conn->rx_eof = 0;
        conn->has_blocked = 0;
        conn->has_accept_blocked = 0;
        conn->has_connect_blocked = 0;
        conn->listen_parent = listen_idx;
        /* v0.4.87: inherit owner PID from listen socket so
         * NET_CLEANUP_PID frees connection on process death */
        conn->owner_pid = sockets[listen_idx].owner_pid;

        /* Send SYN-ACK */
        net_tcp_send(conn->remote_ip, conn->remote_mac,
                     dst_port, src_port,
                     conn->snd_nxt, conn->rcv_nxt,
                     TCP_SYN | TCP_ACK, NULL, 0);
        conn->snd_nxt++;
        return;
    }

    /* Find established/SYN_RCVD connection by port + remote */
    int si = -1;
    for (int i = 0; i < MAX_NET_SOCKETS; i++) {
        struct net_socket *s = &sockets[i];
        if (!s->in_use || s->type != 1) continue;
        if (s->local_port != dst_port || s->remote_port != src_port) continue;
        if (s->remote_ip[0] != src_ip[0] || s->remote_ip[1] != src_ip[1] ||
            s->remote_ip[2] != src_ip[2] || s->remote_ip[3] != src_ip[3]) continue;
        if (s->state >= TCP_SYN_RCVD) { si = i; break; }
    }
    if (si < 0) return;

    struct net_socket *s = &sockets[si];

    /* SYN_SENT + SYN-ACK -> ESTABLISHED (client-side connect) */
    if (s->state == TCP_SYN_SENT && (flags & TCP_SYN) && (flags & TCP_ACK)) {
        s->rcv_nxt = seq + 1;
        s->state = TCP_ESTAB;
        /* Send ACK to complete 3-way handshake */
        tcp_tx_window = SOCK_RX_BUF_SZ - 1;
        net_tcp_send(s->remote_ip, s->remote_mac,
                     s->local_port, s->remote_port,
                     s->snd_nxt, s->rcv_nxt, TCP_ACK, NULL, 0);
        /* Wake blocked connect() caller */
        if (s->has_connect_blocked) {
            seL4_SetMR(0, 0);
            seL4_Send(s->connect_blocked_cap,
                      seL4_MessageInfo_new(0, 0, 0, 1));
            seL4_CNode_Delete(seL4_CapInitThreadCNode,
                              s->connect_blocked_cap, seL4_WordBits);
            s->has_connect_blocked = 0;
        }
    }

    /* SYN_RCVD + ACK -> ESTABLISHED */
    if (s->state == TCP_SYN_RCVD && (flags & TCP_ACK)) {
        s->state = TCP_ESTAB;

        /* Wake blocked accept on parent listen socket */
        int pi = s->listen_parent;
        if (pi >= 0 && pi < MAX_NET_SOCKETS &&
            sockets[pi].has_accept_blocked) {
            seL4_SetMR(0, (seL4_Word)si);
            seL4_Send(sockets[pi].accept_blocked_cap,
                      seL4_MessageInfo_new(0, 0, 0, 1));
            seL4_CNode_Delete(seL4_CapInitThreadCNode,
                              sockets[pi].accept_blocked_cap, seL4_WordBits);
            sockets[pi].has_accept_blocked = 0;
            /* v0.4.163: handed to a blocked accept -- mark consumed so the
             * NET_ACCEPT backlog scan does not return it a second time. If no
             * accept was blocked, listen_parent stays set and the connection
             * waits in the backlog for the next accept(). */
            s->listen_parent = -1;
        }
    }

    /* ESTABLISHED: receive data */
    if (s->state == TCP_ESTAB && data_len > 0) {
        /* v0.4.87: retransmit tolerance -- accept retransmissions
         * (seq < rcv_nxt) by re-ACKing without buffering duplicate data.
         * Reject future/out-of-order (seq > rcv_nxt) with dup ACK. */
        if (seq != s->rcv_nxt) {
            /* Re-ACK with current rcv_nxt so sender knows where we are */
            net_tcp_send(s->remote_ip, s->remote_mac,
                         s->local_port, s->remote_port,
                         s->snd_nxt, s->rcv_nxt, TCP_ACK, NULL, 0);
            if (seq < s->rcv_nxt) {
                /* Retransmission: check if tail end has new data.
                 * If segment overlaps rcv_nxt, deliver the new portion. */
                uint32_t seg_end = seq + (uint32_t)data_len;
                if (seg_end > s->rcv_nxt) {
                    int skip = (int)(s->rcv_nxt - seq);
                    data += skip;
                    data_len -= skip;
                    net_rx_stats.tcp_overlap_trims++;
                    /* Fall through to deliver the new tail bytes */
                } else {
                    net_rx_stats.tcp_dup_segs++;
                    return;  /* Pure retransmission, already ACKed */
                }
            } else {
                net_rx_stats.tcp_ooo_drops++;
                return;  /* Out-of-order (seq > rcv_nxt), drop */
            }
        }
        /* v0.4.170: accept ONLY what we can deliver+buffer, and ACK ONLY that.
         * The old code did rcv_nxt += data_len unconditionally, then silently
         * DROPPED any bytes that did not fit the rx ring -- i.e. it ACKed data it
         * threw away, so the sender never retransmitted it and a multi-window push
         * stalled with a permanently short stream (worked <=5KB, hung >=20KB). Now
         * we advance rcv_nxt by the accepted count and advertise the TRUE free
         * window; the sender retransmits the unACKed tail when the window reopens
         * (the read-side window update in NET_RECVFROM provides the reopen). */
        {
            int mask = SOCK_RX_BUF_SZ - 1;
            int accepted = 0;

            if (s->has_blocked) {
                /* A reader is parked: hand it up to blocked_max bytes now. */
                int d = data_len;
                if (d > s->blocked_max) d = s->blocked_max;
                seL4_SetMR(0, (seL4_Word)d);
                seL4_SetMR(1, 0);
                seL4_SetMR(2, 0);
                int mr = 3;
                seL4_Word w = 0;
                for (int j = 0; j < d; j++) {
                    w |= ((seL4_Word)data[j]) << ((j % 8) * 8);
                    if (j % 8 == 7 || j == d - 1) { seL4_SetMR(mr++, w); w = 0; }
                }
                seL4_Send(s->blocked_cap, seL4_MessageInfo_new(0, 0, 0, mr));
                seL4_CNode_Delete(seL4_CapInitThreadCNode,
                                  s->blocked_cap, seL4_WordBits);
                s->has_blocked = 0;
                accepted = d;
                net_rx_stats.tcp_reader_handoff++;
                /* Buffer any remainder that fits the ring. */
                int rest = data_len - d;
                if (rest > 0) {
                    net_rx_stats.tcp_split_deliver++;
                    int free_sp = SOCK_RX_BUF_SZ - 1 -
                                  ((s->rx_head - s->rx_tail) & mask);
                    int b = rest < free_sp ? rest : free_sp;
                    for (int j = 0; j < b; j++) {
                        s->rxbuf[s->rx_head & mask] = data[d + j];
                        s->rx_head++;
                    }
                    accepted += b;
                }
            } else {
                /* No reader parked: buffer what fits; the rest is unACKed and
                 * the sender will retransmit it once the window reopens. */
                int free_sp = SOCK_RX_BUF_SZ - 1 -
                              ((s->rx_head - s->rx_tail) & mask);
                int b = data_len < free_sp ? data_len : free_sp;
                for (int j = 0; j < b; j++) {
                    if (data[j] == 0xFF && net_rx_stats.dbg_ff_store_off == 0)
                        net_rx_stats.dbg_ff_store_off = s->dbg_store_off + j;
                    s->rxbuf[s->rx_head & mask] = data[j];
                    s->rx_head++;
                }
                s->dbg_store_off += b;
                net_rx_stats.dbg_store_bytes += b;
                accepted = b;
            }

            /* Advance rcv_nxt only by the accepted bytes, then ACK with the real
             * free window so the sender's flow control tracks us exactly. */
            if (accepted < data_len)
                net_rx_stats.tcp_window_partial++;
            s->rcv_nxt += (uint32_t)accepted;
            int used = (s->rx_head - s->rx_tail) & mask;
            int freew = SOCK_RX_BUF_SZ - 1 - used;
            if (freew < 0) freew = 0;
            tcp_tx_window = (uint16_t)freew;
            net_tcp_send(s->remote_ip, s->remote_mac,
                         s->local_port, s->remote_port,
                         s->snd_nxt, s->rcv_nxt, TCP_ACK, NULL, 0);
        }
    }

    /* FIN received */
    if (flags & TCP_FIN) {
        s->rcv_nxt++;
        net_tcp_send(s->remote_ip, s->remote_mac,
                     s->local_port, s->remote_port,
                     s->snd_nxt, s->rcv_nxt, TCP_ACK, NULL, 0);

        /* Signal EOF to blocked reader (len=0) */
        if (s->has_blocked) {
            seL4_SetMR(0, 0); /* len=0 = EOF */
            seL4_Send(s->blocked_cap, seL4_MessageInfo_new(0, 0, 0, 1));
            seL4_CNode_Delete(seL4_CapInitThreadCNode,
                              s->blocked_cap, seL4_WordBits);
            s->has_blocked = 0;
        }
        s->rx_eof = 1;
        s->state = TCP_FIN_WAIT;
    }

    /* FIN_WAIT + ACK -> closed */
    if (s->state == TCP_FIN_WAIT && (flags & TCP_ACK) && !(flags & TCP_FIN)) {
        s->state = TCP_CLOSED;
        s->in_use = 0;
    }
}

/* ---- Server thread ---- */
void net_server_fn(void *arg0, void *arg1, void *ipc_buf) {
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    (void)arg1; (void)ipc_buf;

    for (int i = 0; i < MAX_NET_SOCKETS; i++) {
        cspacepath_t p1, p2;
        vka_cspace_alloc_path(&vka, &p1);
        blocked_slots[i] = p1.capPtr;
        vka_cspace_alloc_path(&vka, &p2);
        accept_slots[i] = p2.capPtr;
        cspacepath_t p3;
        vka_cspace_alloc_path(&vka, &p3);
        connect_slots[i] = p3.capPtr;
    }


    /* v0.4.152: acquire an address via DHCP before announcing/using an IP.
     * Falls back to the static /etc/network.conf config on timeout. */
    if (net_available) {
        printf("[net] DHCP: discovering...\n");
        if (net_dhcp_acquire() == 0)
            printf("[net] DHCP: lease acquired\n");
        else
            printf("[net] DHCP: no lease, using static %d.%d.%d.%d\n",
                   net_cfg_ip[0], net_cfg_ip[1], net_cfg_ip[2], net_cfg_ip[3]);
    }

    net_send_gratuitous_arp();
    uint8_t gw[4];
    for (int _g = 0; _g < 4; _g++) gw[_g] = net_cfg_gw[_g];
    net_send_arp_request(gw);

    int selftest_done = 0;

    while (1) {
        /* Poll rx_ring */
        while (net_rx_ring.tail != net_rx_ring.head) {
            uint32_t t = net_rx_ring.tail % NET_RX_RING_SIZE;
            /* v0.4.225: ACQUIRE -- order the entry reads after the head read
             * that published the slot. The producer's dmb (data before head)
             * pairs with this; without it the A72 may satisfy the entry
             * loads early and hand a torn/stale packet to the stack. */
            __asm__ volatile("dmb sy" ::: "memory");
            struct rx_pkt_entry *entry = &net_rx_ring.pkts[t];
            if (entry->len > 0)
                net_handle_packet(entry->data, entry->len);
            __asm__ volatile("dmb sy" ::: "memory");
            net_rx_ring.tail++;
        }

        if (!selftest_done && net_arp_resolved(gw)) {
            net_send_ping(gw);
            selftest_done = 1;
        }


        /* IPC: socket API */
        seL4_Word badge = 0;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);

        if (label == 0 && badge != 0) {
            /* Notification wake -- re-poll ring immediately */
            continue;
        }

        if (label == NET_SOCKET) {
            int type = (int)seL4_GetMR(1);
            int slot = -1;
            for (int i = 0; i < MAX_NET_SOCKETS; i++) {
                if (!sockets[i].in_use) { slot = i; break; }
            }
            if (slot >= 0) {
                sockets[slot].in_use = 1;
                sockets[slot].type = type;
                sockets[slot].state = TCP_CLOSED;
                sockets[slot].local_port = 0;
                sockets[slot].rxlen = 0;
                sockets[slot].rx_head = 0;
                sockets[slot].rx_tail = 0;
                sockets[slot].rx_ack_tail = 0;
                sockets[slot].rx_eof = 0;
                sockets[slot].has_blocked = 0;
                sockets[slot].has_accept_blocked = 0;
                sockets[slot].listen_parent = -1;
                sockets[slot].owner_pid = (int)seL4_GetMR(3);
                sockets[slot].has_connect_blocked = 0;
            }
            seL4_SetMR(0, (seL4_Word)slot);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));

        } else if (label == NET_BIND) {
            int sid = (int)seL4_GetMR(0);
            uint16_t port = (uint16_t)seL4_GetMR(1);
            int rc = -1;
            if (sid >= 0 && sid < MAX_NET_SOCKETS && sockets[sid].in_use) {
                sockets[sid].local_port = port;
                rc = 0;
            }
            seL4_SetMR(0, (seL4_Word)rc);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));

        } else if (label == NET_LISTEN) {
            int sid = (int)seL4_GetMR(0);
            int rc = -1;
            if (sid >= 0 && sid < MAX_NET_SOCKETS && sockets[sid].in_use &&
                sockets[sid].type == 1) {
                sockets[sid].state = TCP_LISTEN;
                rc = 0;
            }
            seL4_SetMR(0, (seL4_Word)rc);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));

        } else if (label == NET_ACCEPT) {
            int sid = (int)seL4_GetMR(0);
            /* v0.4.175: nonblock accept flag in MR1, GUARDED by message length so
             * old 1-MR callers (v1 netconsole, sshd) keep the blocking behaviour
             * -- a stale MR1 must never flip them to a spurious EAGAIN. */
            int acc_nb = (seL4_MessageInfo_get_length(msg) >= 2) ? (int)seL4_GetMR(1) : 0;
            if (sid < 0 || sid >= MAX_NET_SOCKETS || !sockets[sid].in_use ||
                sockets[sid].state != TCP_LISTEN) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            } else {
                /* v0.4.163: drain the accept backlog before blocking. A
                 * connection can reach ESTABLISHED before the app re-enters
                 * accept() -- the wake-on-handshake only fires when a caller is
                 * already blocked. Without this scan a fast reconnect blocks
                 * forever on an already-established socket, the limitation that
                 * capped netconsole and sshd at one session. A pending child is
                 * an ESTABLISHED socket whose listen_parent still points here;
                 * clear it to -1 to mark it consumed. */
                int pending = -1;
                for (int i = 0; i < MAX_NET_SOCKETS; i++) {
                    if (sockets[i].in_use && sockets[i].type == 1 &&
                        sockets[i].state == TCP_ESTAB &&
                        sockets[i].listen_parent == sid) {
                        pending = i;
                        break;
                    }
                }
                if (pending >= 0) {
                    sockets[pending].listen_parent = -1;
                    seL4_SetMR(0, (seL4_Word)pending);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                } else if (acc_nb) {
                    /* v0.4.175: non-blocking accept -- no pending connection, so
                     * reply EAGAIN (-11) instead of parking the caller. Lets a
                     * single-process event loop (netconsole2) poll accept() while
                     * it services its other sessions. */
                    seL4_SetMR(0, (seL4_Word)-11);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                } else if (net_park_caller(accept_slots[sid]) == 0) {
                    sockets[sid].accept_blocked_cap = accept_slots[sid];
                    sockets[sid].has_accept_blocked = 1;
                }
            }

        } else if (label == NET_SENDTO) {
            int sid  = (int)seL4_GetMR(0);
            int len  = (int)seL4_GetMR(1);
            uint32_t dst_ip   = (uint32_t)seL4_GetMR(2);
            uint16_t dst_port = (uint16_t)seL4_GetMR(3);
            uint8_t pdata[900];
            int mr = 4;
            seL4_Word w = 0;
            for (int i = 0; i < len && i < 900; i++) {
                if (i % 8 == 0) w = seL4_GetMR(mr++);
                pdata[i] = (uint8_t)(w & 0xFF);
                w >>= 8;
            }
            int rc = -1;
            if (sid >= 0 && sid < MAX_NET_SOCKETS && sockets[sid].in_use) {
                struct net_socket *sk = &sockets[sid];
                if (sk->type == 2) {
                    /* UDP */
                    rc = net_udp_send(sid, sk->local_port,
                                      dst_ip, dst_port, pdata, len);
                    if (rc == 0) rc = len;
                } else if (sk->type == 1 && sk->state == TCP_ESTAB) {
                    /* TCP: send data segment */
                    net_tcp_send(sk->remote_ip, sk->remote_mac,
                                 sk->local_port, sk->remote_port,
                                 sk->snd_nxt, sk->rcv_nxt,
                                 TCP_ACK | TCP_PSH, pdata, len);
                    sk->snd_nxt += len;
                    rc = len;
                }
            }
            seL4_SetMR(0, (seL4_Word)rc);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));

        } else if (label == NET_RECVFROM) {
            int sid = (int)seL4_GetMR(0);
            int max = (int)seL4_GetMR(1);
            int nb  = (int)seL4_GetMR(2);  /* v0.4.84: O_NONBLOCK flag */
            if (sid < 0 || sid >= MAX_NET_SOCKETS || !sockets[sid].in_use) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            } else if (sockets[sid].type == 1) {
                /* TCP: read from circular buffer */
                struct net_socket *sk = &sockets[sid];
                int mask = SOCK_RX_BUF_SZ - 1;
                int avail = (sk->rx_head - sk->rx_tail) & mask;
                if (avail > 0) {
                    int n = avail;
                    if (n > max) n = max;
                    if (n > 900) n = 900;
                    seL4_SetMR(0, (seL4_Word)n);
                    seL4_SetMR(1, 0);
                    seL4_SetMR(2, 0);
                    int mr2 = 3;
                    seL4_Word w2 = 0;
                    for (int j = 0; j < n; j++) {
                        uint8_t rb = sk->rxbuf[sk->rx_tail & mask];
                        if (rb == 0xFF && net_rx_stats.dbg_ff_read_off == 0)
                            net_rx_stats.dbg_ff_read_off = sk->dbg_read_off + j;
                        w2 |= ((seL4_Word)rb) << ((j % 8) * 8);
                        sk->rx_tail++;
                        if (j % 8 == 7 || j == n - 1) { seL4_SetMR(mr2++, w2); w2 = 0; }
                    }
                    sk->dbg_read_off += n;
                    net_rx_stats.dbg_read_bytes += n;
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, mr2));
                    /* v0.4.170: TCP window update on read. Draining the rx ring
                     * frees receive-window space; advertise it to the sender with
                     * a pure ACK, else a push that filled the window stalls (the
                     * sender is window-blocked and only RECEIVE used to ACK).
                     *
                     * v0.4.226 throughput: COALESCE these. The old code sent a
                     * window-update ACK on EVERY 900B read = ~1740 GENET TX +
                     * network RTTs per 1.5MB push, the HW receive bottleneck
                     * (QEMU hid it: near-zero TX latency). Send the reopen ACK
                     * only when >=1/4 buffer (8KB) has been drained since the
                     * last one, OR the ring just went empty (the sender may be
                     * window-blocked with nothing left to trigger a receive-ACK
                     * -- must wake it). net_tcp_deliver still ACKs per received
                     * segment, so in-flight data keeps advancing rcv_nxt; this
                     * only thins the redundant reopen ACKs. ~9x fewer TX. */
                    {
                        int used = (sk->rx_head - sk->rx_tail) & mask;
                        int freew = SOCK_RX_BUF_SZ - 1 - used;
                        if (freew < 0) freew = 0;
                        uint16_t freed_since =
                            (uint16_t)(sk->rx_tail - sk->rx_ack_tail);
                        if (freed_since >= (SOCK_RX_BUF_SZ / 4) || used == 0) {
                            sk->rx_ack_tail = sk->rx_tail;
                            tcp_tx_window = (uint16_t)freew;
                            net_tcp_send(sk->remote_ip, sk->remote_mac,
                                         sk->local_port, sk->remote_port,
                                         sk->snd_nxt, sk->rcv_nxt, TCP_ACK, NULL, 0);
                            net_rx_stats.tcp_read_acks++;
                        }
                    }
                } else if (sk->rx_eof) {
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                } else {
                    if (nb) {
                        /* v0.4.84: O_NONBLOCK -- return EAGAIN */
                        seL4_SetMR(0, (seL4_Word)(-11));
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    } else if (net_park_caller(blocked_slots[sid]) == 0) {
                        sk->blocked_cap = blocked_slots[sid];
                        sk->blocked_max = max;
                        sk->has_blocked = 1;
                    }
                }
            } else if (sockets[sid].rxlen > 0) {
                /* UDP: single datagram */
                struct net_socket *sk = &sockets[sid];
                int n = sk->rxlen;
                if (n > max) n = max;
                uint32_t ip_word = ((uint32_t)sk->rx_src_ip[0] << 24) |
                                   ((uint32_t)sk->rx_src_ip[1] << 16) |
                                   ((uint32_t)sk->rx_src_ip[2] << 8) |
                                   sk->rx_src_ip[3];
                seL4_SetMR(0, (seL4_Word)n);
                seL4_SetMR(1, (seL4_Word)ip_word);
                seL4_SetMR(2, (seL4_Word)sk->rx_src_port);
                int mr2 = 3;
                seL4_Word w2 = 0;
                for (int j = 0; j < n; j++) {
                    w2 |= ((seL4_Word)sk->rxbuf[j]) << ((j % 8) * 8);
                    if (j % 8 == 7 || j == n - 1) { seL4_SetMR(mr2++, w2); w2 = 0; }
                }
                sk->rxlen = 0;
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, mr2));
            } else {
                if (nb) {
                    seL4_SetMR(0, (seL4_Word)(-11));
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                } else if (net_park_caller(blocked_slots[sid]) == 0) {
                    sockets[sid].blocked_cap = blocked_slots[sid];
                    sockets[sid].blocked_max = max;
                    sockets[sid].has_blocked = 1;
                }
            }

        } else if (label == NET_CLOSE_SOCK) {
            int sid = (int)seL4_GetMR(0);
            if (sid >= 0 && sid < MAX_NET_SOCKETS && sockets[sid].in_use) {
                struct net_socket *sk = &sockets[sid];
                /* v0.4.229: free any SaveCaller'd reply slot before retiring the
                 * socket, like op-98 -- otherwise a stale cap poisons the slot
                 * when this index is reused (DESIGN_NETD F4). */
                net_sock_drop_parked(sk);
                /* TCP: send FIN if connected */
                if (sk->type == 1 && sk->state == TCP_ESTAB) {
                    net_tcp_send(sk->remote_ip, sk->remote_mac,
                                 sk->local_port, sk->remote_port,
                                 sk->snd_nxt, sk->rcv_nxt,
                                 TCP_ACK | TCP_FIN, NULL, 0);
                    sk->snd_nxt++;
                    sk->state = TCP_FIN_WAIT;
                } else {
                    sk->in_use = 0;
                }
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));

        } else if (label == 98) {
            /* v0.4.86: NET_CLEANUP_PID -- free sockets owned by exiting process */
            int dead_pid = (int)seL4_GetMR(0);
            int freed = 0;
            for (int i = 0; i < MAX_NET_SOCKETS; i++) {
                if (sockets[i].in_use && sockets[i].owner_pid == dead_pid) {
                    struct net_socket *sk = &sockets[i];
                    if (sk->type == 1 && sk->state == TCP_ESTAB) {
                        net_tcp_send(sk->remote_ip, sk->remote_mac,
                                     sk->local_port, sk->remote_port,
                                     sk->snd_nxt, sk->rcv_nxt,
                                     TCP_ACK | TCP_FIN, NULL, 0);
                        sk->snd_nxt++;
                    }
                    /* v0.4.229: same delete-parked-caps logic, now via the
                     * shared helper. The owner is being reaped, so delete
                     * without waking (DESIGN_NETD s5 reply-cap ground truth:
                     * deleting a saved reply cap does not wake a parked caller;
                     * only the caller's own teardown frees it). */
                    net_sock_drop_parked(sk);
                    sk->in_use = 0;
                    sk->state = TCP_CLOSED;
                    freed++;
                }
            }
            seL4_SetMR(0, (seL4_Word)freed);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));

        } else if (label == NET_CONNECT) {
            /* v0.4.86: TCP connect() -- client-side 3-way handshake */
            int sid = (int)seL4_GetMR(0);
            uint32_t dst_ip_word = (uint32_t)seL4_GetMR(1);
            uint16_t dst_port = (uint16_t)seL4_GetMR(2);
            int caller_pid = (int)seL4_GetMR(3);
            if (sid < 0 || sid >= MAX_NET_SOCKETS || !sockets[sid].in_use ||
                sockets[sid].type != 1) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            } else {
                struct net_socket *sk = &sockets[sid];
                /* Resolve destination MAC via ARP (use gateway for off-subnet) */
                uint8_t dst_ip[4];
                dst_ip[0] = (dst_ip_word >> 24) & 0xFF;
                dst_ip[1] = (dst_ip_word >> 16) & 0xFF;
                dst_ip[2] = (dst_ip_word >> 8) & 0xFF;
                dst_ip[3] = dst_ip_word & 0xFF;
                uint8_t dst_mac[6];
                if (arp_cache_lookup(dst_ip, dst_mac) != 0) {
                    uint8_t gw[4];
                    for (int g = 0; g < 4; g++) gw[g] = net_cfg_gw[g];
                    if (arp_cache_lookup(gw, dst_mac) != 0) {
                        seL4_SetMR(0, (seL4_Word)(-101)); /* ENETUNREACH */
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                        goto connect_done;
                    }
                }
                /* Assign ephemeral port if not bound */
                if (sk->local_port == 0) {
                    static uint16_t ephemeral = 49152;
                    sk->local_port = ephemeral++;
                    if (ephemeral == 0) ephemeral = 49152;
                }
                /* Set up connection state */
                for (int j = 0; j < 4; j++) sk->remote_ip[j] = dst_ip[j];
                for (int j = 0; j < 6; j++) sk->remote_mac[j] = dst_mac[j];
                sk->remote_port = dst_port;
                sk->snd_nxt = 1000 + (uint32_t)(uintptr_t)sk; /* simple ISN */
                sk->rcv_nxt = 0;
                sk->rx_head = 0;
                sk->rx_tail = 0;
                sk->rx_ack_tail = 0;
                sk->rx_eof = 0;
                sk->owner_pid = caller_pid;
                sk->state = TCP_SYN_SENT;
                /* Send SYN */
                tcp_tx_window = SOCK_RX_BUF_SZ - 1;
                net_tcp_send(sk->remote_ip, sk->remote_mac,
                             sk->local_port, sk->remote_port,
                             sk->snd_nxt, 0, TCP_SYN, NULL, 0);
                sk->snd_nxt++;
                /* Block until SYN-ACK received (or RST -> ECONNREFUSED). */
                if (net_park_caller(connect_slots[sid]) == 0) {
                    sk->connect_blocked_cap = connect_slots[sid];
                    sk->has_connect_blocked = 1;
                }
            }
            connect_done: ;

        } else if (label != 0) {
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

    }
}
