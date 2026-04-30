# AIOS HANDOVER

Self-contained briefing for a fresh development session.
Read this end-to-end, then check `docs/AI_BRIEFING.md` and the
latest `docs/NEXT_*.md` for deeper background.

---

## Quick orientation

* **Project**: AIOS (Open Aries) -- microkernel research OS on seL4
* **Repo**: `~/Desktop/github_repos/AIOS`
* **Branch**: `main`, currently at **v0.4.109** (after a big batch from
  v0.4.99)
* **Target**: AArch64 (qemu-system-aarch64 + Raspberry Pi 4)
* **Host**: macOS Apple Silicon, cross-compile to aarch64-linux-gnu
* **Developer**: Bryan -- prefers Python patch scripts over sed/heredocs;
  no apostrophes in C comments (zsh copy-paste breaks)

---

## Where we left off (v0.4.99 -> v0.4.109)

Big focus on virtual memory and observability. Eager static BSS was the
main resource bottleneck -- a process eagerly mapped a 6 MB
morecore_area regardless of actual heap use, so a 4-process pipeline
exhausted the 8000-page pool. We solved this end-to-end:

* **v0.4.99**: ZSH Phase 2 interactive ZLE working
* **v0.4.100**: `/var/log` mount; dual virtio-blk warmup fix
* **v0.4.101**: boot/ printf migration to LOG_*
* **v0.4.104**: logging round 2 + boot resilience (recovery banner) +
  VKA observability + IPC-based anonymous mmap (PIPE_MMAP_ANON)
* **v0.4.105**: design doc `DESIGN_DEMAND_BSS.md`
* **v0.4.106**: demand-paged BSS for EXEC_RUN/PIPE_EXEC -- 10-50x
  reduction in per-process page count
* **v0.4.107**: demand-paged BSS extended to EXEC_RUN_BG (getty)
* **v0.4.108**: TCC self-host PROVEN -- programs without libc
  compile and run on AIOS via on-disk `/tmp/tcc2`
* **v0.4.109**: VKA per-process page release on tear-down +
  log rotation at 1 MB

Cumulative this batch: 55+ printfs migrated to structured logging
across 13 files; recovery mode banner; accurate `/proc/vka` and
`/proc/meminfo`; eager BSS eliminated everywhere.

---

## What is pending

Two design docs sit ready for implementation:

* `docs/DESIGN_DEMAND_BSS.md` -- already implemented in v0.4.106-107
  (kept for reference)
* `docs/DESIGN_COW_FORK.md` -- the next architectural piece. Currently
  fork eagerly duplicates writable pages via `fork_dup_region`. With
  COW, fork shares R/O caps and only copies on write fault. Estimated
  2-3 focused sessions for Phase 1 + 2.

Lesser items (rough priority):
* TCC self-host with libc.a -- archive parser fails on AIOS's
  augmented libc.a (1417 members, duplicates) and on libc_min.a
  (177 members). Documented in `docs/NEXT_20260501a.md` with 5
  candidate fixes. Multi-session research.
* RPi4 hardware re-test -- recovery mode is ready (v0.4.102).
  `mkflash_rpi4.sh` single-script flasher would help.
* Log rotation backup -- currently truncates at 1 MB. Add proper
  `aios.log -> aios.log.1` rotation.
* mprotect stub -> real implementation
* file-backed mmap (currently -ENOSYS)
* Swap / paging out (long-term research)

---

## Architecture: virtual memory (the part we just rebuilt)

### Per-process layout

Each spawned process has its own seL4 VSpace via
`sel4utils_configure_process_custom`. ELF segments are loaded by
`sel4utils_elf_load`. The main writable region is a 6 MB
`morecore_area` BSS that musl uses for malloc.

### Demand-paged BSS (v0.4.106-107)

After `sel4utils_elf_load`, exec_server / pipe_server:

1. Parse program headers, find largest LOAD segment with
   `memsz > filesz`. That is the BSS.
2. `vspace_unmap_pages(VSPACE_FREE)` releases the eagerly-allocated
   BSS frames.
3. `vspace_reserve_range_at()` keeps the address range reserved for
   later fault-time mapping.
4. Store `[bss_lazy_start, bss_lazy_end]` and `bss_reservation` in
   `active_proc_t`.

The fault handler runs in two places:

* **exec_thread** -- handles foreground processes (EXEC_RUN/NICE)
  spawned with their own dedicated fault EP. Loop in
  `src/servers/exec_server.c` after the spawn, classify VMFault vs
  exit.
* **pipe_server dispatcher** -- handles background / forked-then-execed
  processes whose fault EP is minted into the shared `pipe_ep` with a
  per-process badge. `src/servers/pipe_server.c`, before
  `handle_child_fault`.

Both fault paths:

* If `seL4_Fault_VMFault` and `seL4_GetMR(seL4_VMFault_Addr)` is in
  `[bss_lazy_start, bss_lazy_end)`:
  * `vka_audit_check_headroom(1)` (refuse on pressure)
  * `vspace_new_pages_at_vaddr(bss_reservation)` for one page
  * `vka_audit_frame(VKA_SUB_OTHER, 1)`
  * `ap->audit_pages_allocated++`
  * `seL4_Reply` to resume the thread; loop for more
* Else (real fault or exit): break out, take the existing exit path

EXEC_RUN_BG (getty) uses minted pipe_ep so pipe_server handles its
faults too. Without this, getty would deadlock on first BSS access.

### IPC anonymous mmap (v0.4.104)

`PIPE_MMAP_ANON` (label 83) in `pipe_server.c`. Caller sends number
of pages in MR0; server calls `vspace_new_pages` on caller's
proc.vspace and replies with the vaddr. Cap at 1024 pages (4 MB)
per call. Used by `aios_sys_mmap` in `src/lib/posix_misc.c` when
`MAP_ANONYMOUS` is requested.

### VKA accounting (v0.4.103, 0.4.109)

* `vka_audit_frame(sub, n)` increments per-subsystem and
  `vka_live_frames`
* `vka_audit_frame_release(n)` decrements `vka_live_frames`
* `vka_audit_check_headroom(n)` returns -1 if pool pressured;
  exec_server uses this to refuse spawn when free pages < 2000
* `vka_audit_release_proc_pages(&ap->audit_pages_allocated)` is
  called before each `sel4utils_destroy_process` to release the
  process's tracked pages
* `/proc/vka` and `/proc/meminfo` show real numbers

---

## Build and boot

### Full rebuild (when CPIO contents change)

```
cd ~/Desktop/github_repos/AIOS
rm -rf build-04 && mkdir build-04 && cd build-04
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
ninja
```

### Incremental (most edits)

```
cd build-04 && ninja
```

`tty_server.c`, `auth_server.c` are in CPIO -- changing them needs
full rebuild. Everything in `src/aios_root.c`, `src/boot/*`,
`src/servers/*`, `src/process/*`, `src/lib/*` is in the root task
binary -- ninja handles incrementally.

### Disk image (after editing programs in `src/apps/` or `disk/rootfs/`)

```
python3 scripts/mkdisk.py disk/disk_ext2.img \
    --rootfs disk/rootfs \
    --install-elfs build-04/sbase \
    --aios-elfs build-04/projects/aios/
```

### Sbase

```
python3 scripts/build_sbase.py
```

Runs after `rm -rf build-04` (which deletes sbase binaries).

### Dash (rebuild after libaios_posix.a changes)

```
DASH=~/Desktop/github_repos/dash/src
./scripts/aios-cc \
    $DASH/main.c $DASH/eval.c $DASH/parser.c $DASH/expand.c \
    $DASH/exec.c $DASH/jobs.c $DASH/trap.c $DASH/redir.c \
    $DASH/input.c $DASH/output.c $DASH/var.c $DASH/cd.c \
    $DASH/error.c $DASH/options.c $DASH/memalloc.c \
    $DASH/mystring.c $DASH/syntax.c $DASH/nodes.c \
    $DASH/builtins.c $DASH/init.c $DASH/show.c \
    $DASH/arith_yacc.c $DASH/arith_yylex.c \
    $DASH/miscbltin.c $DASH/system.c \
    $DASH/alias.c $DASH/histedit.c $DASH/mail.c $DASH/signames.c \
    $DASH/bltin/test.c $DASH/bltin/printf.c $DASH/bltin/times.c \
    -I $DASH -include $DASH/config.h -DSHELL -DSMALL -DGLOB_BROKEN \
    -o build-04/sbase/dash
```

### ZSH (rebuild after libaios_posix.a changes)

```
python3 scripts/build_zsh.py
```

### Boot QEMU (with both drives -- log file persists)

```
cd ~/Desktop/github_repos/AIOS
qemu-system-aarch64 \
    -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G \
    -nographic -serial mon:stdio \
    -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -drive file=disk/log_ext2.img,format=raw,if=none,id=hd1 \
    -device virtio-blk-device,drive=hd1 \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

Login: `root` / `root`.

### Boot without log drive (test recovery mode)

Same command, drop the `hd1` drive. See "AIOS RECOVERY MODE" banner.

---

## Session protocols

### bump-patch at start

```
./scripts/bump-patch.sh
./scripts/version.sh
```

Always at the start of new work. `make bump-minor` is for major
milestones only.

### Commit

User prefers GitHub Desktop for commits, BUT we can do `git commit`
directly when explicitly asked. Format:

```
v0.4.XXX: short title

3-5 bullets / paragraphs of why and what changed.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

NEVER skip hooks. NEVER amend. NEVER force-push.

### Code edits

* Use Python heredoc to /tmp script then `python3` invocation when
  the user is running scripts in their terminal
