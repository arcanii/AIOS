# HANDOVER -- session 27 (2026-07-02..03): the SCHEDULER trilogy + net bind/listen + boots-into-AIOS + two real kernel bugs

**HEADLINE: eight milestones on `main`, v0.5.37 -> v0.5.40.** Session 27 evolved AIOS's own CPU scheduler
through three layers, finished network-access confinement, made the RPi5 *boot into AIOS*, and -- the two
most valuable finds -- root-caused + fixed **two real intermittent kernel bugs** (a host-tracee zombie
leak, and an exec `ENAMETOOLONG` that was the true cause of the long-standing "flaky login"). Every
milestone: commit on `main` + `sh gate.sh` green BOTH PAL backends on colima AND the RPi5 + an adversarial
Workflow review (find->verify) BEFORE commit. Bryan pushes; some commits were unpushed at wrap.

Continues the 2026-06-24 pivot (AIOS as a gVisor-style userspace kernel on Linux; verified seL4-on-x86-64
is the destination; verification is the soul; programs see only the AIOS ABI, the host behind a narrow
PAL). Prior: docs/HANDOVER_20260702_session26.md (networking arc complete).

## What shipped (8 commits on `main`)

1. **`dad8cbe` -- scx_aios is AIOS-AWARE.** Evolved uk/sched_ext/scx_aios from a flat global-FIFO into a
   policy that PRIORITISES the AIOS workload: tag a task AIOS if it or an ancestor within 8 `real_parent`
   hops has comm `aios-uk` (CO-RE `BPF_CORE_READ`, unrolled bounded loop, verifier-clean) -> a HIGH DSQ
   drained before a NORMAL DSQ; 4 mmap'd `.bss` counters. RPi5: verifier accepts (nr_rejected=0), the full
   gate passes BOTH backends while scx_aios owns the host, aios_enq=207 (=hi_dispatch), detach reverts.
