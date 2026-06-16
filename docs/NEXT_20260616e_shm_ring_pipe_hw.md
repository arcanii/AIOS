# NEXT (seed): SHM-ring pipes — DONE on QEMU, needs HW cross-core verify (v0.4.258)

Self-contained handoff for a FRESH session. Repo `~/Desktop/github_repos/AIOS`, branch `main`.
Read `HANDOVER.md` (top) + `MEMORY.md` + this. Companion (the original task seed):
`docs/NEXT_20260616d_shm_ring_pipe.md`. Predecessor SMP arc:
`docs/NEXT_20260616c_smp_tlb_stall_fix.md`, `[[project_stall_hunt]]`, BACKLOG "SMP re-architecture".

## TL;DR

The **direct SPSC SHM-ring pipe** is IMPLEMENTED + QEMU-VERIFIED (26/26 logic, full gate green) +
ADVERSARIALLY REVIEWED (6 real bugs found & fixed, re-gated green, 0 remaining — see the review
section below; the disputed data-path barriers were independently re-adjudicated as CORRECT).
It takes the kernel out of the per-chunk pipe data path so a `cmd | cmd` pipeline can move bytes in
USERSPACE on the producer/consumer cores. Ships **DEFAULT OFF** behind `/proc/shmring` (the
cross-core A72 coherency path needs real-hardware soak — QEMU cannot model it). **COMMITTED `b113844`
(+ HANDOVER `8ad0dec`); kernel FLASHED to the Pi (v0.4.258 build 2573) + HW-verified boot.** The
remaining work is the cross-core COHERENCY validation — and the ROOT CAUSE of why the direct ring
"never engaged" is now FOUND + LOCAL (not netd, not A72): **the ring fast path was wired only into
`read()`/`write()`, NOT into the stdio backend (`aios_stdio_write`) or `writev`/`readv` — which is how
real filter tools (`seq|wc`) do pipe I/O.** So every stdio pipeline bypassed the ring (the `map_ok=33`
seen in QEMU was the netconsole RELAY, which uses raw read/write). The fix (ring-ify those 3 paths) is
designed + prototyped; it engages the ring but EXPOSES a direct-reader premature-EOF bug — see
**`## HW BRING-UP 2026-06-16b`** below for the full write-up, the fix, and the precise next step. The
fix is currently REVERTED (main green, `shmring` 26/26). The server-mediated ring path IS HW-verified
data-exact.

## HW BRING-UP 2026-06-16b — kernel HW-VERIFIED; server-ring HW-VERIFIED; DIRECT path OPEN

**Flashed + boots clean.** `build-rpi4-netd` → `mkkernel8 --kernel build-rpi4-netd/images/aios_root-image-arm-bcm2711`
(point it at the netd-ON image — `mkkernel8` DEFAULTS to the netd-OFF `build-rpi4`, which would regress the
Pi's net) → `pi_flash.py --host 192.168.0.8`. 3-way sha OK; `/proc/version` = **AIOS v0.4.258 (build 2573),
4-core SMP**. Non-regressive (net up, SHM-ring default-OFF). The never-before-flashed pipe coalescing (4903fb9)
is now also HW-proven to boot.

**Server-mediated ring path HW-VERIFIED (data EXACT).** Pushed ring-aware `seq`/`wc` to `/tmp` (`pi_filexfer
push`; NOT `/bin` — no clobber, recoverable), armed `/proc/shmring.1` + `/proc/coresched.1`, ran
`seq 1 100000 | wc -l`==100000 and `wc -c`==588895 across OFF / ON / cross-core (**9/9 exact**, board healthy).
BUT `/proc/shmring` showed **`map_ok=0`, `push`/`pull`≈5.3M** — ALL bytes went through the core-0 server
(`ring_server_push/pull`). So the DIRECT userspace ring — and thus **the #1 cross-core COHERENCY risk — was
NOT exercised** (only core 0 ever touched the ring frame).

**ROOT CAUSE FOUND (2026-06-16c, fully LOCAL/QEMU — supersedes the earlier "netd-ON vs A72" guess).**
The direct ring never engaging was NEVER a netd or A72 problem. **The SHM-ring fast path was wired only into
`aios_sys_read`/`aios_sys_write` (the raw read()/write() syscalls), but NOT into the two paths real filter
tools actually use:** (1) the stdio backend `aios_stdio_write` (aios_posix.c:214 — every `printf`/`fputs` to a
piped stdout) and (2) `aios_sys_writev`/`aios_sys_readv` (how musl stdio flushes/fills its buffer). Both send
`PIPE_WRITE`(61)/`PIPE_READ`(62) straight to the server (→ `ring_server_push/pull`, the `push`/`pull` counters)
**without ever calling `pipe_map_ring`**. So `seq | wc` (and ~every stdio pipeline) bypasses the ring entirely
→ `map_ok=0`. The `map_ok=33` seen over netconsole was the **netconsole relay's** pipe (the relay loop uses raw
read()/write(), the only ring-mapping caller). Proven LOCALLY: drive `seq|wc` over the **serial** console
(no relay) and `map_ok=0` on BOTH `build-04` (netd-OFF) and `build-netd` (netd-ON) — identical to HW. The Pi
was behaving correctly and consistently with QEMU all along.

