# HANDOVER -- session 18 (2026-06-25): the PROCESS MODEL (fork/exec/wait/pipe) + an AIOS shell

Continuation of the s17 pivot (AIOS as a gVisor-style userspace kernel on Linux). This session built
the **entire process model** on the `uk/` tree and capped it with a real shell. The userspace kernel
went from "runs one program at a time" to "multi-process, runs pipelines of real programs through an
AIOS shell." Live state: memory [[project_pivot_linux_userspace_kernel]]. Design:
docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md. Branch `userspace-kernel` (LOCAL -- Bryan
pushes).

## What shipped (4 commits, each validated on colima AND native RPi4 / Linux 6.12)

- **M3d.1 -- exec (`4065dbf`).** `AIOS_SYS_EXEC` (0x1008) replaces a guest's image by rewriting its
  trapped `svc` in place into a Linux `execve` (using the guest's own path/argv/envp pointers).
  execve has no normal return on success -> `PTRACE_O_TRACEEXEC` gives a `PTRACE_EVENT_EXEC` stop,
  after which the new image's first syscall traps as usual; on failure the guest gets x0 = -1.
  - **Driver hardening this forced:** the injected execve leaves a trailing syscall-exit stop in the
    new image, which desynchronised the driver's assumed strict entry/exit alternation (every
    post-exec write was read at its EXIT -> targeted a bogus fd -> no output). Fixed at the root:
    `trap_next` now classifies each stop with **`PTRACE_GET_SYSCALL_INFO`** (Linux >= 5.3) and
    resumes past anything that is not a genuine syscall ENTRY. The driver no longer assumes
    alternation -- injected syscalls may leave stray exit/event stops without desync. (Also set up
    fork's event stops.)
  - libaios grew `aios_execve`/`aios_exec` + a POSIX `environ` captured at `_start`.

- **M3d.2 -- fork + wait + exit (`f9c26d2`).** The big refactor: the kernel was one global guest +
  one global fd table; it is now genuinely multi-process.
  - **PAL went pid-keyed** (the future verified seam): `pal_guest_spawn -> pid`, `pal_guest_next`
    (= `waitpid(-1)`), `pal_guest_return(pid,ret)`, `pal_guest_resume(pid)`, read/write/mmap/exec
    take a pid, `pal_guest_fork(parent) -> child`, `pal_guest_exit(pid,code)`. Resume is owned by
    return/resume, waiting by next -- decoupled (the standard multi-tracee shape). Per-pid registers
    (no global `g_regs`).
  - **fork** = injected `clone(SIGCHLD)` + `PTRACE_O_TRACEFORK` + `GETEVENTMSG`; BOTH register files
    restored to the guest's saved state with x0 = child pid in the parent, 0 in the child (POSIX's
    two returns). **exit** = injected `exit_group` (the process really dies; its exit drives reap/
    wake bookkeeping). Options set on every forked child so its own syscalls + fork/exec trace.
  - **Kernel:** a process table (state, parent, exit code, wait bookkeeping) + a **refcounted
    open-file table** under per-process fd tables -- so fds shared across fork keep correct close
    semantics. WAIT reaps a matching zombie now, or **parks** the caller (a "blocked" wait is just
    "don't resume" -- the loop keeps servicing others and wakes the parent when its child exits).
  - Tests: `prog_fork` (N children fork/exit/wait-any, gate sum 6), `prog_spawn` (the shell core:
    fork -> child exec -> parent waitpid -- the per-command pattern dash runs).

