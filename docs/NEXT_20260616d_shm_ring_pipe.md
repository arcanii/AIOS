# NEXT (seed): SHM-ring pipes -- let a pipeline span cores

Self-contained handoff for a FRESH session. Repo `~/Desktop/github_repos/AIOS`, branch `main`.
Read `HANDOVER.md` (top) + `MEMORY.md` + this. Companion: `docs/NEXT_20260616c_smp_tlb_stall_fix.md`
(the SMP arc), `[[project_stall_hunt]]`, and `BACKLOG.md` ("SMP re-architecture" entry).

## Where we are (end of the v0.4.257 SMP session)

LOCAL commits ahead of origin/main (Bryan pushes; he already pushed 9cc6fe4 + 32dbc39):
```
4903fb9 pipe: SHM-write coalescing (groundwork for SHM-ring pipes) + multikernel backlog
06e0edd smp: Stage S -- opt-in per-process core distribution + fastpath residency hook
32dbc39 kernel: per-ASID residency-masked TLB shootdown -- fix the RPi4 remote-TLBI stall   [PUSHED]
9cc6fe4 usb: BOT/SCSI bulk-STALL recovery (Stage 5) + churn fragility                        [PUSHED]
```
- **Pi runs v0.4.257 build 2523** (= Stage S kernel) at 192.168.0.8, `coresched` default OFF.
  Build 2523 does NOT have the pipe-write coalescing (4903fb9) -- that's committed but UNFLASHED.
- Kernel changes (TLB fix + fastpath hook) live in the sibling seL4 working tree (`deps/kernel`)
  + captured in `deps/patches/seL4-kernel.patch`.

## The problem this solves

seL4's big kernel lock caps total kernel throughput at ~1 core's worth of syscalls/sec (the
fastpath itself takes the BKL). AIOS pipes route data client->`pipe_server`(core 0)->reader: a
`seL4_Call` PER CHUNK, serialized on core 0. So when work distributes to cores 1-3 (Stage S),
IPC-bound pipelines REGRESS (ceiling 30->6) -- the cores contend on the BKL + the core-0 server
instead of parallelizing. The ONLY way IPC-bound pipelines scale is to take the kernel OUT of the
per-chunk data path: a direct producer<->consumer shared-memory ring, syscall only on block/wake.

## The design: direct SPSC shared-memory ring per pipe

Make a pipe a **single-producer single-consumer lock-free ring** that the writer AND reader both
map (the same frame, in their own vspaces). Data flows in USERSPACE on the producer/consumer
cores; `pipe_server` is touched only to (a) set up the ring at pipe creation and (b) sleep/wake a
blocked end.

Ring layout (one shared page, or a few): a header (`head` reader-owned index, `tail` writer-owned
index, `writer_closed` flag, `reader_waiting`/`writer_waiting` flags) + the data buffer.
- **Writer**: copy bytes into `ring[tail % SZ]`, advance `tail` with a RELEASE barrier (`dmb ish`).
  If full (`tail - head == SZ`): set `writer_waiting`, `seL4_Call(pipe_ep, PIPE_BLOCK_W)` -> server
  SaveCaller-parks it until the reader signals.
- **Reader**: if empty (`head == tail`): if `writer_closed` -> EOF; else set `reader_waiting`,
  `seL4_Call(pipe_ep, PIPE_BLOCK_R)` -> parked until the writer signals. Else read `ring[head]`,
  advance `head` with a release barrier.
- **Wakeup**: after a writer advances `tail`, if `reader_waiting` it `seL4_Call`s (or signals) the
  server to wake the parked reader (reuse the existing `pipe_read_blocked[]` + `wake_one_blocked_
  reader` + `seL4_Send` machinery). Mirror for writer-full.
- The blocking path is the ONLY kernel traffic, and only at empty/full transitions -- a steadily-
  flowing pipe does ~zero syscalls for the data. cat (core 1) + wc (core 2) then truly parallelize.

## The HARD parts (read before coding)