- Repro tool: `scripts/shmring_netd_isolate.py` (boots a tree over the QEMU serial console à la
  `netd_qemu_test`, arms `/proc/shmring.1`, runs `seq|wc`, reads `/proc/shmring`). `AIOS_ISO_KERNEL=<image>`
  selects the tree. build-04 serial → `map_ok=0`; build-04 netconsole → `map_ok=33` (relay).

**THE FIX (designed + prototyped this session, then REVERTED to keep main green — see below).** Ring-ify the
3 missing paths, mirroring the proven `aios_sys_read`/`write` ring logic:
1. `aios_stdio_write` (aios_posix.c, the `stdout_pipe_id>=0` branch): try the ring first. Needs a non-static
   entry (the ring helpers are `static` in posix_file.c) — add `int aios_stdout_ring_write(const char*, size_t,
   long*)` in posix_file.c + declare in posix_internal.h, call it before the legacy `PIPE_WRITE` loop.
2. `aios_sys_writev` fd-pipe branch (posix_file.c): `pipe_map_ring(f->pipe_id,1)` + `shm_ring_send` per iov.
3. `aios_sys_readv` fd-0 (`stdin_ring_get`) and fd-pipe (`pipe_map_ring(f->pipe_id,0)`) branches: `shm_ring_recv_fast`
   per iov, fall to `PIPE_READ` on `-1` (empty+writer-live), `break` on `0` (EOF).