2. **`20c24b7` -- NET confinement bind/listen (v0.5.38).** `AIOS_NET_BIND_ALLOW` gates which LOCAL
   addr:port a guest may CLAIM (the net analogue of connect's `AIOS_NET_ALLOW`): pal_host_bind refuses a
   disallowed bind -EACCES; pal_host_listen has an AUTO-BIND GUARD (getsockname vs the list, so an unbound
   listen() can't auto-bind an ephemeral 0.0.0.0 port past the gate). Parser+matcher FACTORED for both
   lists; a fail-closed fix (a SET-but-EMPTY list now denies all). Gate key `bindjail`. The NET arc's
   confinement is now COMPLETE for reach (connect) AND claim (bind/listen).
3. **`4efd60e` -- BOOTS INTO AIOS on the RPi5 (systemd light path).** uk/appliance/rpi5/: aios-console.service
   hands tty8 to `aios-uk /opt/aios/aiosroot/sbin/init` at boot WITHOUT touching the boot chain / tty1
   getty / ssh; fully reversible. StandardInput=tty+TTYPath give a controlling terminal the getty way (no
   setsid/TIOCSCTTY wrapper -- confirmed by aios-uk running `Ss+` = session leader). ENABLED + REBOOTED
   (Bryan-authorized) -> the RPi5 comes up with the live AIOS login on tty8 (/dev/vcs8 confirmed).
4. **`dd46308` -- host-tracee REAPING fix, CLONE_PARENT (v0.5.39).** A guest that forked a child which
   exited while the forking guest lived left a lingering HOST ZOMBIE (the sysinit `echo` on the console was
   the visible one). Root cause (empirical ptrace probe): the child's tracer is aios-uk but its real parent
   was the forking guest; a tracer!=real-parent tracee lingers as the real parent's zombie after the tracer
   reaps it, and a guest's wait() is a virtualized AIOS syscall (never a host waitpid). Fix: pal_guest_fork
   injects `clone(SIGCHLD | CLONE_PARENT)`, so aios-uk is the real parent of every guest and its single
   waitpid(-1) fully reaps them. AIOS semantics unchanged (proc_t.parent_pid still records the forker).
5. **`58ff44a` -- scx_aios ANTI-STARVATION VALVE.** Strict HIGH-first could starve NORMAL/sshd if AIOS
   saturates every CPU. Now: while HIGH+NORMAL both have work, if NORMAL has gone unserved > 50ms
   (`AIOS_NORMAL_STARVE_NS`, tunable), serve NORMAL first this cycle then reset. So NORMAL can't be starved
   (bounded ~N*window/task) while AIOS keeps ~90%. A 5th counter `valve_fires` is a true saturation
   signature (0 on idle -- the timer ages only while BOTH queues have work). RPi5 stress (6 AIOS hogs on 4
   CPUs): a NORMAL probe kept progressing, valve_fires=317 vs 2878 HIGH.
6. **`4058dc9` -- scx_aios LAYER 3: weighted VTIME fairness.** Within each DSQ, weighted virtual time
   replaces FIFO (the scx_simple pattern): .enqueue insert_vtime, .stopping charges `used*100/weight`,
   .running advances a global clock, an UNCONDITIONAL one-slice clamp seeds new tasks + wipes banked sleep
   credit (incl. a review-confirmed default-select-cpu direct-dispatch bypass). Per-guest `nice` weights
   WORK: bursty guests split ~3:1 (nice+5) / ~25:1 (nice+15); FIFO gives 1:1. Documented caveat: two PURE
   CPU hogs on one CPU ping-pong ~1.3:1 (never co-queued, so vtime can't compare them) -- not the AIOS norm.
7. **`47d2da8` -- EXEC ENAMETOOLONG fix (v0.5.40) -- the REAL "flaky login" root cause.** Under load ~2-4%
   of GUEST EXECS failed with ENAMETOOLONG (login couldn't exec its shell, init couldn't exec /bin/login,
   dash couldn't exec whoami). pal_guest_exec stages the exec PATH into the guest's own memory then injects
   a Linux execve; the old stage_str wrote below sp CLAMPED to the stack-page start and bailed
   (-ENAMETOOLONG) whenever sp sat within a path-length of a stack-page BOTTOM at exec time -- random per
   exec. Fix: stage into the current (always-mapped) page with room + SAVE the overwritten bytes to RESTORE
   on execve failure. Proven 200/200 across both backends on colima AND the RPi5 under load, zero
   ENAMETOOLONG.
8. **`95e4c81` -- login_pty HARDENED.** The flaky test/login_pty.c (which "a dedicated session owned" --
   Bryan made THIS session it) rewritten EXPECT-STYLE: accumulate pty output and wait for each token
   (`login:`, `Password:`, and the `who=aios` OUTPUT before `exit`, so logout can't race the whoami output)
   with generous ceiling deadlines (sum < the gate timeout, bumped 25s->60s). This rewrite is what surfaced
   the exec bug (clear per-step failure reporting). Now committed (no longer an uncommitted working change).

## Validation

- **colima** (gcc:13 container, `sh gate.sh`) + **RPi5** (`aios@tkrpi5.local`, Ubuntu 26.04, kernel 7.0,
  gcc 15): every milestone RESULT `linux=0 seccomp=0` BOTH backends.
- **sched_ext** (scx_aios) cross-built in an ubuntu:26.04 container against the RPi5's BTF (the RPi5 ships
  only gcc); the self-contained loader copied over + `sudo ./scx_aios`; the full gate passes while it owns
  the host; detach reverts. The container does NOT run the verifier -- the RPi5 load is the real test.
- **Adversarial Workflow review** (find->verify, 2-3 lenses) on every milestone: caught the fail-closed
  empty-list gap (bind/listen), confirmed the CLONE_PARENT/reaping semantics, the valve's counting +
  starvation-freedom, the vtime direct-dispatch clamp bypass (already closed by the unconditional clamp),
  and 0 findings on the exec fix. Net: real bugs caught pre-commit; keep doing this.
- **RPi5 persistent state (Bryan-OK'd):** /opt/aios re-staged at **v0.5.40** (exec fix + reaping fix) +
  the enabled aios-console.service (console logs into AIOS on tty8, no zombies, no exec flake). Revert:
  `sudo systemctl disable --now aios-console; sudo rm -rf /opt/aios /etc/systemd/system/aios-console.service`.

## KEY LESSONS (carry forward)

- **Empirical HW testing earns its keep.** The scheduler valve's `valve_fires` counted idle noise until a
  both-queues-have-work gate fixed it; the "flaky login" was a REAL kernel exec bug, not a test race,
  found only by running the rewritten test under load and reading LP_DEBUG. Prefer a probe/stress over
  reasoning for anything timing/ptrace/scheduler-shaped.
- **vtime weighting subtleties:** the weight advantage comes from a LIGHT task's vtime racing AHEAD, not
  the heavy banking behind; the enqueue clamp must catch the default-select-cpu wake-to-idle direct-dispatch
  path (which skips ops.enqueue), so it must be UNCONDITIONAL, not wakeup-gated.
- **exec path staging** must land in the current (mapped) stack page with save/restore -- never assume a
  fixed offset below sp is available (sp can sit anywhere in its page).
- **Stray-process gotchas that cost many ssh-drops:** a `( while :; ) &` spinner's cmdline is the PARENT
  SCRIPT's name (NOT "while :"), so `pkill -f "while :"` never matches it -- kill by the script name / high
  %cpu. And `pkill -f <pattern>` self-matches your own ssh command line. Always clean stray hogs before a
  scheduling A/B (they contaminate results + starve guest-spawn).

## Dev loop (carry forward)

- **ptrace kernel:** colima -- `UK=/Users/bryan/Desktop/github_repos/AIOS/uk; docker run -d --platform
  linux/arm64 --cap-add=SYS_PTRACE -v "$UK":/uk -w /uk gcc:13 sh -c 'stdbuf -oL sh gate.sh >
  /uk/scratch_gate.log 2>&1'` then poll `$UK/scratch_gate.log` for `RESULT:` (rm before commit). RPi5 --
  my Mac key + passwordless sudo installed: `rsync -az --files-from=<git ls-files uk/> uk/
  aios@tkrpi5.local:~/uk/` then `ssh ... 'cd ~/uk && rm -f gate_hw.log && nohup setsid sh -c "sh gate.sh >
  gate_hw.log 2>&1" &'` + poll.
- **sched_ext (DIFFERENT):** colima's 6.8 kernel CANNOT load sched_ext -- cross-build the BPF in an
  ubuntu:26.04 container (apt-get install clang llvm libbpf-dev bpftool) against the RPi5's world-readable
  `/sys/kernel/btf/vmlinux` (scp it out, `make BTF=vmlinux.btf`), copy the self-contained loader over, `sudo
  ./scx_aios`. Build context MUST be under $HOME (colima only mounts $HOME).
- GOTCHAS: ABSOLUTE `.../AIOS/uk` path for docker -v (git drifts $PWD); gate log INTO uk/; the GATEWAY for
  seccomp; `.pal.stamp` forces a PAL-switch rebuild; NEVER edit a source while a gate/build reads it off the
  live virtiofs mount; gcc 14/15 stricter than 13; NO apostrophes in a `sh -c '...'` body; NO `*/` in a C
  block comment; macOS has no setsid/timeout (detach + poll on the Pi, ssh ConnectTimeout on the Mac).

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue building AIOS as a **gVisor-style userspace kernel on Linux** (the 2026-06-24 pivot off
seL4/RPi4 -- Linux is the interim substrate, verified seL4-on-x86-64 is the destination; verification is
the soul; programs see only the AIOS ABI, the host sits behind a narrow PAL). READ FIRST: memory
[[project_pivot_linux_userspace_kernel]] + docs/HANDOVER_20260703_session27.md + uk/README.md +
uk/sched_ext/README.md + uk/appliance/rpi5/README.rpi5.md.

WORKING BRANCH = `main` (commit per milestone on main, Bryan pushes; confirm unpushed commits with Bryan).
The `uk/` tree: a host-agnostic kernel (kernel/aios_kernel.c includes ONLY aios_abi.h + aios_version.h +
pal.h) over a SHARED Linux host-driver core (pal/pal_linux_common.c) + TWO trap front-ends (pal/pal_linux.c
= PTRACE_SYSCALL, pal/pal_seccomp.c = seccomp RET_TRACE; `make PAL=linux|seccomp`) + libaios + shadow
standard headers (lib/include, -nostdinc). NEVER patch vendored (dash/sbase) -- grow libaios.

DONE through **v0.5.40, 62-syscall ABI**: OPERATIONAL (vendored dash + 28 sbase utils UNMODIFIED) + the
boundary COMPLETE + FULL job control + raw termios + a SECOND PAL backend (seccomp via the GATEWAY) + a
minimal Linux-6.18 QEMU appliance + the SYSTEM LAYER COMPLETE (init->login->session->logout->respawn,
per-process uid/gid, login SWITCHES USER, crypt() SHA-512, a root-gated SHUTDOWN, /etc/inittab) + the
NETWORKING ARC COMPLETE (client/server/non-blocking/DNS + confinement for reach=connect AND
claim=bind/listen) + AIOS OWNS SCHEDULING (uk/sched_ext/scx_aios: AIOS-aware comm-ancestry tag -> HIGH DSQ
+ an anti-starvation valve + weighted-vtime per-guest `nice` fairness; a BPF struct_ops, HW-validated) +
BOOTS INTO AIOS on the RPi5 (enabled aios-console.service on tty8, /opt/aios at v0.5.40). Two intermittent
kernel bugs fixed this session: host-tracee zombie leak (CLONE_PARENT) and exec ENAMETOOLONG (path
staging). Gate keys incl. net netsrv netloop rcvtimeo dns netjail bindjail jobctl ctrlc/z rawkey login.
ENTIRE tree HW-validated on the RPi5 (`sh gate.sh` -> linux=0 seccomp=0 both backends). Each milestone
adversarially reviewed by a Workflow BEFORE commit -- real bugs got caught; KEEP DOING THIS.

PRIMARY TASK (Bryan picked, deferred from s27) -> **pal_sel4.c, the eventual soul.** This is a genuine
SCOPE FORK to settle with Bryan first (I flagged both; Bryan deferred the whole arc to a fresh session):
(a) a SEAM-PROVING SCAFFOLD -- write pal/pal_sel4.c implementing the full include/pal.h interface as
documented stubs, each stating its seL4 proof obligation (caps for memory instead of mmap injection; a
fault-handler/VMM for transparent syscall trapping, since seL4 has NO ptrace; a root task setting up the
guest's vspace/threads/caps), compiling via `make PAL=sel4` (the Makefile already routes
PAL=sel4 -> pal/pal_sel4.c, which -- UNLIKE the two Linux backends -- would NOT include
pal_linux_common.c). This PROVES the kernel is genuinely host-agnostic (kernel/aios_kernel.c compiles +
links against a THIRD, non-Linux PAL backend) and becomes the proof-obligation document for the eventual
verified boundary -- the M6 "prove the PAL seam" milestone, bounded + in-session, does NOT run seL4. VS
(b) a REAL seL4 port -- needs the seL4 SDK, an x86-64-or-seL4-board target, and a VMM, none set up yet; a
multi-session epic + infra decisions (target, toolchain). CONFIRM the scope with Bryan before sinking
effort; the scaffold is the honest bounded deliverable.

OTHER arcs still open (Bryan's later call): IPv6 / a fuller net stack; a finer scx_aios policy still
(explicit per-guest weight tags beyond nice); more of the userland (a real editor -- vi/ed absent). The
RPi5 aios-console persistent deploy is live at v0.5.40; get explicit OK before touching persistence/keys
anywhere new.

DEV LOOP: see the "Dev loop" section above (ptrace kernel via colima gcc:13 / RPi5 rsync+gate; sched_ext
cross-built in an ubuntu:26.04 container against the RPi5 BTF -- colima can't load sched_ext). GOTCHAS:
ABSOLUTE .../AIOS/uk path for docker -v; gate log INTO uk/; the GATEWAY for seccomp; .pal.stamp;
gcc 14/15 stricter; NO apostrophes in a `sh -c '...'` body; a `( while :; ) &` spinner's cmdline is the
SCRIPT name (kill stray hogs by script name / %cpu before any scheduling A/B); `pkill -f` self-matches
your own ssh command line.
