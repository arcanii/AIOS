# NEXT 2026-06-06b -- fork-free SSH shell spawn (the reconnect fix)

Seed for a focused session. SSH is recovered + always-on (v0.4.177 work, committed:
f0078b4 / ddb80cb / 0b21761 / 2cc5c01). This doc is ONLY about the remaining
**reconnect** limitation: the 2nd+ SSH connection per boot fails.

## What the bug IS (proven this session -- do not re-chase)
- The shell `fork()` in `ssh_channel.c:spawn_shell` corrupts a **living** sshd's
  server IPC. conn1 works; conn2 fails. sshd's PID stays constant (never reaped).
- PROVEN by isolation: if conn1 fails auth (NO shell fork), conn2 works; if conn1
  forks the shell, conn2 fails. So the FORK is the trigger.
- NOT COW: `COW_STRIP_PARENT=0` did not help; sshd is never reaped.
- NOT the SMP allocator race: pinning all root threads to core 0 (affinity pin,
  shipped this session, `boot_services.c` -- KEEP it as latent-SMP hardening)
  did NOT help, AND the single-core RPi4 fails too (one core cannot race).
- BOTH background "expert" analyses (COW, then SMP-race) were confidently WRONG.
  The true mechanism is a single-core fork->server-IPC corruption and resists
  every instrument AIOS allows (fd routing, block-cache inode lag, console
  misalignment all defeat tracing). QEMU symptom: "key exchange failed". RPi4
  symptom: "banner timeout" (sshd stuck before accept). Same trigger.

## The fix: don't fork. Spawn dash fresh with the relay pipes.
exec_server's EXEC_RUN already spawns a fresh process from the ELF (no fork,
`sel4utils_spawn_process_v`). Removing the fork removes the proven trigger.
HIGH confidence (it deletes the trigger, independent of the unknown mechanism).
Verify on QEMU first: `scripts/ssh_qemu_reconnect.py` should go all-PASS
(QEMU reproduces the bug). Then flash + retest on the Pi.

## Mechanism is mostly already there
1. **Userspace already wires pipes from argv.** `__wrap_main` (src/lib/aios_posix.c
   :786-824) parses argv[8] = `uid:gid:[spipe:rpipe:]/path` and sets
   `stdout_pipe_id`/`stdin_pipe_id` (99 = no redirect). So a fresh-spawned dash
   gets its fd0/fd1 pipes IF the spawner injects `spipe:rpipe:` into that string.
   NO aios_posix change needed.
2. **exec_server already builds that string** (cwd_buf = child_argv[8],
   exec_server.c:369-403, currently `uid:gid:/path`). Extend it to insert
   `spipe:rpipe:` after `uid:gid:`.
3. **Refcount pattern is known** -- replicate PIPE_EXEC (pipe_server.c:1227-1263):
   set `ap->stdout_pipe_id`/`stdin_pipe_id`; `pipes[stdout].write_refs++` (>=1),
   `write_closed=0`, `pipe_had_child=1`; `pipes[stdin].read_refs++` (>=1),
   `read_closed=0`, `pipe_had_child=1`. Expose a non-static helper
   `pipe_exec_assign(ap_idx, stdout_pipe, stdin_pipe)` in pipe_server.c and call
   it from exec_server (same address space; affinity-pinned so no race).

## The wrinkle: sshd has pipe_ep but NOT exec_ep
Children are given ser/fs/thread/auth/pipe/net/disp/crypto only (exec_server.c
:346-367 + __wrap_main). No userspace process holds exec_ep today. So sshd cannot
call exec_server directly. Two ways:
- **(a) Recommended: give children exec_ep.** Add a `child_exec` cap copy in
  exec_server's child-argv block, parse it in `__wrap_main` into a userspace
  `exec_ep` global, and add a userspace `aios_exec_piped(path, stdout_pipe,
  stdin_pipe)` that sends EXEC_RUN (+2 pipe-id MRs) to exec_ep and returns the
  pid. Smaller; reuses exec_server's existing fresh-spawn. AIOS is root-only so
  the added privilege is acceptable. (Could gate to sshd only if desired.)
- **(b) New pipe_server primitive** `PIPE_SPAWN_PIPED` over pipe_ep (which sshd
  has) that does a fork-free fresh spawn + wiring. Avoids broadening exec_ep but
  needs the ELF-load/configure/spawn logic inside pipe_server (check whether
  PIPE_EXEC's spawn is reusable or delegates) -- likely bigger.

## Userspace + sshd
- Add `int aios_fd_pipe_id(int fd)` (returns `aios_fds[fd-AIOS_FD_BASE].pipe_id`)
  so sshd can get a pipe's id from its fd.
- `ssh_channel.c:spawn_shell`: `pipe2(in)`, `pipe2(out)`; spawn dash via
  `aios_exec_piped("/bin/dash -i", out_pipe_id, in_pipe_id)` (dash writes fd1->out
  which sshd reads via out[0]; dash reads fd0<-in which sshd writes via in[1]);
  then `close(in[0])` + `close(out[1])`; keep `in[1]=stdin_wr`, `out[0]=stdout_rd`;
  relay as today; `waitpid(pid)`. NO fork.

## Risks
- Pipe refcount order: increment dash's ref BEFORE sshd closes its copy of that
  end, or the pipe latches *_closed -> early EOF. (PIPE_EXEC handles the same.)
- exec_ep broadening (option a) -- acceptable in AIOS, note it.
- Root-task change -> needs `ninja -C build-04` (QEMU) + `ninja -C build-rpi4`
  + mkdisk + mksdcard + a balenaEtcher FLASH to verify on the Pi.

## Test
`scripts/ssh_qemu_reconnect.py` (QEMU, -smp 4) must go all-PASS. Then flash; on the
Pi do sequential `ssh -tt -p 2222 root@192.168.0.8` (password root) several times.
