# DESIGN: Robust netconsole v2 for AIOS

A robust replacement for the AIOS `netconsole` network-control channel. Every claim is
grounded in the code at `main` (v0.4.166+). (Produced by a research agent after the v1
`__put` wedge surfaced in testing; see `netconsole-status` memory.)

## 1. Current architecture (with the real socket semantics)

**Process/boot topology.** `getty.c:277-288` forks `/bin/netconsole` exactly **once** at
boot and does not `waitpid` it - if netconsole dies, **nothing respawns it**.
`netconsole.c:362-408` `main()`: socket -> bind(0.0.0.0:2323) -> listen(lfd,1) -> infinite
`accept()` loop, serving **one client at a time, fully synchronously**. The backing thread
is `net_server.c:346 net_server_fn`, single-threaded in the root task.

**The crux - real socket semantics (from code, not assumed):**
- **(a) The accepted client socket is BLOCKING.** `accept4` (`posix_net.c:176-198`) reads
  the `flags` arg via `va_arg` but **discards it** and never sets `is_nonblock`. SOCK_NONBLOCK
  is silently ignored.
- **(b) `read()` on a socket.** `posix_file.c:478-496` packs (socket_id, want<=900, is_nonblock)
  and `seL4_Call(NET_RECVFROM)`. Server `net_server.c:523-565` (TCP): data -> returns n
  (<=900); `rx_eof` (peer FIN) -> returns **0**; nothing yet and `nb==0` ->
  `seL4_CNode_SaveCaller(blocked_slots[sid])`, sets has_blocked, **returns without replying**
  (caller parked in its seL4_Call indefinitely until data or FIN); nothing yet and `nb==1`
  -> returns `-11` (-EAGAIN).
- **(c) `write()` and dead/half-open peer.** `posix_file.c:662-684` chunks <=900. Server
  `net_server.c:489-521`: rc starts -1, set to len only if `type==1 && state==TCP_ESTAB`. Peer
  ESTAB -> bytes sent; peer half-closed/reset (left ESTAB) -> **-1** -> `-EIO`. This is the one
  reliable "client gone" signal, on the WRITE side.
- **(d) Connection-close signalling.** Peer FIN (`net_server.c:319-336`): ACK, rx_eof=1,
  state=FIN_WAIT, **wakes a blocked reader with len 0 (EOF)**. Peer RST (`net_server.c:107-126`):
  socket force-closed (in_use=0), but a blocked **recv is NOT woken** - only a blocked
  connect() is. **A reader blocked on a socket that gets RST is never replied to (F4).** There
  is **no half-open/keepalive/read-timeout** anywhere; a peer that vanishes without FIN/RST
  leaves the socket ESTAB forever and a blocked reader waits forever.
- **(e) net_server does NOT stall while a client is parked.** RX is driven by a bound
  notification (`boot_services.c:92-93` binds net_srv_ntfn to the TCB; driver signals it on RX,
  `net_virtio.c:321`). So parking a client via SaveCaller does not freeze packet processing -
  **the wedge is entirely in netconsole's own process.**
- **(f) No socket poll.** `posix_compat.c:52-157` `ppoll`/`pselect6` support only fd 0,
  is_tty fds, and is_pipe read ends - **no socket case** (revents stays 0). **poll()/select() on
  a socket is unusable.** A non-blocking socket loop must use `O_NONBLOCK` + `-EAGAIN` + sleep,
  NOT ppoll.

**File transfer (v1).** `__get` (`netconsole.c:285-325`): stat -> "__get ok <len>\n" -> stream
len raw bytes (~370 KB/s). `__put` (`netconsole.c:205-266`): "__put <path> <len>" then read
exactly len bytes into the file. Both share the single TCP connection + single serve loop.

## 2. Fragility analysis (failure modes, each with mechanism)

- **F1 - `handle_put` wedges the server on any large/aborted push** (`netconsole.c:229-242`).
  The receive loop treats **any** `read(cfd) <= 0` as client_gone, with **no -EAGAIN handling
  and no timeout**. Socket is blocking, so a mid-stream stall (host aborts/crashes, no FIN)
  parks the inner read in NET_RECVFROM **forever** -> port 2323 dies for all future clients.
  **This is the reported wedge.** (Only ever tested at ~5 KB, where the whole payload arrives
  in one burst.)