(The fd-1/2 `writev` branch routes through `aios_stdio_write`, so fix #1 covers it automatically.)

**THE FIX WORKS to engage the ring but EXPOSES A REAL DIRECT-READER BUG (the actual remaining work).** With
the fix, `RINGLBL=91` fires for BOTH seq and wc (the map now happens) — but `seq 1 10 | wc -l` returns **0**
(premature EOF) and larger sizes HANG. A server-side probe in `PIPE_MAP_RING` proved the frame the reader maps
is correct + populated: when wc maps (dir 0) `tail=21 head=0 wclosed=1 data=31 0a 32 0a 33 0a` (= `"1\n2\n3\n"`),
same physical frame (`cv` identical). Yet wc's `shm_ring_recv_fast` returns 0/EOF as if `avail==0`. So the
DIRECT-READER path returns premature EOF despite the populated, coherent frame. This path was **never exercised
before** — the 26/26 `shmring_qemu_test` ran seq|wc over the SERVER-mediated route (relay only used the direct
read/write), so the bug hid. It is invisible server-side (direct reads don't IPC), so it needs **client-side
instrumentation** (no serial-debug primitive exists in libaios yet — add one, or a per-fd debug counter, or
temporarily force `recv_fast` to return -1 to confirm the server-fallback then works). Suspects: the reader's
first `recv_fast` seeing a stale `head==tail` in its own mapping (cacheability/TLB on the freshly-mapped frame,
or `head` already advanced), or an off-by-one in the readv EOF handling. **This is the precise next step.**

**STATUS: the fix is REVERTED — main is green (`shmring_qemu_test` 26/26).** Committing the fix as-is would
break the armed-ring suite (seq|wc → 0). Default-OFF is byte-identical either way (the ring path only activates
when `/proc/shmring.1` is armed). Re-apply the 3-path fix above, then crack the direct-reader EOF bug, then the
cross-core HW coherency test finally has a real pipeline exercising the direct ring.

**netconsole discipline (HW, learned this run):** wedges HARD under `coresched.1` + load (~1 command then dies)
and on back-to-back medium pushes. Drive recovery ONE command per fresh connection (`shmring_hw_recover.py`);
settle ~5s between pushes. Pi was recovered to clean default-OFF v0.4.258 after the run.

**HW tooling added** (`scripts/`, this session): `shmring_hw_push.py` (push tools + smoke), `shmring_hw_test.py`
(OFF/ON/cross-core data-exact), `shmring_hw_dashtest.py` (ring-aware-dash variant), `shmring_hw_recover.py`
(disarm + health, 1-cmd/conn).

## What shipped (the diff — 5 files + 1 new header + 1 new test)

- `include/aios/shm_ring.h` (NEW): the shared ring layout + A72 barriers. One 4 KB frame: a
  128-byte header (tail in cache line 0, head in line 1 — no false sharing) + a **2048-byte
  power-of-two data ring**. head/tail are FREE-RUNNING uint32 (byte index = `counter & 2047`;
  `used = tail-head`). Barriers: `shm_ring_publish`=`dmb ishst` (release: data before tail),
  `shm_ring_observe`=`dmb ishld` (acquire: index before data; reads before head-advance),
  `shm_ring_fence`=`dmb ish` (full StoreLoad for the Dekker block/wake handshake).
- `src/servers/pipe_server.c` (+626): `pipe_ring[]` (frame/map/cap-copies, armed at PIPE_CREATE
  from `g_shm_ring`), `ring_server_push/pull` (server-side producer/consumer with the barriers),
  the ring branches in PIPE_WRITE / PIPE_WRITE_SHM / PIPE_READ / PIPE_READ_SHM /
  wake_one_blocked_reader, `PIPE_MAP_RING` (91) + `PIPE_RING_WAKE` (92), the **writer-park**
  (`ring_wblock[]` + `park_writer_save` + `wake_blocked_writer`), EOF/teardown hooks, and
  `shmring_cmd` (`/proc/shmring`). `g_shm_ring` default 0.
- `src/lib/posix_file.c` (+165): `shm_ring_send` (direct writer, yields on full, PIPE_RING_WAKE to
  wake a parked reader), `shm_ring_recv_fast` (direct reader, falls to PIPE_READ* to block on
  empty), `pipe_map_ring` + cached `stdin_ring_get`/`stdout_ring_get`, wired into the stdin read,
  fd-pipe read, stdout write, fd-pipe write paths. New `aios_fd_t.ring_vaddr`/`ring_tried`.
- `include/aios/root_shared.h` (+11): labels 91/92 (pipe-ep only; numeric overlap with NET_* on
  net-ep is harmless, like PIPE_SET_TIME(90)==NET_SOCKET(90)) + `g_shm_ring`/`shmring_cmd` externs.
- `src/procfs.c` (+8): `/proc/shmring[.0|.1]` wiring.
- `scripts/shmring_qemu_test.py` (NEW): 26-check logic/data-exactness gate (OFF baseline, ON,
  ON+coresched, kill-switch toggles; cross-checks `cat FILE|wc -c` vs `wc -c FILE`, multi-stage,
  storms). **Drive it before any flash.**

## The design (one buffer, two transports)

A ring-mode pipe's data lives in the ring for its whole life (decided once at PIPE_CREATE — no
asymmetry, no per-end mode negotiation). An end moves bytes EITHER directly in userspace OR via
legacy IPC that the server routes into the SAME ring (`ring_server_push/pull`). So a failed map
never splits data across two buffers, and a mixed direct/legacy pipeline is correct. Blocking is
symmetric: a reader on an empty ring parks via PIPE_READ/PIPE_READ_SHM (SaveCaller); a writer on a
full ring parks via PIPE_WRITE/PIPE_WRITE_SHM (SaveCaller, MR bytes stashed in `ring_wblock.stash`).
The reader's drain wakes a parked writer; a direct writer's publish wakes a parked reader
(PIPE_RING_WAKE). `reader_waiting` is set by the server before it parks a reader (the Dekker flag).
**OFF ⇒ nothing armed ⇒ every `pipe_is_ring(pi)` is false ⇒ the legacy path is byte-identical.**

## Two HW-only-style bugs found + fixed during QEMU bring-up (the hard part of this session)

`seq 1 1000 | wc -l` returned **539 (= exactly 2048 B = one ring)**, deterministically, via the
server-mediated path. Root causes (both now fixed; QEMU 26/26):
1. **The legacy writer dropped data on a full ring.** Line-buffered writers (seq) go through the
   MR `PIPE_WRITE` path; the old client stall-break returned `count` while dropping the bytes the
   full ring couldn't take, so the writer "finished" + exited → reader got EOF at one ring. FIX:
   the server now PARKS the full-ring writer (no drop, POSIX blocking-write), woken when the reader
   drains; teardown / `read_closed` delivers EPIPE so it never hangs. MR payload is stashed
   (`ring_wblock.stash`, ≤900) since MR bytes don't survive SaveCaller.
2. **A 0-byte read deadlocked it.** Something in the pipeline does `read(fd,buf,0)`; the ring branch
   treated `got==0` as "empty → park", so the reader parked while the writer was parked on full →
   mutual deadlock (`got=None` hang). FIX: `want<=0` returns 0 immediately (POSIX).

LESSON: the legacy pipe path's stall-break **silently drops** on a stuck/slow reader (pre-existing,
masked by the 4 KB shm_buf); the smaller 2 KB ring exposed it. The writer-park is the correct fix
and is contained to the server (no client change for the legacy path → OFF stays byte-identical).

