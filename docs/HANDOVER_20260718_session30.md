# HANDOVER — session 30 (2026-07-17..18): Phase D — a REAL SHELL RUNS, and the filesystem became writable

**HEADLINE:** the vendored **dash runs real sbase pipelines on seL4** — `seq 1 5 | wc -l` → 5,
`ls /bin | wc -l` → 31, 3-stage pipelines, command substitution — and since D.3 the filesystem is
**writable**, so shell redirection works: `echo x > /tmp/f`, `>>`, `< file`, `mkdir` + write into it.
Phase D's core goal ("dash + sbase run") is met. Also fixed a real host-agnostic ABI defect the D.2
review surfaced: the wait status never encoded `WIFSIGNALED`.

Continues docs/HANDOVER_20260716_session29.md (Phases 0/A/B/C.1–C.5 — Phase C COMPLETE). Read:
memory [[project_pivot_linux_userspace_kernel]] + docs/PLAN_20260709_sel4_real_port.md +
uk/pal/sel4/{boot.c,tarfs.c,pipe.c}.

## What shipped (3 commits on `main`, all UNPUSHED — Bryan pushes)

1. **`e5da3d5` — D.1: fs breadth + the clock.** Directories now OPEN (the open-file entry carries a
   normalized path + a getdents cursor; `read()` on a dir → `EISDIR`; `O_DIRECTORY` on a file →
   `ENOTDIR`). `pal_host_getdents` enumerates immediate children + `.`/`..` as packed `aios_dirent`
   records (19-byte header, 8-aligned `d_reclen`), resuming across calls via the cursor.
   `openat`/`fstatat`/`faccessat`/`readlink`/`chdir` over the fs. **The CLOCK:**
   `pal_host_clock_gettime` reads the ARM generic timer directly at EL0 — `mrs cntpct_el0` /
   `cntfrq_el0`, the proven 0.4.x pattern (qemu-arm-virt has no RTC → REALTIME == MONOTONIC ==
   uptime). Review must-fix (all 3 finders): `getdents("/")` leaked the archive's own top prefix dir
   (`aios_loginroot`) as a phantom child — `dir_nth_child` omitted the top-dir→root case
   `tar_lookup` had. Fixed + regression-guarded.
2. **`21e7d9e` — D.2: the REAL dash + sbase RUN.** No new PAL primitive — the C + D.1 machinery
   sufficed. `guest_shrun` execs `/bin/sh -c` pipelines with `PATH=/bin`; dash forks, resolves +
   execs real sbase by PATH, wires pipes (C.5) + command substitution + redirections, runs 3-stage
   pipelines, reaps them. The one fix: `guest_copy` now **pre-checks a page is mapped**
   (`vspace_get_cap`) and stops QUIETLY, so a variable-length guest-string read (exec argv, kernel
   path reads) running its fixed cap off a short string's page is a benign short copy, not a
   `ZF_LOGE`. Pipelines are self-verifying (dash `[ "$(...)" = N ]`).
3. **`42cf989` — WIFSIGNALED in the wait status** (host-agnostic; found by the D.2 review). Before,
   `do_wait`/`on_exit` encoded `(code & 0xff) << 8` (zero low byte → always `WIFEXITED`) and signal
   deaths were force-exited `128+sig`, so dash never printed "Broken pipe"/"Segmentation fault".
   Now: `proc_t.term_sig` set at the kernel's OWN kill sites (kreturn + handle_signal_stop
   SIG_DFL-terminate → sig; boundary-escape → 31) — which is what disambiguates a real
   `exit(128+sig)` from a signal kill — plus a **negative `exit_code` = -signum** from
   `pal_guest_next` for PAL-DETECTED crashes (pal_linux.c/pal_seccomp.c `-WTERMSIG`; the seL4
   VM-fault → `-11`, replacing the magic 139), converted by the kernel's event-0 handler. One
   encoder `wait_status_of()` serves both encoding sites. `$?` is UNCHANGED (shells derive
   `128+WTERMSIG`); only the `WIFSIGNALED` bit + the diagnostic change. **Gotcha:** the old
   convention was baked into the suite — `prog_sigpipe` + `prog_stop` hard-expected
   `WIFEXITED`/128+sig and had to be updated (a "fix" without them FAILS the gate). Also routed
   `abort()` through `raise(6)` so both abort routes agree.

## >>> UNCOMMITTED: D.3, the WRITABLE fs — validated but NOT yet reviewed <<<

The working tree holds a complete, QEMU-validated **D.3** (7 modified files + the new
`uk/guest/guest_wfs.c`). **It has NOT had its adversarial find→verify review, and is therefore not
committed** — the session was redirected before that step. **First task next session: run the review,
address findings, commit.**