- **F2 - Single client, fully blocking, never respawned** (`netconsole.c:395-404`,
  `getty.c:278-288`). Any wedge blocks every other connection; getty forks once and orphans,
  so a death/wedge loses remote control until reboot.
- **F3 - No read/idle timeout on the control path** (`read_line` `netconsole.c:96-114`). A
  client that connects and sends nothing parks the server forever. The per-command *output*
  timeout (~30 s) does not bound time waiting for the next input line.
- **F4 - RST against a blocked reader is a permanent hang** (`net_server.c:107-126`). RST sets
  in_use=0 but wakes only a blocked connect, never a blocked recv -> the SaveCaller reply cap
  is dropped; netconsole wedges forever even though the slot was freed (and the freed slot can
  be reallocated while the old reply cap is still saved -> cross-talk).
- **F5 - Socket-slot exhaustion** (`MAX_NET_SOCKETS=8`, `net_server.c:12`). Wedged/half-open
  sessions hold sockets; a few exhaust the table and accept()/socket() start failing.
- **F6 - `__put`/`__get` length desync corrupts the stream.** Transfer is multiplexed onto the
  command stream with no per-message envelope; a short read/early EOF leaves leftover/missing
  bytes that the next read_line runs as a **command, as root**.
- **F7 - Output framing relies on the `aios# ` sentinel** (`pi_filexfer.py:42`); any command
  whose output contains `aios# ` desyncs the host parser.
- **F8 - 900-byte MR I/O cap + 10 ms poll** (throughput bound, not a correctness bug).
- **F9 - No auth, unconditional root, auto-exposed every boot** - every gap above is a remote
  root-shell exposure. By design (trusted LAN), but v2 should keep that contract explicit and
  optionally add a shared-secret gate.
- **F10 - `signal(SIGINT, SIG_IGN)` only** - no on-demand session abort.

## 3. AIOS constraints

1. No select/poll on sockets - multiplex via `O_NONBLOCK` + `-EAGAIN` + sleep.
2. Single-threaded non-blocking event loop is the simplest robust model (threads exist but
   add cost; `waitpid` has no WNOHANG - poll child exit via the O_NONBLOCK-pipe trick).
3. `fork()` works (used per-command).
4. No mbedTLS -> SSH/TLS out; auth = optional plaintext shared secret.
5. 900-byte MR I/O cap.
6. MAX_NET_SOCKETS=8 - bound concurrent sessions, close aggressively.
7. RST does not wake a blocked reader (F4) -> a robust loop must **never** do an unbounded
   blocking socket read; poll non-blocking so it can notice "stuck too long" itself.

## 4. Design options

- **A - Harden netconsole in place** (keep blocking model + watchdog child). Smallest diff;
  but still one client at a time, and respawn covers death not wedge unless wedge is made
  impossible via timeouts.
- **B - Single-process non-blocking MULTI-SESSION event loop** (RECOMMENDED). One process; all
  sockets O_NONBLOCK; a small array of session structs, each a state machine
  (READ_LINE/RUN_CMD/PUT/GET) with a `last_activity` deadline. Commands still run via
  fork+`dash -c`, parent polls the child's output pipe non-blocking interleaved with other
  sessions. No client can wedge or stall another; supports reconnects + concurrent sessions;
  idle/stuck sessions reaped by the loop itself (solves F4 - we never block on a socket).
- **C - Fork-per-connection + bounded pool + supervisor.** Isolation, but child reaping is
  awkward (no WNOHANG), a wedged child still holds a slot, and RST-vs-blocked-reader still
  bites each child unless each goes non-blocking (= Option B replicated).
- **D - Split control + file-transfer onto two ports.** Isolation, but doubles slot pressure
  (2 of 8) and two supervised processes.

## 5. RECOMMENDED design

**Adopt Option B** (single-process non-blocking multi-session event loop), with a length-
prefixed binary framing for all ops, per-operation deadlines throughout, and getty-based
supervision for the death case. Keep control and file-transfer split **within** the framed
protocol (distinct message types) rather than on two ports - Option D's isolation without a
second listener slot. Option B is the only model that makes the wedge **structurally
impossible** (we never issue an unbounded blocking socket call), killing F1/F3/F4, and it
naturally serves reconnects/multiple sessions (F2/F5).

