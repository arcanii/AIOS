# HANDOVER — session 29 (2026-07-16..17): Phase C.2 fork + C.3 exec + C.4 tarfs — fork/exec/files on seL4; a VENDORED SBASE BINARY runs

**HEADLINE:** the two hardest mechanisms of family B (the process model) landed in one session. A
guest **forks** (eager VSpace copy, the parent waits PARKED across the child's faults, three memory
regions proven isolated) and **execs** (atomic image swap; a failed exec leaves the caller running;
argv/envp arrive byte-exact per the AIOS SysV contract) — both exit 42 on qemu-arm-virt, both
committed with the full research + adversarial-review discipline.

Continues docs/HANDOVER_20260711_session28.md (Phases 0/A/B/C.1). Read: memory
[[project_pivot_linux_userspace_kernel]] (the C.2 + C.3 entries carry ALL mechanism detail) +
docs/PLAN_20260709_sel4_real_port.md + uk/pal/sel4/boot.c.

## What shipped (2 commits on `main`, both UNPUSHED — Bryan pushes; dba2f9a/f21faff/94ba5af may also be)

1. **`a25f382` — Phase C.2: fork.** The PROCESS TABLE (guest_t slots; per-guest BADGED fault-EP
   copies, badge=slot+1; MONOTONIC pal pids) + **SaveCaller REPLY TOKENS** (needed at C.2, not
   Phase D: non-MCS Recv DELETES unconsumed caller caps → a parent parked in wait() would strand the
   moment another guest faulted; every fault Recv is immediately SaveCaller'd per-guest; replies =
   seL4_Send on the saved slot ≡ doReplyTransfer) + **pal_guest_fork** (eager copy: reconfigure from
   the same CPIO ELF, VERIFY the child's layout matches, copy writable-phdr ranges + the whole stack
   + mmap regions at identical vaddrs; child = parent's full 36-word context, x0=0, **pc=FaultIP+4**)
   + **object teardown** (exit → destroy_process + the badged EP + the reply slot; the Phase B leak
   closed). Test: guest_fork (data/stack/mmap isolation with a sentinel ≠ the ELF initializer — the
   review's catch — and NEGATIVE-tested by disabling the copy → loud fail).
2. **`ffc72d5` — Phase C.3: exec.** The **HAND-STAGED SysV STACK** ([sp]=argc, argv, NULL, envp,
   NULL, strings — the libaios `_start` contract) replaces sel4utils_spawn_process_v for BOTH spawn
   and exec; start = WriteRegisters(pc=entry_point, sp). The **DOUBLE-BUFFERED image swap**:
   sel4utils_process_t embeds its vspace bookkeeping INLINE (struct copy = dangling pointers) → each
   slot holds proc[2]/cur; exec builds into proc[cur^1] while the old image stays fault-stopped +
   intact, retires it only after the new image is fully staged. Failed exec (-ENOENT/-E2BIG/-EFAULT/
   -ENAMETOOLONG) returns to a still-running caller. Test: guest_exec (spawn-stack check +
   failed-exec-continues) → guest_execd (argv/envp byte-exact). guest_fork regression re-booted
   green under the refactor.

## Discipline notes (both reviews caught real things — KEEP DOING IT)

- **Pre-code research Workflow** (C.2): 4 researchers + 5 adversarial verifiers over deps/kernel +
  deps/seL4_libs, every claim file:line-cited. 4 CONFIRMED, **1 CORRECTED — and the correction
  reshaped the design**: sel4utils `elf_regions` is EMPTY on the preload path (and useless on the
  other), so fork parses the ELF phdrs itself. Also surfaced: SaveCaller's silent no-caller case,
  the WriteRegisters pc→FaultIP subtlety, ReadRegisters-on-fault-blocked legality.
- **Pre-commit find→verify reviews** (read-only Explore agents): C.2 = 3 mechanism angles zero
  defects + 1 confirmed should-fix (the vacuous .data check — the parent's value equaled the ELF
  initializer, so the child's fresh preload satisfied it without any copy; fixed with a sentinel).
  C.3 = exec-atomicity angle zero defects + 1 should-fix (stage_stack's 16-byte alignment padding
  copied stale bytes from the SHARED static buffer into the next guest's stack — a cross-guest
  argv/env info leak; zeroed) + 2 nits (ENAMETOOLONG up-front guard; an unused helper).
- C.3 added NO new seL4 API beyond the C.2-validated set (entry_point on the preload path was
  source-verified directly, libsel4utils/src/process.c:551).

## Build + boot (unchanged from s28; the Mac IS the build host)

Same recipe as the s28 handover (cmake -DAIOS_UK_BUILD=1 …, FULL `ninja`, qemu-arm-virt, scrape the
serial log, `pkill -f aios-uk-sel4-image`). The init guest is HARDCODED in pal_guest_spawn
(currently "guest_exec"); regression-test another guest by swapping the name in
`guest_image_build(g, 0, "<name>", &g->img)` + rebuild (the C.3 session did this for guest_fork).
CPIO guests: guest_hello, guest_mmap, guest_fork, guest_exec, guest_execd (uk/guest/*.c, built by
projects/aios-uk/CMakeLists.txt only — deliberately NOT in the uk Makefile's Linux guest list).

## State + open items

- **Unpushed:** a25f382 + ffc72d5 (and possibly dba2f9a/f21faff/94ba5af from s28) — confirm with Bryan.
- **RPi5 OFFLINE** (checked 2026-07-16: mDNS unresolvable). The **Phase 0 gcc-15 recheck stays
  pending** (low-risk; gcc-13 validated). Re-run `sh gate.sh` both backends when it returns.
- **Linux line untouched all session** (kernel/pal.h/uk Makefile byte-identical; boot.c compiles
  only in the seL4 build) → no colima re-gate needed. `make PAL=sel4` link canary verified green.
- Phase C remaining: **C.4 the read-only tarfs** (embed aiosroot.tar in the image + pal_host_open/
  read/lseek/close/fstat over it — the kernel's fd table is already host-agnostic; exec paths then
  become REAL instead of basename→CPIO), **C.5 pipes** (the kernel's pipes are kernel-internal
  already; what C.5 must prove is the PARK/WAKE of blocked readers/writers over the SaveCaller
  tokens + the pal.h PAL_EWOULDBLOCK seams). Phase C gate-key target = `pipebig`.
- Tripwire status: Phase C is at session 2 of ~4 (C.1 was s28) — on pace; eager fork is already the
  shipped choice (COW dropped per plan §6).

---

## >>> SEED PROMPT (next session) <<<

Continue building AIOS toward the verified-seL4 destination — the REAL seL4 PORT (the 2026-06-24
pivot; AArch64 ratified; verification is the soul; programs see only the AIOS ABI). READ FIRST:
memory [[project_pivot_linux_userspace_kernel]] (the DIRECTION line + the C.2/C.3 entries) +
docs/PLAN_20260709_sel4_real_port.md + docs/HANDOVER_20260716_session29.md + uk/pal/sel4/boot.c.

WORKING BRANCH = `main` (commit per milestone; Bryan pushes; confirm unpushed with Bryan). DONE:
Phases 0/A/B/C.1 (s28) + **C.2 FORK (a25f382) + C.3 EXEC (ffc72d5)** (s29) — the process table,
SaveCaller reply tokens, eager-copy fork, the hand-staged SysV stack, the double-buffered atomic
exec. All mechanism detail + gotchas live in the pivot-file entries; build+boot recipe in the s28
handover. Every seL4 API is kernel-source-validated by a research Workflow BEFORE coding, and every
milestone gets an adversarial find→verify review BEFORE commit (a design-reshaping correction and
three real defects caught this session alone — KEEP DOING BOTH).

**C.4 LANDED TOO (52863d7, same session):** the read-only tarfs (uk/pal/sel4/tarfs.c: in-place
ustar parse, PAL-side path normalization — the kernel does NOT path_norm — pal_host open/read/
lseek/close/fstat/stat), the bytes-identity loader (configure-without-elf + manual
sel4utils_elf_load; tarfs real paths first, CPIO fallback), and the HEADLINE: /bin/echo — the real
vendored sbase binary out of aiosroot.tar — exec'd by path and RAN on seL4 (the first unmodified
vendored userland binary on the seL4 backend). Review: 13 raw findings, 8 adversarially refuted,
zero must/should-fix. aiosroot.tar is an UNTRACKED Linux-line build product (uk: make all + sh
mkaiosroot.sh); CMake fails loudly if missing.

PRIMARY TASK → **continue Phase C: C.5 PIPES.** The kernel's pipe machinery is host-agnostic and
kernel-internal (do_pipe/do_read/do_write + PS_BLOCKED_*) — what C.5 must prove on seL4 is the
blocked-reader/writer PARK/WAKE over the SaveCaller reply tokens (the C.2 wait() park already
proved the pattern) + whatever pal.h PAL_EWOULDBLOCK seams the kernel expects; gate-key target
`pipebig` (uk/guest/prog_pipebig.c is the Linux-line reference). Then **Phase D breadth**: ~24 fs
ops (getdents/openat/fstatat/...) + the CNTVCT clock → dash + sbase RUN — the tar already holds
/sbin/init and the whole world, so spawn becomes /sbin/init-for-real. Tripwire: Phase C >~4
sessions → ship what works, defer polish (s29 was session 2 and landed C.2+C.3+C.4).

OTHER: re-run the RPi5 gcc-15 gate when the Pi is back (Phase 0 recheck, pending since s28; Pi
offline 2026-07-11..16). GOTCHAS: FULL `ninja` after a boot.c edit; `pkill -f aios-uk-sel4-image`;
absolute paths (the Bash cwd persists); guests via add_custom_command, never add_executable; the
init guest is a HARDCODED name in pal_guest_spawn (swap + rebuild to regression-test others);
non-MCS reply caps are per-guest SaveCaller slots now — never seL4_Reply, never Recv without an
immediate SaveCaller.