## Adversarial review + 6 fixes (post-bring-up — all QEMU-reverified, 0 remaining bugs)

A multi-lens read-only review (coherency / wakeup / lifecycle / no-regress) found bugs; the
confirmed-real ones are FIXED and the whole gate re-ran green:
1. **`shm_ring_recv_fast` false-EOF (data loss).** The empty branch read `writer_closed` then
   re-read `tail` with the `dmb ishld` AFTER both → A72 could observe `closed=1` with a STALE tail
   → false EOF that drops the stream's final bytes. FIX: barrier BETWEEN the closed-load and the
   tail re-load (so observing closed=1 implies observing every pre-close write); a second acquire
   guards the data read when bytes appeared. `posix_file.c` shm_ring_recv_fast.
2. **Ring write hid EPIPE.** stdout (~L799) + fd-pipe (~L894) writes returned `count`, not the
   `shm_ring_send` result. FIX: return `sent` — it is `< count` ONLY on `reader_closed` (a live
   reader is always drained to completion), so EPIPE surfaces and normal writes still return count.
3. **`ring_clear_reader_waiting` unfenced** → added `shm_ring_fence()` (Dekker symmetry; a spinning
   direct writer promptly sees the clear).
4. **`wake_one_blocked_reader` could strand a reader.** An empty `ring_server_pull` `break` left the
   reader parked. FIX: serve EOF(0)+clear when `writer_closed`, else leave parked (reader_waiting
   stays set → the next tail publish re-issues PIPE_RING_WAKE). No lost wakeup, no double-serve vs
   `wake_blocked_readers_eof`.
5. **PIPE_EXEC ring reset wiped a live EOF/EPIPE.** Each end cleared BOTH close flags. FIX: the
   writer-end exec clears ONLY `writer_closed`, the reader-end ONLY `reader_closed` (mirrors the
   legacy `write_closed`/`read_closed` reset). Else e.g. `yes | head -1` could spin (writer never
   sees the reader's EPIPE) or a reader hang (never sees the writer's EOF). Symmetric — the review
   caught only the reader-end half.
6. **PIPE_MAP_RING cap leak past 4 maps.** `copies[4]` overflow left an untracked cap. FIX:
   early-return when `copy_count >= 4` (before allocating) → the end falls back to legacy IPC, which
   routes into the SAME ring (no split). The bottom store is now unconditional (room guaranteed).

**The review ALSO flagged the data-path index-load barriers** (`recv_fast`/`ring_server_pull` tail
loads, `shm_ring_send` head load) as "barrier must precede the index load" bugs. **REFUTED**, and a
separate independent memory-model lens re-adjudicated DEFINITIVELY in favor of the code:
`load idx; dmb ishld; load/store data` IS the canonical ARM `MP+dmb.st+dmb.ld` acquire (pairs with
the producer's `store data; dmb ishst; store idx` release; matches the Linux generic
`smp_load_acquire` fallback = READ_ONCE then barrier). A STALE (older) index is conservative-safe in
BOTH directions — a reader under-reads (retries, never loses/garbles), a writer under-estimates free
space (never overflows); you can never observe a FUTURE index. **Do NOT "fix" these** — moving the
barrier before the load would be wrong. Adjudication verdict: 0 remaining bugs; the implementation
is sound on logic + the A72 memory model on paper. ONLY real-silicon coherency stays unverified.

**KNOWN LIMITATION (default-OFF/experimental — documented, not enforced):** ring mode is armed
per-pipe at CREATE and ASSUMES one reader + one writer (SPSC). A single reader is parked-XOR-draining
(safe); a genuine MULTI-reader/writer pipe (`tee`, fork-and-both-read) would break SPSC head/tail
ownership. Standard shell `A|B` pipelines are SPSC. Auto-fallback to server-mediated for the
multi-end case (seed item 4) is a follow-up — do it before exposing ring mode to arbitrary pipelines.