**Wire protocol (length-framed, binary-safe, both directions).** Every message:
```
  magic  u8  = 0xA1      (resync anchor)
  type   u8             (see table)
  flags  u8  = 0
  rsvd   u8  = 0
  length u32 little-endian
  payload[length]
```
8-byte fixed header; binary-safe; no sentinel collision (kills F6/F7). Types: 0x01 HELLO
(C<->S, proto ver + optional auth token), 0x02 RUN (C->S, command line), 0x03 STDOUT (S->C,
output chunk), 0x04 EXIT (S->C, u32 status - explicit end-of-command, so the host no longer
scans for `aios# ` and gets the real exit code), 0x05 PUT_BEGIN (C->S, path + u64 len), 0x06
DATA (both, streamed file bytes), 0x07 PUT_END (S->C, status), 0x08 GET_BEGIN (C->S, path),
0x09 GET_META (S->C, u64 len or -errno), 0x0A GET_END, 0x0B PING/PONG (liveness), 0x0C BYE.
Payloads chunk at <=512 bytes to stay under the 900-byte MR cap with header overhead. An
optional Phase-0 compat shim keeps the old `__put`/`__get`/line protocol working (detect 0xA1
magic vs ASCII first byte) until `pi_filexfer.py` is ported; then drop legacy.

**The I/O loop (non-blocking, deadline-driven).** One process/thread. Per session: cfd
(O_NONBLOCK), state, deadline_tick (reset on progress), frame header buffer, type/need/got,
child pid + out_rd (O_NONBLOCK), file_fd + remaining, a small buf. MAX_SESSIONS = 6 (8 -
listener - margin). Main loop, no blocking socket calls anywhere:
```c
fcntl(lfd, F_SETFL, O_NONBLOCK);
for (;;) {
    int c = accept(lfd, 0, 0);
    if (c >= 0) { fcntl(c, F_SETFL, O_NONBLOCK); session_open(c); }
    int did_work = 0;
    for (i in sessions) if (sess[i].state != S_FREE) did_work |= session_step(&sess[i]);
    uint64_t now = tick_now();
    for (i in sessions) if (sess[i].state != S_FREE && now > sess[i].deadline_tick)
        session_timeout(&sess[i]);          /* kill child, close cfd, free slot */
    if (c < 0 && !did_work) nanosleep(5ms); /* idle sleep only; avoid hot spin */
}
```
`session_step`: S_HDR/S_BODY read non-blocking (>0 consume + reset deadline; ==0 FIN close;
-EAGAIN return); on full frame dispatch by type. S_RUN: non-blocking read of out_rd (v1's
mechanism), wrap in STDOUT frames, write to cfd non-blocking; pipe EOF -> EXIT frame with the
reaped status. **S_PUT: read up to `remaining` bytes non-blocking; `-EAGAIN` means "not done,
come back" - the precise fix for F1 (a stall keeps the session in S_PUT until its deadline, it
cannot wedge anything).** S_GET: read file in <=512 chunks, emit DATA frames with back-pressure.
Socket WRITE is now non-blocking too: keep a tiny per-session pending-write buffer, retry on
-EAGAIN, treat -EIO/short as client-gone.

**Why this kills the wedges:** F1/F3 - no unbounded blocking read exists; a silent peer just
fails to reset its deadline and is reaped. F4 - irrelevant: we poll non-blocking, so a dead
socket manifests as "no progress past deadline". F2 - one stalled session can't block accept or
the others.

**Timeouts (concrete, all reset on progress):** idle/connection 120 s; per-command output 60 s
(SIGKILL child, EXIT(-1), keep session); transfer stall 30 s (abort, PUT_END(ETIMEDOUT), keep
session); optional PING after 30 s idle, reap if no PONG. Use the ARM generic timer (getty
already does, `getty.c:351-357`) or accumulated nanosleep ticks.

**Respawn/watchdog.** (1) getty supervises **death**: change `getty.c:277-288` from fork-and-
forget to keep the pid and re-fork on exit (rate-limited >=2 s backoff) inside getty's existing
loop - the post-settle spawn path is safe (boot-critical-path spawn ban still holds). (2) The
event loop's deadlines mean netconsole **cannot silently wedge**, so "respawn a hung process" is
designed out; getty supervision is belt-and-suspenders for real crashes (SIGSEGV/OOM).

