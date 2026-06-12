# NEXT: inbound TCP corruption on large netconsole pushes -- 2026-06-12

Found (and contained) during the fatswap HW arc: ~50% of 1.5MB netconsole
`__put` pushes land on disk with a SHORT corrupt run; the rest are
byte-perfect. The fatswap deploy flow is SAFE against it (pull-back compare +
retry in scripts/pi_flash.py, three-way sha before any reboot), so this is a
correctness hunt, not a deploy blocker.

## Evidence (v0.4.224 board, 2026-06-12 evening; scripts in this doc's session)

- 4-push forensic run (distinct paths /tmp/ex1..4.bin, 1,564,368 bytes each):
  ex1 CORRUPT (112 bytes of 0xFF at +0xf0800), ex2 OK, ex3 OK, ex4 CORRUPT
  (6 bytes ff ff ff ff ff 7f at +0x174000). Both corruptions STABLE on
  double pull (truly on disk, not a read flake).
- All observed corrupt runs start BLOCK-ALIGNED (mod 1024 = 0) and are
  short (6 / 112 / 124 bytes), 0xFF-biased; the rest of the block is
  correct. The 124-byte case (v0.4.223 deploy) and the 112-byte case hit
  the SAME file offset 0xf0800 in DIFFERENT files on different days.
- The block layer is EXONERATED: /proc/cachestats emmc_timeout_retries=0,
  emmc_timeout_fails=0 across corrupted pushes (v0.4.224 counters), and a
  LOCAL `cp` of a verified-clean 1.5MB file through the identical
  FS_PWRITE -> ext2 RMW -> blk_cache -> eMMC path is byte-identical.
- The OUTBOUND path is exonerated too: `__get` pulls (Pi -> Mac, assembly
  on macOS) are consistently correct, including double-pulls of the same
  corrupt files. Asymmetry => the INBOUND assembly on AIOS is the suspect:
  GENET rx -> net_rx_ring -> net_server TCP reassembly -> RECVFROM_SHM ->
  netconsole recv()/write() loop.
- Corruptions correlate with the 32.4s stall quanta (see
  NEXT_20260612_vl805_dma_stall.md): quanta freeze the consumer, the 32KB
  rx window fills, segments drop, retransmission storms follow. Clean
  pushes (17-21s, no quantum) never corrupted; pushes that rode quanta
  sometimes did. But note the eMMC-style "fall-through" class is ruled
  out -- this needs loss+retransmit, not just a frozen CPU.

## Why the on-disk pattern is block-aligned (interpretation, unverified)

netconsole __put writes the socket stream in <=900B chunks via FS_PWRITE;
ext2_pwrite_file does a read-modify-write per 1024B block. A corrupt run
confined to the HEAD of a block with a correct tail is the signature of a
chunk boundary: bytes delivered for [head..] were wrong/stale when an
earlier chunk wrote them, then a later chunk correctly wrote [boundary..].
I.e. the stream delivered DIFFERENT bytes for the same stream positions at
different times -- exactly what a TCP resequencing/overlap bug produces
(each retransmitted segment is internally valid; the SPLICE is wrong).
The 0xFF bias suggests stale/uninitialized buffer reuse (rx ring slot or
SHM transfer page), not wire damage. The recurring +0xf0800 offset is the
strongest clue: something about stream position ~984KB (mod 32KB rx ring =
~0x800? window/sequence wrap interaction?) repeats across runs.

## Hunt plan

1. Reproduce DETERMINISTICALLY without quanta: a QEMU harness that drops /
   delays/duplicates inbound segments (SLIRP cannot; use a host-side proxy
   that mangles the stream, or add a debug drop-Nth-packet knob in
   net_driver RX). The 32KB-window + retransmit interleave is the trigger.
2. Audit src/net/net_tcp.c inbound resequencing: out-of-order handling,
   OVERLAPPING retransmit splicing (seq < rcv_nxt with seq+len > rcv_nxt),
   the post-v0.4.86 "buffer excess TCP data after waking blocked reader"
   path (feedback_tcp_partial_read), and the circular RX buffer wrap math.
   Prime suspect: a partial-overlap segment trimmed wrongly, or accepted
   bytes copied from a STALE offset into the circular buffer.
3. Check NET_RECVFROM_SHM copy bounds + the rx ring slot lifecycle under
   overflow (RXp-RXc gaps of 16+ observed during quanta).
4. Instrument cheaply (counters like the v0.4.224 emmc ones, surfaced in
   /proc/cachestats or a /proc/net): ooo segments, overlap trims, ring
   overflows, resequence-buffer hits. Counter deltas across a corrupt vs
   clean push localize the path fast.
5. Keep using the pull-back verify in any transfer tooling until fixed
   (NEVER trust a bare `__put ok` for multi-MB files).

## Workarounds already shipped

- scripts/pi_flash.py: __get pull-back compare + reconnect-and-retry per
  push attempt (quantum-sized timeouts).
- fatswap itself re-hashes its source AND its on-card readback; pi_flash
  compares both against the local sha before any reboot. A corrupt push
  can never reach the boot partition.

---

## RESOLVED 2026-06-12 (v0.4.225): NOT the network -- ext2 builder off-by-one

The inbound TCP path was INNOCENT. Added inbound checksum verification + a
deterministic drop-every-Nth fault knob + /proc/netstat counters; under
forced retransmission storms the stream stayed byte-perfect (netrx_qemu_test).
A 0xFF-FREE payload still corrupted with 0xFF on disk, and the rx counters
showed ZERO 0xFF ever stored/read on the socket (dbg_ff_store_off/read_off=0,
store_bytes==read_bytes). Pi-side sha == pulled sha != source -> on-disk write
corruption. An ext2_pwrite probe showed the data block written CORRECTLY then
clobbered -- and the clobbered physical block was a group's BLOCK BITMAP.

Root cause: scripts/ext2/builder.py laid out block group g>0 at `g*bpg`, but
the kernel allocator uses `first_data_block + g*bpg`. The off-by-one made the
kernel's last-block-of-group-g alias group g+1's bitmap; the allocator handed
the bitmap block out as file data and later bitmap writes overwrote the file.
Fixed builder to standard layout + padding reservation; netrx_repro 0/16.
The checksum verify + counters + acquire barrier were kept as hardening.
