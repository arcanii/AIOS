# AIOS HANDOVER

Self-contained briefing for a fresh development session.
Read this end-to-end, then check `docs/AI_BRIEFING.md` and the
latest `docs/NEXT_*.md` for deeper background.

---

## Quick orientation

* **Project**: AIOS (Open Aries) -- microkernel research OS on seL4
* **Repo**: `~/Desktop/github_repos/AIOS`
* **Branch**: `main`, currently at **v0.4.120**
* **Target**: AArch64 (qemu-system-aarch64 + Raspberry Pi 4)
* **Host**: macOS Apple Silicon, cross-compile to aarch64-linux-gnu
* **Developer**: Bryan -- prefers Python patch scripts over sed/heredocs;
  no apostrophes in C comments (zsh copy-paste breaks)

---

## Where we left off (v0.4.110 -> v0.4.119)

Three intertwined arcs landed in this batch: copy-on-write fork,
block-layer caching with reclaim, and a stack-overflow bug that had
been masquerading as a BSS-init mystery for several sessions.

* **v0.4.110**: COW fork infrastructure (Phase 1 WIP) -- cow.c +
  fault handler hooks, gated off pending the reservation-tracking
  interaction.
* **v0.4.111**: COW enabled for the file-backed (data) portion of
  writable LOAD segments. Reservations created fresh after
  sel4utils_elf_load frees its own. cow_release_proc explicitly
  unmaps before destroy.
* **v0.4.112**: Block cache + `/proc/cachestats`. ext2 sector I/O
  routes through 4 KB cache lines (LRU, hash table, vka-backed).
  Two backends (drive 0 = system disk, drive 1 = log).
* **v0.4.113**: Cache reclaim under memory pressure --
  vka_audit_check_headroom calls blk_cache_evict before failing.
  Process pages always beat cache pages.
* **v0.4.114**: Boot warmup prefetch + `/proc/filehits` profiler.
  17 files (~123 KB) preloaded before login; per-path access counter
  hooked into vfs_read.
* **v0.4.115**: File-redirect inheritance across exec.
  `aios_sys_execve` packs stdout/stderr file paths into the
  PIPE_EXEC IPC; `__wrap_main` re-opens and seeds stdout_redir_*.
  Fixes `ls > /tmp/o` producing an empty file.
* **v0.4.116**: Warmup file list tuned from real `/proc/filehits`
  data after a representative interactive session.
* **v0.4.117**: TCC self-host with libc -- pre-linked libaios_tcc.o
  (~613 KB) wraps the AIOS runtime + libc subset, packaged as a
  one-member `/usr/lib/libc.a`. tcc on AIOS now compiles + links
  programs that #include <stdio.h>, call printf/malloc/exit.
* **v0.4.118**: Stack-overflow fix that we had been working around
  with FILEHITS_MAX/PATH_MAX tuning since v0.4.114. Symptom looked
  like a BSS-init bug; root cause was a stack overflow corrupting
  probe_info. Real fix landed; the FILEHITS comment is now just
  the simple "80 chars is plenty" line.
* **v0.4.119**: cow_release_proc page-by-page unmap (was bulk).
  Reduces but does NOT eliminate the residual "BSS fault: map
  failed / Range for vaddr X not reserved" log noise after
  fork+exec -- the v0.4.119 commit message overstated this.
  Pure baseline still emits ~10 such errors per smoke session.
* **v0.4.120**: Log rotation now keeps a backup generation. Was
  unlink+recreate (data loss); now reads current `aios.log` into a
  lazily-malloc'd 1 MB heap buffer, recreates `aios.log.1` from it,
  then drops `aios.log` and recreates fresh. Buffer is heap (not
  static BSS) to avoid shifting the root task's BSS layout.

### COW Phase 2 attempt (between v0.4.119 and v0.4.120, abandoned)

Spent a session attempting Phase 2 (parent-side stripping +
refcount table + stack COW per `docs/DESIGN_COW_FORK.md`). All
of the work was reverted. Three concrete blockers found and
documented in `docs/NEXT_20260502b.md`:

1. **Parent stripping** via `seL4_ARM_Page_Map(parent_cap,
   parent_pd, va, R/O)` triggered "Invocation of invalid cap" /
   cap fault in the forked child. Mechanism unclear -- possibly
   needs unmap-then-remap, or interacts with the shared frame's
   CDT.
2. **Stack COW** reservation collided with child's IPC buffer
   when probe extended into low memory.
3. **Refcount table size** (1024 entries) shifted BSS layout
   enough to amplify the v0.4.119 baseline noise.

The wholesale rewrite was too coupled to debug. The NEXT doc
proposes a 5-step incremental restart. Step 1 (WnR fault
detection) is ~10 LOC and risk-free; do that first if resuming.

Cumulative this batch: COW Phase 1 fully functional, disk-cache
hit rates ~84% in normal sessions, file redirection works
end-to-end, on-AIOS tcc can build hello-world style programs
against libc, log rotation keeps one historical generation.

---

## What is pending

Two design docs sit ready for implementation:

* `docs/DESIGN_DEMAND_BSS.md` -- implemented in v0.4.106-107.
* `docs/DESIGN_COW_FORK.md` -- **Phase 2 still pending**, attempt
  reverted v0.4.119 -> v0.4.120; see `docs/NEXT_20260502b.md` for
  the incremental restart plan.

### Tactical items, sized

| Item | LOC | Risk | What ships |
|---|---|---|---|
| **COW Phase 2 Step 1** -- WnR fault detection | ~10 | very low | Read faults on R/O COW pages get killed instead of handled as writes. Pure correctness. No new BSS. |
| **Server health probes -- ping only** | ~50 | low | Periodic poll thread that pings fs/exec/pipe/net/disp/crypto via a no-op IPC label. `/proc/serverstats` shows last-ping age. Auto-restart is a separate, much bigger step. |
| **COW Phase 2 Step 2** -- refcount table observation-only | ~150 | low-med | Track shared-frame refcounts; expose via `/proc/cow`. Earlier 1024-entry table broke; 64 entries should be safe. Foundation for parent stripping. |
| **Block cache write-back** | ~150 | medium | Switch from write-through. AIOS fs traffic too low for measurable speedup right now. |
| **mprotect real impl** | ~200 | medium | New PIPE_MPROTECT label; server walks caller's vspace, calls `seL4_ARM_Page_Map` per page. We confirmed remap-in-place works during the COW work. |
| **file-backed mmap** | ~300 | medium | MAP_SHARED on a regular file, extends PIPE_MMAP_ANON. Coherency on writes (msync?) is the hard bit. |
| **Server health probes -- full** (with auto-restart) | ~400 | high | Detecting death is easy; restoring server state across restart is hard (BSS-resident state, in-flight reply caps, registered clients). |
| **RPi4 hardware re-test** | n/a | n/a | Needs physical hardware. Recovery mode banner is ready (v0.4.102); `mkflash_rpi4.sh` flasher would streamline. |
| **Swap / paging out** | many | long-term research | |

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
cat /proc/cachestats            # block-cache hit rate / size
cat /proc/filehits              # top accessed files (profiler)

zsh                             # interactive, ZLE working
                                # (compctl warning is cosmetic)

ls /bin > /tmp/o; wc -c /tmp/o  # file redirect across exec works
echo abc | wc -c                # pipe across fork+exec works
cat /etc/passwd | head -1       # head limit works correctly

/tmp/tcc2 -o /tmp/t /tmp/t.c    # native tcc (libc-free programs)
tcc /usr/include/hello.c -o /tmp/h  # native tcc with libc (v0.4.117)
/tmp/h; echo $?                 # libc programs run

# kill foreground with Ctrl-C twice (two-stage SIGINT)
# logout via Ctrl-D from getty
```

---

## Suggested next sessions

See the sized table in "What is pending" above. The shortest
high-confidence next step is **COW Phase 2 Step 1 (WnR fault
detection)** -- ~10 LOC, validates the incremental-restart
approach from `docs/NEXT_20260502b.md` after the abandoned
wholesale Phase 2 attempt.

After that, **server health probes (ping-only)** is the highest
user-visible-value low-risk medium item. Then **mprotect real
impl** for usefulness to JITs / dlopen-style code.

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
