# HANDOVER -- session 26 (2026-07-02): NETWORKING arc COMPLETE (server + non-blocking + DNS + confinement)

**LATEST (inc 5): NETWORK-ACCESS CONFINEMENT -- the networking arc (client / server / non-blocking /
DNS / confinement) is COMPLETE.** The net analogue of M4.2 fs confinement, a pure PAL policy (NO new
kernel/ABI; commit `9f49b4d`, v0.5.37; HW-validated on the RPi5). Launched with `AIOS_NET_ALLOW` set, a
guest may `connect` only to allow-listed endpoints; anything else is refused with `-EACCES` before any
host connect (a confined guest cannot phone home or scan). `pal_net_init_once` (both front-ends) parses
`AIOS_NET_ALLOW="ADDR[/prefix][:port],..."` (dotted quad or `*`, optional CIDR, port number or
`*`/omitted = any; IPv4) into a rule table; `net_addr_allowed` gates `pal_host_connect`. Default (unset)
= unrestricted (byte-identical to before). Scope: outbound `connect` only (bind/listen not yet confined;
a confined guest must allow its resolver `:53`). **An adversarial review caught a FAIL-OPEN** (`atoi`
returns 0 on junk, and 0 is the "any" sentinel -> a malformed `/prefix` = mask 0 = allow-all, a
non-numeric `:port` = any-port) -- FIXED: a prefix/port must be non-empty all-digits, else the rule is
dropped (fail-**closed**). Proof: `test/net_jail.c` red-teams `guest/prog_netjail.c` (checks `connect`
fails with `EACCES` specifically) across allow (exact + CIDR) and deny (wrong-port / different-subnet /
malformed-prefix / malformed-port) cases; gate key `netjail`, green both backends.

---

**inc 4: AIOS programs use HOSTNAMES, not just dotted-quad IPs -- the networking arc
(client / server / non-blocking / DNS) is FUNCTIONAL.** Bryan picked the "from-scratch + timed wait"
path. Two sub-milestones, both NO new ABI, both HW-validated on the RPi5 (`sh gate.sh` -> linux=0
seccomp=0):
- **inc 4a (v0.5.35, commit `8edeb48`): a TIMED socket read via `SO_RCVTIMEO`** (reuses `setsockopt`).
  A guest sets `SO_RCVTIMEO` (a struct timeval); the kernel stores it per-fd and gives a parked read a
  deadline (`proc.blk_deadline_ms`); `net_publish_watches` carries the earliest deadline to the `ppoll`,
  and `net_expire_deadlines` returns `-EAGAIN` to a timed-out read (run AFTER `net_retry_parked` so a
  read that readied exactly at the deadline still completes). `pal_net_wait_ready` now surfaces a `ppoll`
  TIMEOUT (not just readiness) as code 4. Proof: `guest/prog_rcvtimeo.c` (`SO_RCVTIMEO 300ms` ->
  `EAGAIN ~303ms`, an already-queued datagram read returns immediately). Gate key `rcvtimeo`.
- **inc 4b (v0.5.36, commit `9486417`): a FROM-SCRATCH UDP DNS resolver in libaios.** `gethostbyname` +
  `getaddrinfo`/`freeaddrinfo`/`gai_strerror` + `inet_ntoa`, built ENTIRELY on the AIOS socket ABI
  (`SOCK_DGRAM` + `connect` + `read`/`write`) with `SO_RCVTIMEO` for a 2s×3 timeout on packet loss -- so
  a seL4 PAL gets DNS for free and the kernel stays host-agnostic. It builds a type-A query and parses
  the response (header/question skip + answer records WITH name-compression pointers, bounds-checked
  untrusted input). Nameserver from `$AIOS_DNS_SERVER` ("ip[:port]") else `/etc/resolv.conf`. `struct
  hostent`/`addrinfo` live in `include/aios_abi.h` (so `libaios.c` sees the same layout under every build
  flag); shadow `<netdb.h>`. Scope: A/IPv4 only, no search domains, first answer wins, numeric
  `getaddrinfo` service. Proof: `guest/prog_dns.c` resolves a name via BOTH APIs through a host DNS stub
  (`test/dns_server.c`, `$AIOS_DNS_SERVER` -> 127.0.0.1) with a fixed A record. Gate key `dns`.