1. **A72 cross-core coherency (the #1 risk; QEMU CANNOT validate it).** With Stage S, writer and
   reader are on DIFFERENT cores. The shared ring must be mapped CACHEABLE INNER-SHAREABLE on both
   ends (the existing pipe xfer pattern is "single-core PIPT A72 coherent" -- verify it's actually
   inner-shareable for cross-core). The `head`/`tail` index updates NEED release/acquire barriers
   (`dmb ish`) or the consumer sees a stale index / reads uninitialized data. This is the documented
   all-NUL bug class ([[feedback_pipe_shm_cache]]) and QEMU does NOT model it -- budget HW iteration
   (flash + `cat bigfile | wc -c` with the two stages pinned to different cores via `/proc/coresched.1`).
2. **Backpressure + EOF + signal races**: the writer must publish `tail` (release) BEFORE checking
   `reader_waiting`; the reader must set `reader_waiting` BEFORE re-checking `head == tail` (else a
   lost-wakeup: writer publishes between the reader's empty-check and its park). Classic condvar
   ordering -- get it provably right.
3. **Teardown**: writer/reader exit must mark `writer_closed`/`reader_closed`, wake the other end,
   and free the ring (mirror the existing `xfer` cleanup + cap-copy tracking in pipe_server.c ~160).
4. **Multiple readers/writers** (`(a;b) | c`, tee): SPSC assumption breaks. Keep SPSC; fall back to
   the current server-mediated path for the multi-end case.

## What's already in place (groundwork, v0.4.257 / commit 4903fb9)

- `pipe_t` (include/aios/root_shared.h) has a SEPARATE write-direction xfer page
  (`xfer_frame_w`/`xfer_buf_w`/`xfer_valid_w`/`xfer_copies_w`).
- `PIPE_MAP_SHM` (src/servers/pipe_server.c ~1712) is parameterized by direction (MR1: 0=read,
  1=write) + maps the frame CACHEABLE into the child vspace + tracks the cap copy for cleanup.
- `PIPE_WRITE_SHM` (~1789) copies the write xfer -> ring, returns the accepted count (backpressure).
- posix_file.c: `pipe_request_shm` (read map, dir=0) + `stdout_request_wshm` (write map, dir=1) +
  the stdout write loop uses 4KB SHM with bounded yield-retry on a full ring.
- This is server-mediated (a Call per 4KB chunk). The SHM-ring removes the server from the per-chunk
  path entirely -- reuse the frame-map + cap-tracking + cleanup patterns, add the SPSC indices +
  the block/wake protocol + the cross-core barriers.

## Plan + verification

1. Add the ring header + map BOTH ends at PIPE_CREATE (or lazily). Reuse the dir-parameterized map.
2. Implement the userspace SPSC read/write in posix_file.c (barriers!) with PIPE_BLOCK_R/W syscalls
   only at empty/full.
3. Implement PIPE_BLOCK_R/W + wake in pipe_server.c (reuse `pipe_read_blocked[]` + `seL4_Send`).
4. Teardown + EOF + lost-wakeup correctness.
5. QEMU: `smp_qemu_test` (pipe-storm data EXACT, ceiling >=30 -- correctness/no-regress) +
   `pipe_throughput_qemu.py`. QEMU validates LOGIC, NOT coherency.
6. HW: flash build-rpi4-netd, pin a producer + consumer to different cores (`/proc/coresched.1`),
   run `cat bigfile | wc -c` and a parallel-pipeline ceiling -- expect the ceiling to RISE above
   the Stage-S-ON regression (6) toward/above 30, and a real cross-core throughput win. Drive HW
   from SERIAL (`/dev/cu.usbserial-0001`, lossy -- short probes); netconsole wedges under churn.
7. Honest exit: even with SHM-ring data, SPAWN stays BKL-bound (the ceiling is partly spawn-bound)
   -- the full fix is the multikernel re-arch (BACKLOG.md). SHM-ring handles the DATA-FLOW half.

## Tooling (reusable, this session)
`scripts/netstall.py` (teardown-after-idle stall probe), `scripts/serwatch.py` (lossy mini-UART
capture), `scripts/coresched_speedup_qemu_test.py` (CPU-bound distribution A/B),
`scripts/pipe_throughput_qemu.py` (single-stream pipe throughput). `/proc/coresched.1` = distribute
procs to cores 1-3; `/proc/corewarm` = idle-core warmer. `python3 scripts/pi_flash.py --host <ip>`
= flash-over-network (3-way sha). Pre-flash: full QEMU gate (BACKLOG "deeper pre-flash smoke").
