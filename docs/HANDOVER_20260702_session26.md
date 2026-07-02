# HANDOVER -- session 26 (2026-07-02): NETWORKING inc 2 -- AIOS gets a socket SERVER

**HEADLINE: AIOS can now LISTEN.** Networking increment 2 adds the TCP **server** surface --
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

## NETWORKING -- what's next (the recommended order, unchanged)

1. **DONE (s25): inc 1 -- a TCP CLIENT** (SOCKET/CONNECT).
2. **DONE (s26): inc 2 -- a TCP SERVER** (BIND/LISTEN/ACCEPT + SETSOCKOPT/GETSOCKNAME).
3. **NEXT: inc 3 -- NON-BLOCKING + park/wake (THE crux).** Integrate socket readiness into the
   kernel's event loop like the pipe park/wake, so network I/O no longer blocks other guests (removes
   today's single-guest limit). The hard part: the single-threaded kernel must NOT block in
   `pal_guest_next`/`accept`/`read`. Today `pal_guest_next` blocks in `waitpid(-1)` for the next
   tracee event; to also wake a guest parked on a socket, the PAL needs to co-wait tracee-events AND
   host-fd readiness (e.g. `ppoll`/`signalfd(SIGCHLD)` + non-blocking sockets, then `waitpid(WNOHANG)`
   to drain the actual ptrace stop). New kernel states like `PS_BLOCKED_READ` already exist for pipes;
   the model to copy is `do_read`/`pipe_settle`, generalized to a socket-readiness source. This is a
   real reworking of the PAL's core wait -- design it carefully; a ptrace hang is SILENT
   (timeout-guard + fprintf while developing).
4. **THEN: DNS** -- a resolver so programs use hostnames not dotted quads (simplest = a PAL
   passthrough to host `getaddrinfo`; the "grow our own" path = a from-scratch UDP resolver).
5. **THEN: network-access CONFINEMENT** -- which hosts/ports a guest may reach, a PAL policy analogous
   to M4.2 fs confinement (`AIOS_ROOT`).

Other endgame arcs (Bryan's call): scx_aios AIOS-AWARE policy; a "boots into AIOS" RPi5 appliance
deploy (systemd/getty light path); `pal_sel4.c` (BACKLOGGED -- the eventual soul).

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

DONE through **v0.5.33, 62-syscall ABI**: OPERATIONAL (vendored dash + 28 sbase utils UNMODIFIED) +
the boundary COMPLETE + FULL JOB CONTROL + raw termios + a SECOND PAL backend (seccomp via the
GATEWAY) + a minimal Linux-6.18 appliance + the SYSTEM LAYER COMPLETE (inc 1+2: init->login->session->
logout->respawn, per-process uid/gid identity + login SWITCHES USER, crypt() SHA-512 =openssl passwd
-6, a fuller shell incl. uname reports AIOS / a real FLOAT printf =glibc / date / tr / cut / seq, a
root-gated clean SHUTDOWN, config-driven init /etc/inittab) + two endgame subsystems: (1) sched_ext --
uk/sched_ext/scx_aios is AIOS's OWN CPU scheduler as a BPF struct_ops, HW-validated on the RPi5; (2)
NETWORKING inc 1 (a TCP CLIENT: SOCKET/CONNECT) + inc 2 (a TCP SERVER: BIND/LISTEN/ACCEPT +
SETSOCKOPT/GETSOCKNAME) -- host-passthrough behind the boundary (a socket is an AIOS fd backed by a
host socket; ACCEPT returns a NEW AIOS fd); proven by an AIOS client round-tripping a host echo AND an
AIOS echo server a host client connects to (gate keys `net` + `netsrv`), + fetched http://example.com
over the REAL internet from the RPi5. ENTIRE tree HW-validated on the RPi5 (`aios@tkrpi5.local`, Ubuntu
26.04, kernel 7.0, gcc 15; `sh gate.sh` -> linux=0 seccomp=0). NEVER patch vendored sources -- grow
libaios.

PRIMARY TASK -> **continue NETWORKING, in the recommended order** (each a milestone: commit + gate +
RPi5-validate): **(3) NON-BLOCKING + park/wake -- THE crux.** Integrate socket readiness into the
kernel's event loop (like the pipe park/wake in do_read/pipe_settle) so a blocking socket
read/accept/connect no longer blocks the single-threaded kernel + other guests (removes today's
single-guest limit). The hard part: pal_guest_next today blocks in waitpid(-1) for the next tracee
event; to ALSO wake a guest parked on a host socket, the PAL must co-wait tracee-events AND host-fd
readiness (e.g. ppoll/signalfd(SIGCHLD) + non-blocking sockets, then waitpid(WNOHANG) to drain the
ptrace stop). Reuse the existing PS_BLOCKED_READ machinery; keep kernel/aios_kernel.c host-agnostic
(readiness is a PAL concern). A ptrace hang is SILENT -> timeout-guard + fprintf while developing.
**THEN (4) DNS** (PAL getaddrinfo passthrough, or a from-scratch UDP resolver). **THEN (5)
network-access CONFINEMENT** (which hosts/ports a guest may reach -- a PAL policy analogous to M4.2 fs
confinement). THEN other endgame arcs (Bryan's call): scx_aios AIOS-AWARE policy; the RPi5 "boots into
AIOS" appliance deploy; pal_sel4.c (BACKLOGGED -- the eventual soul).

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
block -- park/wake is the crux of networking inc 3.