## Verification status

- **QEMU GREEN (all 4 trees compile: build-04 / build-netd / build-rpi4 / build-rpi4-netd):**
  `shmring_qemu_test` 26/26, `smp_qemu_test` 7/7 (ceiling 30, no-regress with OFF default),
  `net_socket` 8/8, `netd` 10/10, `pipe_throughput` no-hang.
- **ssh_qemu_test is 3/6 — PRE-EXISTING, NOT a regression.** Confirmed by reverting ALL changes and
  rebuilding: HEAD is also 3/6 on this machine (the documented SSH session flakiness,
  [[project_ssh_recovered]] / BACKLOG "last command before exit"). The login passes; the session
  times out the same with or without this change.
- QEMU validates LOGIC ONLY. The A72 cross-core coherency (cacheable-inner-shareable + the
  release/acquire/fence handshake) is **UNVERIFIED** — needs the real Pi.
- `shmring_qemu_test` runs on `build-04` (its default; the standard QEMU target). On `build-netd`
  (netd-ON, = the RPi4 HW config) its netconsole driver times out at boot — a HARNESS limitation
  (it was tuned for build-04's boot timing), NOT a product issue: `netd_qemu_test` is 10/10 on the
  same cleaned `build-netd`, so netd-ON boots + nets fine with this change. The ring code is
  identical across trees; HW (build-rpi4-netd) is driven directly over serial, not this harness.

## HW test plan (the actual remaining work — drive from SERIAL, /dev/cu.usbserial-0001, lossy)

1. Pre-flash: re-run the FULL QEMU gate (BACKLOG "deeper pre-flash smoke") — it's already green.
2. `python3 scripts/pi_flash.py --host 192.168.0.8` (flash `build-rpi4-netd` kernel8, 3-way sha).
   Pi is on v0.4.257 build 2523; ARP `dc:a6:32:1c:2e:e1` if `.8` is dark (bounces `.8`/`.250`/`.197`).
3. Arm it: `cat /proc/shmring.1` then `cat /proc/coresched.1` (distribute procs to cores 1-3).
4. **Coherency (the #1 risk):** `cat /etc/passwd | wc -c` must equal `wc -c /etc/passwd`; then a big
   stream: `seq 1 100000 | wc -l` must be 100000, `seq 1 100000 | wc -c` byte-exact,
   `seq 1 2000 | sha256sum` must match the same stream read directly. ANY mismatch / all-NUL =
   a cross-core barrier/memory-type bug (the documented all-NUL class). `/proc/shmring` shows
   `push/pull/wake/park` counters to confirm the ring is actually exercised.
5. **The win:** with `shmring.1 + coresched.1`, the parallel-pipeline ceiling should RISE above the
   Stage-S-ON regression (was 30→6) toward 30, and `cat bigfile | wc -c` with the two stages on
   different cores should show a real throughput gain (TCG on QEMU can't show it — HW only).
6. If a coherency bug appears: the ring frame is mapped CACHEABLE on every end (ring_alloc +
   PIPE_MAP_RING both pass `1` for cacheable). Suspect a barrier type (dmb ishst/ishld/ish) or a
   missing fence; QEMU is blind to it. netconsole wedges under churn — drive HW from serial, rest
   ~45s between back-to-back connections.

## Honest exit (what this does NOT fix)

SHM-ring fixes the **data-flow half** only. The parallel-pipeline ceiling is partly SPAWN-bound
(fork/exec/teardown all funnel through the single core-0 servers + the lock-free allocator + the
seL4 big kernel lock). Even with zero-syscall data flow, spawning N pipelines stays BKL-limited
until the **multikernel re-arch** (per-core allocators + per-core server replicas — BACKLOG "SMP
re-architecture"). So expect the ceiling to recover toward 30 (not far past it) and the per-stream
cross-core throughput to improve; broad spawn-bound scaling is the next epic.

## Tuning knobs / follow-ups

- `SHM_RING_DATA` is 2048 (one page, pow2). A bigger ring (multi-page) smooths bursty producers and
  reduces park/wake transitions — a tuning follow-up after HW proves correctness. Keep it pow2.
- `ring_wblock.stash[900]` × MAX_PIPES = ~58 KB root BSS. Could be lazily allocated or shrunk
  (line-buffered MR writes are tiny; partial-park already handles a small stash). Low priority.
- The direct reader frees space in userspace; a parked legacy writer is only woken when that reader
  next hits the server (blocks on empty). Batchy but correct. A direct-reader→server space-notify
  would tighten it (follow-up).