An **adversarial-review Workflow** (3-lens find->verify) ran on the diff: the response parser survived
(no OOB reads), and it caught + we FIXED one real bug pre-commit -- a stale `SO_RCVTIMEO` deadline: a
completed timed read left `blk_deadline_ms` set, so a later non-read socket park (write/connect/accept)
could inherit it and be spuriously aborted with `-EAGAIN` (a partial write would discard already-sent
bytes). Fix: every socket-park entry now sets `blk_deadline_ms` (read -> deadline; write/connect/accept
-> 0), plus a defensive `SKOP_READ` guard in `net_expire_deadlines`. Re-gated green (colima + RPi5).

**Remaining networking limits:** IPv4-only; **no network-access confinement** (which hosts/ports a guest
may reach) -- that is inc 5, the natural next step (a PAL policy analogous to M4.2 fs confinement).

---

**inc 3: socket I/O no longer BLOCKS the single-threaded kernel.** Networking increment 3
makes host sockets NON-BLOCKING and adds PARK/WAKE, so a socket read/write/accept/connect that would
block PARKS the guest (the socket analogue of the pipe park) and the kernel services others -- removing
the honest single-guest limit of inc 1/2. **NO new ABI** (a pure kernel+PAL mechanism; ABI stays 62;
v0.5.33 -> **v0.5.34**). `pal_guest_next` (both trap front-ends) now CO-WAITS guest events AND socket
readiness (`ppoll` on the kernel-published watch set with SIGCHLD atomically unblocked, race-free);
when NO guest is socket-parked it uses the ORIGINAL blocking `waitpid` -- byte-identical, zero
regression. The guest still sees ordinary BLOCKING socket semantics. Committed **`bbe1503`** on `main`.
**HW-validated on the RPi5: `sh gate.sh` -> linux=0 seccomp=0 both backends** (green on colima too).
Proof: `guest/prog_netloop.c` -- TWO AIOS guests (a forked echo server + the client) round-trip TCP
inside ONE aios-uk, which would DEADLOCK under the old blocking accept (gate key `netloop`; stress
40/40). An **adversarial-review workflow** caught + fixed two real defects before commit: an in-flight
`connect` being reported as a false success (getsockopt(SO_ERROR)==0 on a still-connecting socket --
now gated on `pal_host_sock_writable`), and `SKOP_WRITE` returning the full length on an unreadable
guest buffer (now bytes-sent / -EFAULT). Full detail: the "increment 3" section below.

(A pre-existing FLAKY test surfaced during validation: `test/login_pty.c`'s `whoami=aios` pty-capture
races under load -- it flaked once each on colima and the RPi5 across many runs, always a clean re-run.
NOT a kernel bug. A dedicated session is hardening it expect-style; a local expect-style rewrite is in
the working tree but was deliberately NOT committed here so that session owns the file. `execjail` also
flaked once on loaded colima -- stress-proven 40/40, environmental, not the diff.)

---

**inc 2 HEADLINE: AIOS can now LISTEN.** Networking increment 2 adds the TCP **server** surface --
`BIND`/`LISTEN`/`ACCEPT` + `SETSOCKOPT`/`GETSOCKNAME` (v0.5.32 -> **v0.5.33**, ABI **57 -> 62**). An
AIOS program binds a port, listens, and accepts connections; a host client connects to it and
round-trips a message. Committed **`f0030e7`** on `main` (Bryan pushes). Green on BOTH PAL backends on
colima AND HW-validated on the RPi5 (`aios@tkrpi5.local`, Ubuntu 26.04, kernel 7.0, gcc 15):
`sh gate.sh` -> **linux=0 seccomp=0**.

Continues the 2026-06-24 pivot (AIOS as a gVisor-style userspace kernel on Linux; verified
seL4-on-x86-64 is the destination; verification is the soul; programs see only the AIOS ABI, the host
behind a narrow PAL). Session 25 (docs/HANDOVER_20260627_session25.md) completed the system layer +
delivered sched_ext + networking increment 1 (a TCP client). This session delivers networking
increment 2.

## What shipped (1 commit on `main`)

**`f0030e7` -- NETWORKING inc 2: a socket SERVER (v0.5.33, ABI 57 -> 62).** Five new syscalls,
host-passthrough behind the boundary exactly like the client surface (inc 1) and the VFS:

- `AIOS_SYS_BIND` (0x1038), `AIOS_SYS_LISTEN` (0x1039), `AIOS_SYS_ACCEPT` (0x103A),
  `AIOS_SYS_SETSOCKOPT` (0x103B), `AIOS_SYS_GETSOCKNAME` (0x103C).
- **ACCEPT mirrors sys_socket** -- it returns a NEW AIOS fd backed by the accepted host socket, so
  `read`/`write`/`close` work on it for free. `accept`/`getsockname` fill the caller's `sockaddr`
  with an in/out `addrlen` (caller buffer size -> actual peer length, truncated to the buffer).
- **bind/listen/setsockopt** pass the sockaddr/optval bytes straight to the host socket (the layouts +
  `SOL_SOCKET`/`SO_REUSEADDR`/`SO_REUSEPORT` values match the host, so it is a byte-forward like
  termios). **getsockname** lets a server bind an ephemeral port (`:0`) and learn which one it got --
  the race-free way to test a server (no fixed-port collision, no TOCTOU).
- Layers touched (all additive; no existing path changed): `include/aios_abi.h` (numbers + `SO_*`
  constants), `include/pal.h` + `pal/pal_linux_common.c` (pal_host_bind/listen/accept/setsockopt/
  getsockname -- shared by both trap backends), `kernel/aios_kernel.c` (sys_* + dispatch, still
  host-agnostic -- only the PAL touches the host), `lib/libaios.c` (the five wrappers),
  `lib/include/sys/socket.h` (decls + constants), `include/aios_version.h`, `Makefile`, `gate.sh`.
- The accept fd-alloc error path frees the host socket (no leak) and matches sys_socket.

**Proof (gate key `netsrv`):** `guest/prog_netserver.c` is a real AIOS TCP echo server -- it sets
`SO_REUSEADDR`, binds `127.0.0.1:0`, prints its port, accepts one connection, and echoes the message.
`test/net_server.c` is a host driver: it launches the AIOS server, reads the announced port from the
server's stdout (a pipe), connects as a client, sends a message, reads the echo, and PASSes iff the
echo matches AND the AIOS server exited 0. Fully self-contained -- no external network.

**Vendored, running UNMODIFIED (unchanged this session):** dash + 28 sbase utils.

## Validation

- **colima** (`docker run -d ... gcc:13 ... gate.sh`): `RESULT: linux=0 seccomp=0` -- all 23 keys
  including `net` and `netsrv`, both backends.
- **RPi5** (`aios@tkrpi5.local`, Ubuntu 26.04, kernel 7.0, gcc 15): re-run `RESULT: linux=0
  seccomp=0`. Both `net` and `netsrv` green under both backends; the AIOS server accepted a real
  host-client TCP connection and echoed the message.
  - **NOTE (a known flake, not a regression):** the FIRST RPi5 run had `seccomp=1` from `login`
    alone -- the login_pty test logged in + ran a command + logged out + respawned fine, but the
    `whoami=aios` **output capture** flaked under the seccomp backend. This is the documented
    intermittent seccomp/pty stall (it bit ~3x in s25). The identical binary passed `login` under
    seccomp on colima and under ptrace on the Pi, and the diff is purely additive to the socket path
    (never touches login/dash/pty/the trap mechanism). A clean re-run was fully green.
- **Adversarial review** (a 3-lens workflow -- kernel memory-safety/error-paths, cross-layer ABI
  consistency, test-harness correctness -- each finding verified): **zero findings**.

## KEY LESSONS / notes (carry forward)

- **A new AIOS syscall costs nothing in the trap layer.** Both PAL backends decode the GATEWAY
  (x8=gettid/178, real nr in x9); a new `>= 0x1000` number needs NO seccomp filter change. The
  kernel-side `pal_host_*` calls run in the tracer (unfiltered), so server ops "just work" under
  seccomp the same as socket/connect did.
- **getsockname is worth the one syscall.** It makes the server test race-free (bind :0 -> announce
  the real port -> the host connects), which is how a real ephemeral-port server works anyway.
- **The single-threaded kernel still BLOCKS on accept/read/connect.** That is the honest limit and
  the whole point of the NEXT increment. Fine for one guest (the gate is single-guest).

## What shipped -- increment 3 (commit `bbe1503`; NO new ABI, v0.5.34)

Non-blocking sockets + park/wake. The mechanism, layer by layer:

