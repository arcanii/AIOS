# NEXT: netconsole RX throughput -- profiling result + real levers (2026-06-12)

Goal was to speed up the push (Mac -> Pi `__put`) receive path (~55KB/s,
1.5MB kernel ~= 24-30s). v0.4.226 coalesced the window-reopen ACKs; that was
a correct change but a THROUGHPUT NO-OP. This doc records what was measured
and where the real wins are, so the next attempt starts from facts.

## What was tried (v0.4.226) and the result

- net_server NET_RECVFROM was sending a TCP window-update ACK on EVERY 900B
  read (~1740 ACK TX per 1.5MB). Coalesced to fire only every >=8KB drained
  (or ring-empty). Confirmed 9x fewer ACKs (/proc/netstat tcp_read_acks: 197
  vs ~1740). Correct under injected loss (netrx_qemu_test B1/B2 byte-perfect).
- HW throughput UNCHANGED: ~24-30s/1.5MB before and after. So ACK TX was not
  the bottleneck. (Kept the change anyway -- 9x less TX is real load relief,
  and standard TCP behaviour -- but it does not address the goal.)

## Where the time actually goes (measured/reasoned)

- QEMU pushes the same 1.5MB in ~3s (520KB/s); HW in ~24s (55KB/s). Same IPC
  count both places -> the ~9x gap is HW per-operation latency, not algorithm.
- Per 1.5MB push: ~1740 NET_RECVFROM IPCs (900B cap each, the seL4 message-
  register limit) + ~3900 FS_PWRITE IPCs (libc splits writes at 800B). ~5600
  IPC round-trips, EACH a context switch among netconsole <-> net_server <->
  fs_thread, ALL serialized on core 0 (the Source-B single-core interim,
  KernelMaxNumNodes=1). ~24s / ~5600 ~= 4ms/IPC incl ext2 + scheduling.
- /dev/null-vs-/tmp push comparison was inconclusive: dominated by the 32.4s
  stall quanta (a /dev/null run took 191s = ~6 quanta; another push failed at
  36s, another at 68s = 2 quanta). The quanta both FAIL pushes (netconsole's
  10s TICKS_STALL drops the connection) and inflate timings -- they dominate
  the HW user experience far more than steady-state KB/s.

## Real levers (all bigger than an afternoon; pick deliberately)

1. **SHM socket recv path** -- NET_RECVFROM_SHM / NET_MAP_SHM are DEFINED in
   root_shared.h (101/102) but UNIMPLEMENTED. A mapped-page bulk recv (4KB+
   per IPC instead of 900B) would cut recv IPCs ~5x. Pairs with a bulk file
   write (mmap or a big-chunk FS verb) to cut the FS_PWRITE IPCs too. Biggest
   structural win; moderate project (mirror the pipe SHM machinery).
2. **Restore SMP=4** -- single-core serializes every IPC context switch.
   Blocked on Source B (KernelMaxNumNodes=1 interim). When SMP returns,
   netconsole/net_server/fs_thread can overlap.
3. **Fix Source B** -- the stall quanta are the dominant HW pain; they make
   any throughput number unreliable and fail ~1/3 of pushes outright. This is
   the highest-value HW correctness item regardless of throughput (see
   NEXT_20260612_vl805_dma_stall.md; the keyboardless-quanta finding reopened
   it).

## Recommendation

Don't chase RX throughput further in isolation -- the ACK result shows the
bottleneck is IPC count x single-core, and the quanta dominate the lived
experience. The ordered payoff is: (a) Source B (correctness + unblocks SMP),
(b) SMP=4, (c) SHM socket path. pi_flash's pull-back+retry already makes
deploys reliable despite the quanta, so this is not urgent.