**Optional auth (no crypto).** HELLO may carry a token compared (constant-time) against
`/etc/netconsole.key`; mismatch -> BYE. Obfuscation only (plaintext on the wire), default off;
real auth waits on the lost mbedTLS/SSH path. (F9 mitigation.)

**IPC/labels touched: NONE on the kernel side are strictly required** - netconsole is userspace
and the primitives exist (O_NONBLOCK via F_SETFL, NET_RECVFROM -EAGAIN, NET_SENDTO,
NET_CLOSE_SOCK, the accept-backlog drain `net_server.c:468-486`). Keep the blast radius in
userspace. **Recommended small, decoupled net_server hardening (separate commit):** R1 (fixes
F4) - in the RST handler (`net_server.c:107-126`), wake a blocked recv with EOF the same way FIN
does, not just a blocked connect (benefits sshd + any socket app); R2 (optional) a real
NET_POLL readability label - NOT needed for Option B (it polls via -EAGAIN), defer; optionally
bump MAX_NET_SOCKETS 8->16.

**Files changed:** `src/apps/netconsole.c` (rewrite to the event loop + framed protocol; reuses
the existing `dash -c` fork + non-blocking output-pipe drain per session); `scripts/pi_filexfer.py`
(port to frames, keep sha256); `scripts/netcon_qemu_test.py` (parse frames; add tests for large
`__put` with mid-stream stall, concurrent sessions, half-open reaping, reconnect storm - this
harness is the gate before flashing and is authoritative for socket logic); `src/apps/getty.c`
(supervise+respawn); optional `src/servers/net_server.c` (R1). No header/label changes for the
minimal path. Build: userspace app via `./scripts/aios-cc src/apps/netconsole.c -o
build-04/sbase/netconsole` -> mkdisk; if net_server/getty change, `ninja -C build-04 && ninja -C
build-rpi4` -> mkdisk -> mksdcard -> flash. Bump patch only when root-task code changes.

## 6. Phased plan

- **Phase 0 - Framing scaffold + host tool** (no behaviour change). Define the 8-byte frame in
  netconsole + pi_filexfer.py + netcon_qemu_test.py; implement HELLO/RUN/STDOUT/EXIT + framed
  PUT/GET on the host with a magic-byte detector so the old protocol still works.
- **Phase 1 - Minimal robust v2 (the actual fix; the line between "robust v2" and "nice-to-
  haves").** Rewrite netconsole.c to the single-process non-blocking event loop, all sockets
  O_NONBLOCK, per-session deadlines, MAX_SESSIONS concurrent clients; framed RUN->STDOUT*->EXIT
  + length-framed -EAGAIN-tolerant PUT/GET (kills F1/F3/F6); getty supervise+respawn (kills
  F2-death); update netcon_qemu_test.py with the stall/large-put/concurrent/half-open/reconnect
  cases. **Exit criteria:** a 256 KB `__put` that stalls mid-stream does NOT wedge the port; a
  2nd client runs a command while the 1st is mid-transfer; `nc -9` (RST) frees the slot within
  the idle deadline; netconsole killed on the Pi is respawned by getty. Then flash + HW smoke.
- **Phase 2 - net_server hardening** (R1 RST-wakes-reader; optional MAX_NET_SOCKETS bump).
  Separate commit, both trees, bump patch.
- **Phase 3 - Nice-to-haves.** Optional shared-secret HELLO gate; PING/PONG liveness; drop the
  legacy compat shim once pi_filexfer.py is fully on frames; (deferred) NET_POLL + socket
  support in ppoll; SSH/scp once mbedTLS is rebuilt.

**Key code references:** wedge site `netconsole.c:229-242` (handle_put), `:96-114` (read_line),
`:395-404` (single accept loop); socket semantics `net_server.c:523-565` (NET_RECVFROM
SaveCaller), `:489-521` (NET_SENDTO rc=-1 unless ESTAB), `:107-126` (RST does not wake recv),
`:319-336` (FIN wakes recv EOF); no socket poll `posix_compat.c:52-157`; libc socket I/O
`posix_file.c:478-496/662-684/758-771`; accept ignores nonblock `posix_net.c:176-198`; net_server
stays live (bound ntfn) `boot_services.c:92-93` + `net_virtio.c:321` + `net_server.c:399-405`;
auto-start no respawn `getty.c:277-288`.
