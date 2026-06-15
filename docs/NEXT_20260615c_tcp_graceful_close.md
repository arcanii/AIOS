# NEXT: TCP graceful close + tail retransmit (fix the SSH last-command drain race)

Root-caused 2026-06-15 (BACKLOG.md "PTY/SSH drain item", commit 295999c). The
last-command-before-exit output loss (~35% of short SSH sessions on HW; also scp
rc=1) is **AIOS TCP has no sender-side retransmission**, NOT the pipe-SHM coherency
the old doc guessed. This is the implementation spec for the fix.

## The bug (confirmed by code trace)

- `struct net_socket` (net_server.c:44) has `snd_nxt` but NO `snd_una`. The incoming
  ACK number (`ack_val`, already passed to `net_tcp_deliver`, net_server.c:211) is
  NEVER used to advance send-side state -- AIOS cannot tell what the peer has ACKed.
- `net_tcp_send` (net/net_tcp.c:42) is fire-and-forget: blast the segment, no copy
  kept, no RTO timer.
- `NET_CLOSE_SOCK` (net_server.c:830) sends FIN -> `TCP_FIN_WAIT`; `net_tcp_deliver`
  (net_server.c:470) frees the socket (`in_use=0`) on the NEXT ACK -- WITHOUT checking
  that our data/FIN was ACKed.
- So the last pre-close segment, if lost on the wire or dropped by a momentarily-full
  client window, is never resent: client gets the FIN, no data. A72-only (SLIRP
  loopback is lossless + instant); QEMU CANNOT reproduce the loss path.
- Every "retransmit" path already in AIOS is the RECEIVE side (relying on the PEER to
  resend to us). There is zero sender-side retransmission.

## Design

### Data structures (net_server.c `struct net_socket`)

    uint32_t snd_una;            /* oldest unACKed seq (== snd_nxt when all ACKed)   */
    uint32_t tx_buf_end;         /* seq one past the last BUFFERED byte (<= snd_nxt)  */
    uint8_t  txbuf[TX_RTX_BUF];  /* unACKed bytes, indexed txbuf[seq % TX_RTX_BUF]    */
    uint8_t  tx_broken;          /* a send overflowed the buffer -> stop buffering    */
    uint8_t  fin_sent;           /* we sent a FIN (graceful close in progress)        */
    uint32_t fin_seq;            /* the seq our FIN consumed (snd_nxt at FIN time)     */
    uint8_t  closing;            /* app called close(); free once fully ACKed         */
    uint8_t  rtx_count;          /* consecutive RTO retransmits (force-close cap)      */
    uint64_t rtx_due_ms;         /* next RTO fire (0 = disarmed)                       */

Constants: `TX_RTX_BUF 4096` (4KB*8 socks = 32KB; sshd unACKed is always < this),
`TCP_RTO_MS 1000`, `TCP_RTX_MAX 8`. Time: `cntpct_el0`/`cntfrq_el0` (copy
`read_cntpct`/`read_cntfrq` from net/net_dhcp.c:83) -> `net_now_ms()`.

**Invariant kept sound:** `txbuf` always holds the CONTIGUOUS prefix
`[snd_una, tx_buf_end)`; we only ever retransmit that range. `tx_buf_end <= snd_nxt`.
Bytes in `[tx_buf_end, snd_nxt)` (only after an overflow) are NOT retransmittable.

### Init (every place a TCP socket reaches a known send seq)

Set `snd_una = tx_buf_end = snd_nxt`, and zero `tx_broken/fin_sent/closing/rtx_count/
rtx_due_ms`, at: the accept ESTAB (net_server.c ~265/306), the connect ESTAB (~325),
and socket alloc. SAFEST: a one-line `net_sock_tx_init(sk)` helper called at each.
MISSING AN INIT SITE = garbage retransmit = corruption -- audit all in_use=1 sites.

### NET_SENDTO (net_server.c:710, TCP ESTAB branch)

    uint32_t unacked = sk->snd_nxt - sk->snd_una;
    if (!sk->tx_broken && unacked + (uint32_t)len <= TX_RTX_BUF) {
        for (int i = 0; i < len; i++)
            sk->txbuf[(sk->snd_nxt + i) % TX_RTX_BUF] = pdata[i];
        sk->tx_buf_end = sk->snd_nxt + (uint32_t)len;
    } else {
        sk->tx_broken = 1;   /* contiguous-prefix invariant: stop extending */
    }
    net_tcp_send(..., sk->snd_nxt, sk->rcv_nxt, TCP_ACK|TCP_PSH, pdata, len);
    sk->snd_nxt += (uint32_t)len;
    if (sk->rtx_due_ms == 0) sk->rtx_due_ms = net_now_ms() + TCP_RTO_MS;
    rc = len;   /* still always full -- NO send-API change (see tradeoff) */