What D.3 does: `tarfs.c` stops re-walking the tar per lookup and instead parses it ONCE at mount into
an **fsnode tree**. A regular file's bytes are NOT copied — the node points at the tar image in
immortal `.rodata` — so the whole userland costs ~49 nodes of metadata. The first WRITE **copies that
file up** into a heap buffer (the root task's 6 MB muslc morecore) and the node becomes heap-backed;
runtime-created files are heap-backed from birth. So a read-only workload allocates nothing and
`echo hi > /tmp/x` costs one small buffer. After mount the tree is the single source of truth —
create/unlink/mkdir/rmdir/rename are ordinary tree edits, **no union/whiteout layering**. `tarfs.c`
now owns the mutating half of the contract (`mkdir/rmdir/unlink/rename/unlinkat/fchmodat/utimensat`,
`O_CREAT/O_TRUNC/O_APPEND`, and `tarfs_write` which boot.c's `pal_host_write` calls for file
handles); only ownership + hard/symbolic links stay refused (the tree has no model for them).

Validated: all six guests boot green (`guest_wfs`, `guest_tarfs`, `guest_fsbreadth`, `guest_pipe`,
`guest_fork`, `guest_shrun` — each exit 42), and dash really does
`echo written-by-dash > /tmp/sh1` → read back, `>>` append → `wc -l < file` = 2, `mkdir /tmp/shd &&
echo deep > /tmp/shd/f`. **Two older tests asserted read-only-ness and were updated** (same shape as
the WIFSIGNALED test updates): `guest_tarfs`'s refused write-open, and `guest_fsbreadth`'s
`faccessat W_OK == -EACCES`.

## State + open items

- **Unpushed:** everything from `dba2f9a` (s28 Phase B) through `42cf989` — 12 commits. Bryan pushes.
- **D.3 uncommitted + unreviewed** (above).
- **RPi5 OFFLINE** since s28 → the **Phase 0 gcc-15 gate recheck is still PENDING**.
- The Linux line is fully green: `sh run.sh` (colima) → `RESULT: linux=0 seccomp=0 sel4=0`. Re-gate
  after any shared-kernel/libaios change; colima sometimes needs `colima start` first.
- `uk/aiosroot.tar` is an **untracked build product** and is **not** regenerated by `make all` (only
  `mkaiosroot.sh`). So the tar's dash/sbase still carry the pre-`abort()`-fix libaios until someone
  regenerates it — harmless, but that is why a libaios change does not automatically reach the seL4
  image.

---

## >>> SEED PROMPT (next session) <<<

Continue building AIOS toward the verified-seL4 destination — the REAL seL4 PORT (the 2026-06-24
pivot; **AArch64 ratified**; verification is the soul; programs see only the AIOS ABI). READ FIRST:
memory [[project_pivot_linux_userspace_kernel]] (the DIRECTION line + the C.2–C.5 / D.1–D.3 entries) +
docs/PLAN_20260709_sel4_real_port.md + docs/HANDOVER_20260718_session30.md +
docs/HANDOVER_20260716_session29.md + uk/pal/sel4/{boot.c,tarfs.c,pipe.c}.

WORKING BRANCH = `main` (commit per milestone; Bryan pushes). DONE: Phases 0/A/B + **C COMPLETE**
(C.1 mmap / C.2 fork / C.3 exec / C.4 tarfs / C.5 pipes) + **D.1** (fs breadth + the CNTPCT clock) +
**D.2** (the REAL vendored dash + sbase run pipelines) + the **WIFSIGNALED** wait-status fix. A real
POSIX shell runs real coreutils on the verified microkernel.

**FIRST TASK — finish D.3 (it is sitting UNCOMMITTED in the working tree):** run the adversarial
find→verify review over the D.3 diff (`git diff` + the new `uk/guest/guest_wfs.c`), address the
findings, re-validate (rebuild + boot the six guests), and COMMIT. D.3 = the writable RAM fs: the tar
is parsed once into an fsnode tree, file data points at the immortal tar bytes and is COPIED UP on
first write (6 MB muslc morecore), and the mutating ops became real. Review angles worth covering:
the node-tree lifecycle (unlink-while-open detaches without freeing — a deliberate leak; rename into
its own subtree; parent pointers after rename), `node_ensure_rw` growth/overflow + the seek-past-EOF
hole, handle vs node invalidation, getdents cursor stability while a directory is being MUTATED
(a file created/removed mid-enumeration shifts the sibling index — likely the sharpest real bug),
and whether any read path regressed vs C.4/D.1.

THEN, Phase D breadth / Phase E:
- **spawn the REAL `/sbin/init`** (drop the hardcoded dev-guest name in `pal_guest_spawn`); init
  reads `/etc/inittab`, forks + execs `/bin/login` — which then needs console **stdin**, i.e. Phase E.
- **`sel4-gate.sh`**: boot qemu with serial on a pty and drive the `prog_*` suite (the Linux gate's
  shape) now that a shell runs; Phase C/D gate keys.
- **Phase E** = console **stdin** (seL4_DebugGetChar or the PL011) + **termios**
  (`pal_host_tcgetattr/tcsetattr` are stubs) + **signal delivery** (`pal_guest_deliver`/`sigreturn`
  are stubs — WriteRegisters a signal frame, x30 = the trampoline) → **INTERACTIVE login → dash on
  seL4**. dash+sbase RUN non-interactively today; interactive is gated on console input.
- Re-run the **RPi5 gcc-15 gate** when the Pi is back (Phase 0 recheck, pending since s28).

DISCIPLINE (keeps catching real bugs — both halves): research-validate every seL4 API against
deps/kernel + deps/seL4_libs BEFORE coding, and run an adversarial find→verify Workflow review BEFORE
every commit. This arc alone: a design-reshaping correction (sel4utils `elf_regions` is empty on the
preload path → fork parses phdrs itself), a phantom root directory entry, a zero-length-pipe-read
hang, and a cross-guest argv/env info leak — all caught by reviews, not by tests.

GOTCHAS: FULL `ninja` after a boot.c edit (the elfloader image step is separate); `pkill -f
aios-uk-sel4-image` (qemu won't self-exit); macOS has no `timeout`; the Bash cwd persists (absolute
paths); guests build via `add_custom_command`, never `add_executable`; the init guest is a HARDCODED
name in `pal_guest_spawn` (a python one-liner over `resolve_image` + `guest_image_build` swaps it to
regression-test others); a >4 KiB static guest `.bss` lands in an unaligned multi-page PT_LOAD that
`sel4utils_elf_load` REJECTS — mmap big guest buffers at runtime; non-MCS reply caps are per-guest
SaveCaller slots — never `seL4_Reply`, never `Recv` without an immediate `SaveCaller`; the Linux gate
runs in colima (`cd uk && sh run.sh`), which may need `colima start`.