- **PAL (`pal_linux_common.c`):** every socket is `O_NONBLOCK` (`set_nonblock` in `pal_host_socket` +
  the accepted fd in `pal_host_accept`). A would-block read/write returns `PAL_EWOULDBLOCK` (which is
  `-EAGAIN` -- so `pal_host_read/write` needed NO change); `pal_host_connect` maps `EINPROGRESS` to it.
  `pal_host_sock_error` (getsockopt `SO_ERROR`) + `pal_host_sock_writable` (poll `POLLOUT`) complete a
  non-blocking connect. The readiness seam: `pal_net_watch_reset`/`pal_net_watch_add` (the kernel
  publishes the fds its parked guests await, each loop), `pal_net_have_watches`, and
  `pal_net_wait_ready` (the `ppoll`). `pal_net_init_signals` blocks `SIGCHLD` process-wide + installs
  an empty handler so `ppoll`'s sigmask can unblock it atomically -- a child stop (guest event) OR a
  ready socket wakes the `ppoll`, with no lost-wakeup race.
- **Trap front-ends (`pal_linux.c` + `pal_seccomp.c`):** `pal_guest_next` gained a co-wait branch --
  when `pal_net_have_watches()`, it does `waitpid(WNOHANG)` (collect a pending guest event) then, if
  none, `pal_net_wait_ready()` (`ppoll`), returning a **new event code 4** when a socket is ready.
  When there are NO watches it falls through to the ORIGINAL blocking `waitpid(-1)` -- byte-identical,
  so every non-network path is untouched (zero regression, confirmed by the full gate + a 40/40
  `execjail` stress).
- **kernel (`aios_kernel.c`):** new states `PS_BLOCKED_NET_IN`/`OUT` + `blk_sock_op` (`SKOP_*`).
  `net_attempt(p)` does ONE non-blocking attempt and either completes (`pal_guest_return` + `PS_RUNNING`)
  or leaves the guest parked -- it serves both the first attempt and every wake retry. `do_read`/
  `do_write` gained a socket branch; `connect`/`accept` moved to the SPECIAL dispatch group
  (`do_connect`/`do_accept`, they can block). The event loop calls `net_publish_watches()` before each
  `pal_guest_next` and `net_retry_parked()` on code 4 (retry-all, level-triggered). A socket-parked
  guest is `EINTR`-interrupted by `^C` (`forward_terminal_signal`). The kernel stays host-agnostic --
  readiness/`ppoll` is entirely a PAL concern behind the `pal_net_*` seam.
- **Two review-caught fixes (before commit):** (1) retry-all completed an in-flight `SKOP_CONNECT` with
  a false success -- `getsockopt(SO_ERROR)` reads 0 on a still-connecting socket, so a wake caused by a
  DIFFERENT guest's fd reported "connected" prematurely -> now `net_attempt` gates `SKOP_CONNECT` on
  `pal_host_sock_writable(this fd)` before trusting `SO_ERROR`, re-parking otherwise; (2) `SKOP_WRITE`
  returned the full requested length when the guest buffer was unreadable -> now returns bytes actually
  sent, or `-EFAULT` (matching the host-file write path).

Proof: **`guest/prog_netloop.c`** -- the parent forks a child echo SERVER (bind :0, listen, accept,
echo) and acts as the CLIENT (connect, send, recv), all inside ONE `aios-uk`; the child hands its port
to the parent over an AIOS pipe. Under the old blocking model the child's `accept()` would freeze the
whole kernel before the parent ran -> deadlock; with park/wake both make progress. Gate key `netloop`
(green both backends); the existing `net`/`netsrv` tests now also traverse the park/wake path. Stress:
`netloop` 40/40, `execjail` 40/40. HW-validated on the RPi5 (`sh gate.sh` -> `linux=0 seccomp=0`).

## NETWORKING -- what's next (the recommended order)