NOTE the deliberate interim tradeoff: we DO NOT add backpressure (that would require
changing the send API + every socket writer -- `sock_write_all` treats <=0 as fatal).
sshd never overflows 4KB unACKed, so it is fully covered. A bulk sender that overflows
sets `tx_broken` -> its tail is not retransmittable (acceptable: netconsole/fatswap
have app-level sha verification). The FULL fix (flow control + unbounded retransmit)
is a follow-up that also touches sock_write_all.

### net_tcp_deliver -- consume the incoming ACK (net_server.c, early, for any ESTAB/FIN_WAIT)

    /* advance snd_una; SEQ-wrap-safe compare */
    if ((flags & TCP_ACK) && (int32_t)(ack_val - sk->snd_una) > 0
                          && (int32_t)(ack_val - sk->snd_nxt) <= 0) {
        sk->snd_una = ack_val;
        if ((int32_t)(sk->tx_buf_end - sk->snd_una) < 0) sk->tx_buf_end = sk->snd_una;
        if (sk->snd_una == sk->snd_nxt) { sk->rtx_due_ms = 0; sk->rtx_count = 0; }
        else sk->rtx_due_ms = net_now_ms() + TCP_RTO_MS;  /* restart timer on progress */
    }

REWORK the FIN_WAIT->CLOSED (net_server.c:470): free ONLY when our FIN is ACKed, i.e.
`closing && fin_sent && (int32_t)(sk->snd_una - (sk->fin_seq + 1)) >= 0`. Until then
keep `in_use=1` so the RTO can resend.

### NET_CLOSE_SOCK (net_server.c:830) -- graceful

Send FIN as today, `snd_nxt++`, `state=TCP_FIN_WAIT`, AND set `closing=1; fin_sent=1;
fin_seq = snd_nxt-1; rtx_due_ms = net_now_ms()+TCP_RTO_MS;`. DO NOT free in_use.
Reply to the caller immediately (close() returns at once; teardown finishes async).
Common case: the peer ACKs the data+FIN within an RTT (the loop wakes on RX) -> freed
fast. Loss case: RTO resends. (Keep NET_CLEANUP_PID op-98 abrupt -- a reaped owner
cannot wait.)

### RTO check -- new `net_tcp_rto_check()` called in the main loop (net_server.c:~575, beside net_dhcp_renew_check)

    uint64_t now = net_now_ms();
    for each in_use TCP socket sk with rtx_due_ms && now >= rtx_due_ms:
        if (sk->rtx_count >= TCP_RTX_MAX) {            /* give up -> force close */
            sk->in_use = 0; sk->state = TCP_CLOSED; continue;
        }
        /* resend the buffered unACKed prefix [snd_una, tx_buf_end) in <=900B chunks */
        uint32_t s = sk->snd_una;
        while ((int32_t)(sk->tx_buf_end - s) > 0) {
            int n = (int)(sk->tx_buf_end - s); if (n > 900) n = 900;
            uint8_t tmp[900];
            for (int i=0;i<n;i++) tmp[i] = sk->txbuf[(s+i) % TX_RTX_BUF];
            net_tcp_send(sk->remote_ip, sk->remote_mac, sk->local_port,
                         sk->remote_port, s, sk->rcv_nxt, TCP_ACK|TCP_PSH, tmp, n);
            s += (uint32_t)n;
        }
        if (sk->fin_sent && (int32_t)(sk->snd_una - (sk->fin_seq+1)) < 0)
            net_tcp_send(... sk->fin_seq, sk->rcv_nxt, TCP_ACK|TCP_FIN, NULL, 0);
        sk->rtx_count++;
        sk->rtx_due_ms = now + TCP_RTO_MS;      /* (optional: * (1<<rtx_count) backoff) */

RTO granularity caveat: when the link is idle the net_server loop wakes only on the
serverstats kick (PROBE_PERIOD_SEC=5, serverstats.c:38) -> worst-case a lost segment
is resent in ~5s, not 1s. Fine for correctness (data arrives) but slow. If snappier
recovery matters, have serverstats kick netd more often while any socket has
`rtx_due_ms != 0`, or arm a dedicated timer ntfn. During an ACTIVE connection the loop
wakes on every RX/ACK, so the common path is fast; only true loss waits for the kick.

## Gating

- Build: `ninja -C build-04 && build-netd && build-rpi4-netd && build-rpi4`.
- QEMU NO-REGRESSION (the loss path cannot be exercised on lossless SLIRP):
  `net_socket_qemu_test.py` 8/8 (flag-ON + flag-OFF), `ssh_qemu_test.py` 6/6,
  `netd_qemu_test.py` 10/10, plus a reconnect run. These prove the new buffering +
  graceful close do not break the normal (no-loss) path.