- **M3d.3 -- pipe + dup2 (`35bcf82`).** Completes the quartet. The kernel is single-threaded over all
  guests, so a read on an empty pipe / write on a full one must NOT block it: pipe ends are
  **non-blocking** at the host (`pal_host_pipe`), and the kernel **parks** the guest
  (`PS_BLOCKED_READ/WRITE`, stashing buf/len/done) and services others. `pipe_settle()` is a tiny
  non-recursive fixpoint that re-runs parked guests when a peer makes the pipe ready (or closes its
  end -> EOF / broken pipe); the bulk of a transfer is driven by the peers' own guest-level loops,
  each re-triggering settle. PAL stays host-agnostic: read/write report `PAL_EWOULDBLOCK`/`PAL_EPIPE`
  (not errno), and the kernel ignores SIGPIPE.
  - **Bug fixed:** a process's fds were never released on exit, so a pipe write end held by a dying
    writer (e.g. a stage that dup2'd it onto stdout) never closed and the downstream reader hung for
    EOF. `on_exit` now releases the dying fd table via a shared `fd_release()` helper (also used by
    close/dup2), waking pipe peers. This is what made `prog_pipeline` stop hanging.
  - Tests: `prog_pipe` (reader parks then drains, EOF on close), `prog_pipeline`
    (`prog_args | prog_wc` via pipe+dup2+exec -> "7 30 210"), `prog_pipebig` (200 KB through a 64 KB
    pipe -> writer + reader parking, verified byte count + checksum).

- **M3d.4 -- prog_sh (`361fbb0`), the capstone.** A minimal shell, pure libaios over the existing
  ABI (no kernel/PAL changes -- the point being the process model was already complete). Reads a
  line, splits on `|`, tokenises, builds pipes, dup2's stdin/stdout onto pipe ends, fork+execs each
  stage, waits all. `./prog_args one two | ./prog_wc | ./prog_wc` works (3-stage). Prompt on stderr;
  ends on `exit` or stdin EOF. Minimal on purpose (no quoting/glob/redirection/$vars/PATH).

**AIOS ABI now:** WRITE/READ/OPEN/CLOSE/EXIT/MMAP/FSTAT/LSEEK/EXEC/FORK/WAIT/PIPE/DUP2. libaios has
printf, malloc, string/ctype, exec/fork/wait/waitpid/pipe/dup2, `environ`, WEXITSTATUS.

## Dev loop (unchanged, working)
- `uk/run.sh` builds + runs the whole M1..M3d suite in an aarch64 `gcc:13` colima container
  (`--cap-add=SYS_PTRACE`); overall rc=0 is the gate.
- Native RPi4: `scp -r uk pi@192.168.0.8:~/ && ssh pi@192.168.0.8 'cd ~/uk && make && ./aios-uk <p>'`
  (login pi/aios, keyless SSH; Linux 6.12). A ptrace hang is SILENT -> in-container `timeout N`
  (hang -> rc=124) + `fprintf(stderr)` in pal_linux.c. NOTE the container `sh` is dash: no
  `${PIPESTATUS[*]}` (use `>/tmp/x; rc=$?`).

## Key technical notes for next time
- **`PTRACE_GET_SYSCALL_INFO` is the spine of the driver** -- `trap_next`/`pal_guest_next` return
  only on op==ENTRY and resume past everything else, so injection artifacts (the trailing
  execve/clone exits, fork event stops) never desync dispatch. Don't reintroduce alternation
  assumptions.
- **fds must be released on process exit** (pipe EOF depends on it) -- `fd_release()` + `on_exit`.
- **AIOS pids == Linux tracee pids** for now (the injected clone's natural returns are already
  correct). Pid virtualization is a later (M4-ish) concern.
- Concurrent guests genuinely interleave (the kernel round-robins per syscall via `waitpid(-1)`), so
  multi-writer stdout is char-interleaved -- cosmetic, not a bug.

## NEXT -> fully operational (the parallel track the seed always pointed at)
The process model is DONE; the remaining road to "operational = dash" is the **libaios -> sbase ->
dash retarget**:
1. Grow `libaios` into the AIOS-ABI retarget of the seL4 `libaios_posix` (on `main`). Ideally
   **shadow standard headers** (string.h/stdio.h/unistd.h/fcntl.h/...) under `uk/lib/include` and
   compile real sources with **`-nostdinc`** so `sbase`/`dash` compile UNMODIFIED. Fill the libc
   surface they need (stdio FILE\* + buffering, full string/ctype, errno, env, getopt, etc.).
2. Recompile **sbase** against it (start with the utilities prog_sh already wants: echo, cat, ls,
   wc...). Then **dash**. The ABI likely needs a few more syscalls (getcwd/chdir, dup, stat-by-path,
   getpid, brk or keep mmap-malloc, maybe a vfork/clone-flavoured fast path, signals).
3. Then **M4** enforce the boundary (seccomp/namespaces so a guest's stray real-Linux syscall is
   *forbidden*, not just ENOSYS'd) · **M5** sched_ext (custom RPi kernel w/ CONFIG_SCHED_CLASS_EXT) ·
   **M6** `pal_sel4.c` (the replant seam -- prove the now-much-larger PAL on a second host).

The seL4 stall + lead-#3 keyboard stay mooted by leaving the platform (seL4 tree preserved on `main`).

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue building AIOS as a **gVisor-style userspace kernel on Linux** (the 2026-06-24 PIVOT off
seL4/RPi4 -- Linux is the interim substrate, verified seL4-on-x86-64 is the destination; verification
is the soul; programs see only the AIOS ABI, the host sits behind a narrow PAL). READ FIRST: memory
[[project_pivot_linux_userspace_kernel]] + docs/HANDOVER_20260625_session18.md +
docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md + uk/README.md.

WORKING BRANCH = `userspace-kernel` (commits M1..M3d.4, LOCAL -- Bryan pushes). The `uk/` tree: a
host-agnostic kernel (kernel/aios_kernel.c includes ONLY aios_abi.h + pal.h) over the ONLY host-aware
file (pal/pal_linux.c, a multi-process PTRACE_SYSCALL driver) + libaios (lib/libaios.{c,h}, a C
runtime on the AIOS ABI). DONE: M0..M3c (first-light, VFS, argv, libaios, wc/tail/bigalloc) and the
**entire PROCESS MODEL (M3d): exec, fork, wait, exit, pipe, dup2** -- the kernel is multi-process
(process table, waitpid(-1) loop, pid-keyed PAL, refcounted open-file table, park/wake for wait +
pipes), and **`prog_sh` is a working AIOS shell running real multi-stage pipelines** (`./prog_args a |
./prog_wc | ./prog_wc`). AIOS ABI: WRITE/READ/OPEN/CLOSE/EXIT/MMAP/FSTAT/LSEEK/EXEC/FORK/WAIT/PIPE/
DUP2. The driver classifies stops with PTRACE_GET_SYSCALL_INFO (never assume entry/exit alternation);
fds are released on exit (pipe EOF depends on it). Validated colima + native RPi4 (Linux 6.12).

DEV LOOP: `uk/run.sh` (colima aarch64 container, --cap-add=SYS_PTRACE; overall rc=0 gates M1..M3d);
HW-validate via `scp -r uk pi@192.168.0.8:~/ && ssh pi@192.168.0.8 'cd ~/uk && make && ./aios-uk
<prog>'` (login pi/aios, keyless SSH; Linux 6.12). DEBUG: a ptrace hang is SILENT -> in-container
`timeout N` (hang -> rc=124) + fprintf(stderr) in pal_linux.c. Container sh is dash (no
${PIPESTATUS[*]}).

PRIMARY TASK -> **fully operational = dash**, via the libaios -> sbase -> dash retarget. Grow libaios
into the AIOS-ABI retarget of the seL4 `libaios_posix` (on `main`): shadow standard headers under
uk/lib/include and compile real sources with `-nostdinc` so sbase/dash compile UNMODIFIED; fill the
libc surface (stdio FILE\*+buffering, full string/ctype, errno, env, getopt...). Recompile **sbase**
(echo/cat/ls/wc first), then **dash**. Expect to add a few ABI syscalls (getcwd/chdir, dup, stat-by-
path, getpid, signals, maybe brk). Keep kernel/aios_kernel.c host-agnostic and the PAL seam minimal
(it's the future verified boundary). Commit per milestone on `userspace-kernel`; validate colima + Pi
each step; Bryan pushes.

THEN: M4 enforce the boundary (seccomp/namespaces so a guest CANNOT bypass the kernel -- today a
stray real-Linux syscall just ENOSYSes); M5 sched_ext (stock RPi kernel lacks CONFIG_SCHED_CLASS_EXT
-> custom kernel); M6 pal_sel4.c (the replant seam). The seL4 stall + lead-#3 keyboard are MOOTED by
leaving the platform (seL4 tree preserved on main as record/fallback).