1. **DONE (s25): inc 1 -- a TCP CLIENT** (SOCKET/CONNECT).
2. **DONE (s26): inc 2 -- a TCP SERVER** (BIND/LISTEN/ACCEPT + SETSOCKOPT/GETSOCKNAME).
3. **DONE (s26): inc 3 -- NON-BLOCKING + park/wake** (net I/O no longer blocks the kernel; the crux).
4. **NEXT: inc 4 -- DNS** -- a resolver so programs use hostnames not dotted quads. Simplest = a PAL
   passthrough to host `getaddrinfo` (a `pal_host_getaddrinfo` behind the seam; but it BLOCKS -- the
   host resolver does synchronous UDP/file I/O, so either accept a brief block or run it via the
   park/wake path over a non-blocking UDP socket). The "grow our own" path = a from-scratch UDP
   resolver in libaios/the kernel over the now-non-blocking socket layer (fits "programs see only the
   AIOS ABI" + reuses inc 3). Decide the split with Bryan.
5. **THEN: inc 5 -- network-access CONFINEMENT** -- which hosts/ports a guest may reach, a PAL policy
   analogous to M4.2 fs confinement (`AIOS_ROOT`): e.g. an allow-list checked in `pal_host_connect`/
   `pal_host_bind`. Zero or minimal ABI (a PAL policy, like fs confinement).

Other endgame arcs (Bryan's call): scx_aios AIOS-AWARE policy; a "boots into AIOS" RPi5 appliance
deploy (systemd/getty light path); `pal_sel4.c` (BACKLOGGED -- the eventual soul).

**Loose end (a dedicated session owns it):** `test/login_pty.c` is intermittently flaky (the
`whoami=aios` pty-capture races the shell under load). A local expect-style rewrite (wait for each
token -- `login:`, `Password:`, and the `who=aios` OUTPUT -- before proceeding) is in the working tree
and passed all four gate passes this session, but was NOT committed here so the spun-off session owns
the file. If it lands, drop the local copy.

## Dev loop (carry forward, unchanged from s25)

- colima: `UK=/Users/bryan/Desktop/github_repos/AIOS/uk; docker run -d --platform linux/arm64
  --cap-add=SYS_PTRACE -v "$UK":/uk -w /uk gcc:13 sh -c 'stdbuf -oL sh gate.sh >
  /uk/scratch_gate.log 2>&1'` then poll `$UK/scratch_gate.log` for `RESULT:` (rm before commit).
- RPi5 (`aios@tkrpi5.local`): my Mac key + passwordless sudo are installed (Bryan-authorized) -- plain
  `ssh`/`rsync` work. `rsync -az --delete <build-artifact excludes> uk/ aios@tkrpi5.local:~/uk/` then
  `ssh aios@tkrpi5.local 'cd ~/uk && rm -f gate_hw.log && nohup setsid sh -c "sh gate.sh > gate_hw.log
  2>&1" &'` + poll `~/uk/gate_hw.log` for `RESULT:`.
- GOTCHAS: ABSOLUTE `.../AIOS/uk` path for docker `-v` (git drifts `$PWD`); write the gate log INTO
  `uk/` (colima only mounts `$HOME`); the GATEWAY for seccomp; `.pal.stamp` forces a PAL-switch
  rebuild; the intermittent seccomp dash/pty stall (timeout-guarded -- re-run if only `login` flakes);
  gcc 14/15 stricter than 13; NO apostrophes in a `sh -c '...'` body; macOS has no `setsid`/`timeout`
  (detach + poll on the Pi, use ssh `ConnectTimeout` on the Mac); a ptrace hang is SILENT.

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue building AIOS as a **gVisor-style userspace kernel on Linux** (the 2026-06-24 pivot off
seL4/RPi4 -- Linux is the interim substrate, verified seL4-on-x86-64 is the destination; verification
is the soul; programs see only the AIOS ABI, the host sits behind a narrow PAL). READ FIRST: memory
[[project_pivot_linux_userspace_kernel]] + docs/HANDOVER_20260702_session26.md +
docs/HANDOVER_20260627_session25.md + docs/AIOS_KERNEL_DEPENDENCIES.md + uk/README.md.

WORKING BRANCH = `main` (commit per milestone on main, Bryan pushes). The `uk/` tree: a host-agnostic
kernel (kernel/aios_kernel.c includes ONLY aios_abi.h + aios_version.h + pal.h) over a SHARED Linux
host-driver core (pal/pal_linux_common.c) + TWO trap front-ends (pal/pal_linux.c = PTRACE_SYSCALL,
pal/pal_seccomp.c = seccomp RET_TRACE; `make PAL=linux|seccomp`) + libaios + shadow standard headers
(lib/include, -nostdinc).

DONE through **v0.5.37, 62-syscall ABI**: OPERATIONAL (vendored dash + 28 sbase utils UNMODIFIED) +
the boundary COMPLETE + FULL JOB CONTROL + raw termios + a SECOND PAL backend (seccomp via the
GATEWAY) + a minimal Linux-6.18 appliance + the SYSTEM LAYER COMPLETE (inc 1+2: init->login->session->
logout->respawn, per-process uid/gid identity + login SWITCHES USER, crypt() SHA-512 =openssl passwd
-6, a fuller shell incl. uname reports AIOS / a real FLOAT printf =glibc / date / tr / cut / seq, a
root-gated clean SHUTDOWN, config-driven init /etc/inittab) + two endgame subsystems: (1) sched_ext --
uk/sched_ext/scx_aios is AIOS's OWN CPU scheduler as a BPF struct_ops, HW-validated on the RPi5; (2) the
**NETWORKING ARC is COMPLETE** -- inc 1 (a TCP CLIENT: SOCKET/CONNECT) + inc 2 (a TCP SERVER: BIND/
LISTEN/ACCEPT + SETSOCKOPT/GETSOCKNAME) + inc 3 (NON-BLOCKING sockets + PARK/WAKE, no new ABI: sockets
O_NONBLOCK, the guest PARKS on would-block PS_BLOCKED_NET_IN/OUT, pal_guest_next co-waits guest events
AND socket readiness via ppoll with SIGCHLD atomically unblocked -> event code 4 -> net_retry; no-socket
path = the ORIGINAL blocking waitpid, zero regression) + inc 4 (DNS: 4a SO_RCVTIMEO timed read, 4b a
FROM-SCRATCH UDP DNS resolver in libaios -- gethostbyname/getaddrinfo -- over the AIOS socket ABI) +
inc 5 (NET-ACCESS CONFINEMENT: AIOS_NET_ALLOW allow-list checked in pal_host_connect, out-of-list ->
EACCES, fail-closed parser -- a PAL policy, no new ABI). All host-passthrough behind the boundary (a
socket is an AIOS fd backed by a host socket; ACCEPT returns a NEW AIOS fd). Proven: gate keys `net`
`netsrv` `netloop` (2 AIOS guests round-trip TCP in one aios-uk) `rcvtimeo` `dns` `netjail`, + fetched
http://example.com over the REAL internet from the RPi5. ENTIRE tree HW-validated on the RPi5
(`aios@tkrpi5.local`, Ubuntu 26.04, kernel 7.0, gcc 15; `sh gate.sh` -> linux=0 seccomp=0). NEVER patch
vendored sources -- grow libaios. Each milestone was adversarially reviewed by a Workflow (3-lens
find->verify) BEFORE commit -- real bugs got caught (incl. the inc-5 fail-open); KEEP DOING THIS.

PRIMARY TASK -> **Bryan's pick: an AIOS-AWARE scx_aios scheduling policy.** Evolve
`uk/sched_ext/scx_aios` (AIOS's own sched_ext BPF scheduler) from its current flat global-FIFO into a
policy that PRIORITISES the AIOS kernel (`aios-uk`) + its guest tracees over other host tasks -- AIOS is
a userspace kernel, so the host's CPU scheduler should favour the AIOS workload. NOT STARTED this
session (only surveyed). Current state (read these): `uk/sched_ext/scx_aios.bpf.c` (a
`struct sched_ext_ops` with init/enqueue/dispatch/exit; ONE shared DSQ served FIFO; kernel-7.0 SCX API;
self-contained kfunc decls; no vendored scx headers), `uk/sched_ext/scx_aios.c` (the userspace loader --
open+load+attach the struct_ops, hold until Ctrl-C, detach), `uk/sched_ext/Makefile` + `README.md`.
HW-validated on the RPi5 (attach -> ops=aios/state=enabled, the full AIOS gate passes BOTH backends
while it schedules the host, detach reverts).

**PROPOSED DESIGN (refine as you build):** (1) Identify AIOS tasks IN BPF by comm-ancestry -- a task is
"AIOS" if it OR an ancestor within ~8 `real_parent` hops has `comm == "aios-uk"` (the kernel's comm is
"aios-uk"; guests are its fork children, so a guest's parent chain reaches it). Use CO-RE reads
(`#include <bpf/bpf_core_read.h>`, `BPF_CORE_READ(t, real_parent)`, read `comm[16]`, byte-compare to
"aios-uk\0"); a bounded (unrolled) 8-iteration loop keeps the verifier happy. (2) Give AIOS tasks
PRIORITY via a second HIGH DSQ: enqueue -> AIOS tasks to `AIOS_DSQ_HI`, others to `AIOS_DSQ_NORMAL`;
dispatch -> drain HI first, then NORMAL. Keep the DEFAULT slice (AIOS is bursty/blocks on I/O + ptrace
stops, so strict priority does NOT starve sshd in practice -- note this honestly; a weighted/budgeted
policy is a further refinement). (3) Make it OBSERVABLE: global counters (`__u64 aios_enq, other_enq`
bumped with `__sync_fetch_and_add`) the loader reads via the skeleton (`skel->bss->...`) + prints -- so
you can SHOW the policy tags + prioritises AIOS tasks (aios_enq >> 0 while the gate runs).

**DEV LOOP (sched_ext is DIFFERENT from the ptrace kernel -- colima's 6.8 kernel CANNOT load sched_ext,
so NO colima load-test):** build the BPF in an `ubuntu:26.04` container (clang21/bpftool7.7/libbpf1.6.3,
ABI-identical to the RPi5) against the RPi5's BTF, OR build on the RPi5 -- BUT the RPi5 is MISSING
`libbpf-dev` headers (`/usr/include/bpf/bpf_core_read.h` absent), so either `sudo apt install libbpf-dev`
on the RPi5 (ASK Bryan first -- installing a package) or cross-build in the container (`make BTF=<rpi5
vmlinux.btf>` after `bpftool btf dump file /sys/kernel/btf/vmlinux format raw`... actually dump the
RPi5's `/sys/kernel/btf/vmlinux` and build the skeleton against it). The RPi5 HAS
`/sys/kernel/btf/vmlinux` + `clang` + `CONFIG_SCHED_CLASS_EXT` (sched_ext state currently `disabled`).
The loader binary is self-contained (embeds the BPF via the skeleton) -> build in the container, copy
the binary to the RPi5, `sudo ./scx_aios` (root; Bryan authorized passwordless sudo). VALIDATE: attach
-> `/sys/kernel/sched_ext/root/ops`=aios + `state`=enabled; run the FULL AIOS gate WHILE attached (must
stay `linux=0 seccomp=0`); confirm `aios_enq >> 0` (AIOS tasks tagged + prioritised); detach ->
`state`=disabled, reverts cleanly. Adversarially REVIEW the BPF (verifier-safety of the ancestry loop +
CO-RE reads; a starvation analysis of strict priority).

OTHER arcs still open (Bryan's later call): a "boots into AIOS" RPi5 appliance deploy; finish net
confinement (bind/listen); `pal_sel4.c` (the eventual soul). Commit + gate + RPi5-validate + adversarial-
review each milestone. (A pre-existing flaky test, test/login_pty.c whoami-capture, is being hardened in
a dedicated session -- re-run the gate if ONLY `login`/`execjail` flakes on loaded colima; the RPi5 is
authoritative.)

DEV LOOP: colima -- `UK=/Users/bryan/Desktop/github_repos/AIOS/uk; docker run -d --platform
linux/arm64 --cap-add=SYS_PTRACE -v "$UK":/uk -w /uk gcc:13 sh -c 'stdbuf -oL sh gate.sh >
/uk/scratch_gate.log 2>&1'` then poll `$UK/scratch_gate.log` for `RESULT:` (rm before commit). RPi5 --
my Mac key + passwordless sudo are installed: `rsync -az --delete <build-artifact excludes> uk/
aios@tkrpi5.local:~/uk/` then `ssh aios@tkrpi5.local 'cd ~/uk && rm -f gate_hw.log && nohup setsid sh
-c "sh gate.sh > gate_hw.log 2>&1" &'` + poll. GOTCHAS: ABSOLUTE .../AIOS/uk path for docker -v (git
drifts $PWD); gate log INTO uk/ (colima only mounts $HOME); the GATEWAY for seccomp; .pal.stamp forces
a PAL-switch rebuild; the intermittent seccomp dash/pty stall (timeout-guarded -- re-run if ONLY
`login` flakes, it is NOT your diff); gcc 14/15 stricter than 13; NO apostrophes in a `sh -c '...'`
body; macOS has no `setsid`/`timeout` (detach + poll on the Pi); the single-threaded kernel must NOT
block -- inc 3 established the socket park/wake (pal_net_watch_* + ppoll co-wait + net_attempt); a
blocking DNS resolver (inc 4) must respect the same discipline.