- HW (the REAL test -- deploy sshd+kernel over net, no flash if userspace-only; here
  net_server is ROOT/netd -> a kernel flash via pi_flash.py): run a short-session loop
  (`ssh root@pi 'echo hi'` x40, count output-loss) before vs after; expect the ~35% ->
  ~0. Also re-verify scp rc (should become 0), netconsole push throughput unchanged,
  and a soak (no socket-slot leak from lingering closes -- watch /proc/net occupancy).

## Risks

- Init-site coverage (garbage retransmit if a TCP socket reaches ESTAB without
  tx-init) -- audit every in_use=1 / state=TCP_ESTAB site; prefer a single helper.
- Socket-slot lingering: a close now holds the slot until ACKed (max ~TCP_RTX_MAX*RTO,
  or 5s*8 idle). With only 8 slots, a burst of closes to a dead peer could starve
  accept briefly -> the TCP_RTX_MAX force-close caps it. Verify under the ssh
  reconnect storm.
- This is the net path EVERYTHING rides (sshd, netconsole push, fatswap deploy, the
  socket suite). A bug here bricks remote access. Adversarial-review the diff
  (READ-ONLY agents -- see feedback_review_subagent_writes) before the flash.

## STATUS 2026-06-15: data-retransmit core IMPLEMENTED + QEMU-green; close/FIN lifecycle NEEDS WORK

Implemented in net_server.c (UNCOMMITTED, no version bump per the 4a-B pattern): the
struct fields, net_sock_tx_init at both ESTAB transitions, the ACK-consume (snd_una),
NET_SENDTO buffering, NET_CLOSE_SOCK deferred close, net_tcp_maybe_send_fin, and
net_tcp_rto_check in the main loop. Builds (4 trees) + QEMU NO-REGRESSION GREEN:
socket 8/8 (ON+OFF), ssh 6/6, ssh-reconnect 6/6, netd 10/10. (QEMU is lossless so the
retransmit itself is NOT exercised -- HW-only.)

A READ-ONLY adversarial review (3 lenses) verdict: data-retransmit core SOUND; the
close/FIN lifecycle is UNSOUND (fix_first). The simplification of dropping fin_sent/
fin_seq (this doc's lines 32-33) is the root of the gaps. REQUIRED before HW:

1. **FIN retransmit (CRITICAL).** maybe_send_fin zeros rtx_due_ms and rto_check bails on
   `state != ESTAB`, so a lost FIN is never resent -> socket stuck in FIN_WAIT. ADD
   `fin_sent` + `fin_seq`; keep rtx armed after sending the FIN; in rto_check, when
   `fin_sent && snd_una <= fin_seq`, resend the FIN (at fin_seq) until ACKed; force-close
   after the cap.
2. **Passive-FIN-during-deferred-close (CRITICAL).** If the peer FINs while we are
   `closing` with a PARTIAL ACK, the existing rx-FIN handler sets state=FIN_WAIT, after
   which our data retransmit (rto_check gated on ESTAB) and our FIN (maybe_send_fin gated
   on ESTAB) both stop -> data + FIN never delivered. FIX: gate the data retransmit on
   `closing && !fin_sent && unACKed` (NOT on state) so it continues through a passive
   FIN_WAIT; let maybe_send_fin work in FIN_WAIT too (drop its `state==ESTAB` guard).
3. **Rework the existing "FIN_WAIT + any ACK -> CLOSED + free" (net_server.c ~546).** It
   frees on ANY ACK, which (a) can free before our FIN is ACKed and (b) fights the FIN
   retransmit. Change to free only when our FIN is ACKed: `fin_sent && (int32_t)(snd_una
   - (fin_seq+1)) >= 0`. (Keep the passive-only case -- peer closed, we never sent a FIN
   -- on its existing path, or send our FIN via the app close.)
4. **Slot orphaning (HIGH).** A deferred close to a silent peer holds a slot up to
   ~TCP_RTX_MAX*5s. ADD an ABSOLUTE deadline (e.g. free a `closing` socket after ~10s
   regardless), and send a RST on force-close so the peer drops it.
5. **tx_broken clear (MEDIUM).** Clear `tx_broken` when `snd_una == snd_nxt` (buffer
   empty, no gap) so a bulk socket can buffer/retransmit again after a burst drains.
6. **NET_SOCKET alloc init (LOW, defense-in-depth).** Zero the tx fields at socket
   alloc (~726-739), not only at ESTAB, so a future ESTAB path that forgets tx_init
   cannot read stale snd_una.

These all touch the delicate FIN_WAIT path -> do them as a FOCUSED unit with the HW
deploy-verify loop ready (re-gate the full QEMU suite + re-review, then flash + the
short-session loss-rate repro). The uncommitted data-retransmit core can stay in the
tree as the starting point, or be reverted and reapplied from this spec.