* When you're operating directly via tools, use `Edit` / `Write`
* Verify changes with `grep`/`Read` after editing
* Single-quote apostrophes in C comments break zsh copy-paste --
  spell out instead (use ascii dashes etc)

---

## Useful files for context

* `docs/AI_BRIEFING.md` -- full architecture reference
* `docs/DESIGN_DEMAND_BSS.md` -- VM design (implemented)
* `docs/DESIGN_COW_FORK.md` -- VM design (next major piece)
* `docs/NEXT_20260430a.md` ... `NEXT_20260501a.md` -- session logs
* `include/aios/root_shared.h` -- IPC labels, active_proc_t
* `include/aios/aios_log.h` -- LOG_MODULE / AIOS_LOG_* macros
* `include/aios/vka_audit.h` -- pool accounting helpers
* `src/servers/pipe_server.c` -- central IPC hub, fault dispatcher
* `src/servers/exec_server.c` -- ELF loader + foreground fault loop
* `src/process/fork.c` -- current eager-copy fork (COW target)

---

## Known gotchas

* **tty_server is in CPIO** -- changing it requires full rebuild
* **dash + zsh + sbase rebuild** needed after `libaios_posix.a`
  changes; ninja does NOT rebuild them
* **dual virtio-blk warmup** required: a dummy
  `plat_blk_read(2, ...)` after `plat_blk_init_log()` or system
  disk reads silently fail (see `feedback_virtio_blk_warmup.md`)
* **EXEC_RUN_BG fault EP** must be minted into pipe_ep, otherwise
  no one polls it and the process hangs on first fault. Set
  `ap->fault_on_pipe_ep = 1` after minting.
* **morecore_area = 6 MB** static BSS per process. Now lazy via
  v0.4.106 unmap+fault. Adjust `LibSel4MuslcSysMorecoreBytes` in
  `settings.cmake` if you need more.
* **VKA pool = 8000 pages** total. With demand-paged BSS, that
  comfortably handles 5+ concurrent processes. Without it: 3
  max.
* **fork is eager** -- big writable regions duplicated on fork.
  COW design ready in `DESIGN_COW_FORK.md`.
* **TCC self-host works for libc-free programs only** -- archive
  parser issues on libc.a / libc_min.a. See `NEXT_20260501a.md`
  for the 5 fix options.

---

## What works "out of the box" right now

After boot + login:

```
ls                              # 99 sbase tools in /bin
echo "hello"                    # builtin
cat /proc/vka                   # accurate live page count
cat /proc/meminfo               # real MemTotal + Pool*
cat /proc/log | tail -50        # ring buffer log
cat /var/log/aios.log | tail    # persistent log

zsh                             # interactive, ZLE working
                                # (compctl warning is cosmetic)

/tmp/tcc2 -o /tmp/t /tmp/t.c    # for libc-free programs
/tmp/t; echo $?                 # works for return-code programs

# kill foreground with Ctrl-C twice (two-stage SIGINT)
# logout via Ctrl-D from getty
```

---

## Suggested next sessions

In rough order of impact:

1. **COW fork (Phase 1)** -- `DESIGN_COW_FORK.md` Phase 1 only.
   Modify `fork_dup_region` to use CNode_Copy + R/O remap, extend
   pipe_server fault handler with cow_ranges check + write-fault
   path. ~2 sessions for working version.

2. **TCC libc archive fix** -- try Approach 3 (pre-linked .o) from
   `NEXT_20260501a.md`. Pre-link relevant musl/AIOS objects via
   `ld -r` into a single .o; link that into programs. Avoids TCC's
   archive parser entirely.

3. **mprotect** -- currently stub. Wire up real seL4 page-table
   protection updates. Touched only by JITs / dlopen-style code,
   not critical.

4. **RPi4 hardware test** -- boot the existing kernel on real
   hardware, verify recovery mode banner appears when SD card has
   no system partition. `mkflash_rpi4.sh` would streamline.

5. **Log rotation backup** -- currently truncates at 1 MB. Move
   aios.log to aios.log.1 first, keep 1 backup.

6. **Server health probes** -- periodic ping to fs/exec/pipe/net.
   Auto-restart on death. Distinguishes "frozen" from "broken"
   (would have helped diagnose getty hang earlier in this batch).

---

## Final notes

The system is in a good place: stable boot, recovery mode, demand
paging, accurate accounting, persistent logs, working shell + ZLE,
TCC for simple programs. The next leap (COW fork) has a written
plan ready to execute against.

When in doubt:
* Check `cat /proc/log` and `cat /var/log/aios.log` for traces
* `cat /proc/vka` to see if memory pressure is the culprit
* Look at `[INF]` / `[WRN]` / `[ERR]` tagged lines on serial -- the
  module name (boot, fs, blk, exec, pipe, vka, etc.) tells you
  which subsystem to read

Good luck.
