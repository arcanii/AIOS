# AIOS HANDOVER

Self-contained briefing for a fresh development session.
Read this end-to-end, then check `docs/AI_BRIEFING.md` and the
latest `docs/NEXT_*.md` for deeper background.

---

## Quick orientation

* **Project**: AIOS (Open Aries) -- microkernel research OS on seL4
* **Repo**: `~/Desktop/github_repos/AIOS`
* **Branch**: `main`, currently at **v0.4.134**
* **Target**: AArch64 (qemu-system-aarch64 + Raspberry Pi 4)
* **Host**: macOS Apple Silicon, cross-compile to aarch64-linux-gnu
* **Developer**: Bryan -- prefers Python patch scripts over sed/heredocs;
  no apostrophes in C comments (zsh copy-paste breaks)

---

## Where we left off (v0.4.126 -> v0.4.134)

Two arcs landed in this batch: POSIX VM/FS syscall fill-in (replacing
silent stubs with real IPC) and RPi4 hardware-test prep (FAT32, flash
script, SMP build enable, test plan). User has an RPi4 in hand; first
hardware boot is the next milestone.

* **v0.4.126**: real `mprotect` via new `PIPE_MPROTECT` (label 85).
  Server walks caller's vspace, calls `seL4_ARM_Page_Map` per page with
  new rights. Same kernel mechanism the v0.4.123 COW investigation
  proved safe. `src/apps/test_mprotect.c` covers the round trip.
* **v0.4.127**: mprotect extended -- `PROT_NONE` via per-page Unmap,
  `PROT_EXEC` clears the XN bit. test_mprotect now exercises both.
* **v0.4.128**: real `munmap` via `PIPE_MUNMAP_ANON`. Previously a
  no-op; now actually frees frames. Verified by re-mmap after munmap.
* **v0.4.129**: `/proc/cmdline` summarises the boot environment.
* **v0.4.130**: real `ftruncate` via new `FS_TRUNCATE` IPC.
* **v0.4.131**: `/proc/cmdline` platform-aware (rpi4 vs qemu-virt) +
  `BACKLOG.md` for deferred items so HANDOVER's pending table stays
  tight.
* **v0.4.132**: `scripts/mksdcard.py` swapped mtools `mformat` for
  macOS `newfs_msdos` (run against the image as a hdiutil `-nomount`
  vnode), then patches BPB `hidden_sectors=2048` by hand. RPi4 firmware
  now reads our FAT32 partition cleanly -- the documented Method 2
  manual workaround is no longer the only path. Linux dev hosts fall
  back to mformat.
* **v0.4.133**: `scripts/flash-rpi4.sh` -- one-shot SD card flasher.
  Wraps `mksdcard.py` + `dd` with safety checks (refuses
  /dev/disk0..2, refuses Device Location: Internal, refuses partition
  slices, requires literal `YES`). Tested all five refusal paths;
  hardware happy path waits on a real card.
* **v0.4.134**: `KernelMaxNumNodes` 1 -> 4 in `settings-rpi4.cmake`.
  Build clean, image grew ~50 KiB. Elfloader spin-table driver was
  already wired upstream. qemu raspi4b is silent for our SD image
  (firmware/UART quirks unrelated to SMP); validation happens on real
  hardware.

Also landed two docs (no version bump):
* **`hw/rpi4/HARDWARE_TEST.md`**: phase-by-phase first-boot checklist.
  Physical setup, expected serial output at each stage, functional
  checklist, diagnosis playbook, version fallback ladder.
* **`docs/SEL4_DEVInvestigation.md`**: seed for a session that wants
  to look at the seL4 side -- current snapshot, the `seL4_Debug*`
  ABI we underuse, five concrete diagnostic gaps from this session's
  work, ground rules for kernel patches behind a single
  `CONFIG_AIOS_KDEBUG` gate, ordered investigation list (A-E).

### State of the RPi4 hardware test (waiting on first boot)

* `disk/sdcard-rpi4.img` (193 MB) is current at v0.4.134. dash + zsh
  were rebuilt against fresh `libaios_posix.a` so the on-disk
  programs include the v0.4.121-134 syscall surface.
* `scripts/flash-rpi4.sh /dev/diskN` is the one-line flash.
* `hw/rpi4/HARDWARE_TEST.md` is the checklist. Open question on
  first boot is whether all four cores come up (v0.4.134 is the
  first RPi4 build with SMP enabled).

## Where we left off (v0.4.121 -> v0.4.125)

This batch finished COW Phase 2 Steps 1-3 from `docs/NEXT_20260502b.md`
plus a new server health probe. Steps 1 and 2 are live. Step 3
(parent-side stripping) has all the plumbing landed but is gated off
because of an unresolved post-promotion regression -- see "COW Phase 2
Step 3 status" below and `docs/NEXT_20260502c.md`.

* **v0.4.121**: COW Phase 2 Step 1 (WnR fault detection) + server
  health probes. cow_handle_write_fault now reads FSR bit 6 and
  returns 0 on read faults so they fall through to the real
  exit path (a read fault inside a COW range is an instruction-fetch
  bug, not a COW promotion). New in-process probe thread pings
  pipe/fs/thread/net/disp/crypto every 5s with `SVC_PING` (label 5);
  exposed via `/proc/serverstats`. Probe runs at priority 200 (same
  as the servers it pings) -- 180 was permanently starved.
* **v0.4.122**: COW Phase 2 Step 2 (per-frame refcount). 64-entry
  table in cow.c keyed by paddr; cow_frame_acquire wired into
  cow_setup_segment, cow_frame_release into cow_release_proc and the
  promotion path. Pure observation -- no behaviour change.
  `/proc/cow` exposes the table. Smoke confirms BSS-fault count
  unchanged from v0.4.121 baseline (kept the table to 64 entries
  specifically to avoid the 1024-entry BSS-shift trap from the
  abandoned attempt).
* **v0.4.123**: kernel investigation + Step 3 probe. Read seL4 ARM
  `performPageInvocationMap` to confirm Page_Map remap-in-place is
  safe and only touches the cap slot, the PTE, and the TLB. Probe
  (gated behind COW_PROBE_PARENT_STRIP) verified the kernel
  mechanism in production: parent strip succeeds, parent dies on
  next write to a stripped page, getty respawns. Probe code stays
  in cow.c gated off; full investigation in
  `docs/NEXT_20260502c.md`.
* **v0.4.124**: Step 3 plumbing landed gated OFF. cow_setup_segment
  takes parent_idx; cow_handle_write_fault has the parent-promotion
  branch (kernel Page_Map of fresh frame, stash in
  cow_promoted_global, set cow_disabled to flip future forks of
  this proc to eager copy). cow_release_proc skips parent ranges
  in the vspace_unmap walk. Per-proc Step 3 state lives in cow.c
  globals (NOT active_proc_t) -- first attempt put it inline and the
  resulting BSS shift broke dash startup, repeating the v0.4.122
  lesson.
* **v0.4.125**: cow_current_cap helper + fork.c plumbing. Threads
  parent_idx through fork_copy_into_existing / region / stack so
  the eager-copy fallback after a parent's promotion sources from
  cow_promoted_global instead of the orphaned parent_cap. Step 3
  mechanism now proven end-to-end: smoke shows
  `parent_promotions: 2, strip_errs: 0`, no kernel errors. But
  with COW_STRIP_PARENT=1 dash post-promotion sees EPERM on
  subsequent fork+exec (`wc`, `shutdown` fail). With the gate at 0
  the system is v0.4.122-equivalent.

### COW Phase 2 Step 3 status

Mechanism: working. Production-on: not yet. The wc/shutdown bug
manifests only when COW_STRIP_PARENT=1, after a parent has
promoted at least one page. It's behind a default-off flag and
loses no existing capability (eager-copy fork path is unchanged
when strip is 0). Next session can flip the flag, repro the EPERM
on second fork+exec, and trace through `do_fork`'s 12 -1 paths
to find which one fires post-promotion. Likely candidates: cap
allocation interacting with the orphaned parent_cap, or a child
cspace cap that ends up wrong. See `docs/NEXT_20260503a.md`.

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

Design docs:

* `docs/DESIGN_DEMAND_BSS.md` -- implemented in v0.4.106-107.
* `docs/DESIGN_COW_FORK.md` -- Phase 1 + Phase 2 Steps 1-2 live;
  Step 3 plumbed but gated off (see `docs/NEXT_20260503a.md`).
  Steps 4-5 (stack COW, parent-dies safety) not started.

### Active tactical items

| Item | LOC | Risk | What ships |
|---|---|---|---|
| **RPi4 hardware re-test** | n/a | n/a | Needs physical hardware. v0.4.98 was the last verified RPi4 boot (build 1541). Codebase has churned heavily since (v0.4.99-130). First step is rebuilding the RPi4 target against current main and addressing any new breakage before flashing. See `hw/rpi4/BOOT_NOTES.md`. |

Everything else (COW Step 3 fix, block cache write-back, file-backed
mmap, COW Steps 4+5, server-probe auto-restart, swap, smoke-driver
polish, fault-observation harness) is in [BACKLOG.md](BACKLOG.md).

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
cat /proc/serverstats           # ping-based server health (v0.4.121)
cat /proc/cow                   # COW per-frame refcount (v0.4.122)
cat /proc/cmdline               # platform-aware boot env summary (v0.4.131)

zsh                             # interactive, ZLE working
                                # (compctl warning is cosmetic)
                                # (rebuild after libaios_posix.a edits!)

ls /bin > /tmp/o; wc -c /tmp/o  # file redirect across exec works
echo abc | wc -c                # pipe across fork+exec works
cat /etc/passwd | head -1       # head limit works correctly

test_mprotect                   # mprotect R/O, PROT_NONE, PROT_EXEC,
                                # munmap, re-mmap round trip (v0.4.126-128)
ftruncate $file $size           # real fs-side truncate (v0.4.130)

/tmp/tcc2 -o /tmp/t /tmp/t.c    # native tcc (libc-free programs)
tcc /usr/include/hello.c -o /tmp/h  # native tcc with libc (v0.4.117)
/tmp/h; echo $?                 # libc programs run

# kill foreground with Ctrl-C twice (two-stage SIGINT)
# logout via Ctrl-D from getty
```

---

## Suggested next sessions

**Top pick: RPi4 first hardware boot** (waiting on the user).
Artefacts are ready -- `disk/sdcard-rpi4.img` is current at v0.4.134
with dash/zsh rebuilt against fresh libaios_posix.a, the
`scripts/flash-rpi4.sh` one-liner handles the flash with safety
checks, and `hw/rpi4/HARDWARE_TEST.md` is the phase-by-phase
checklist. The interesting signal on first boot is whether
all four cores come up (v0.4.134 is the first RPi4 build with
SMP enabled). If only the boot CPU appears, fall back is one line
in `settings-rpi4.cmake`. Either outcome unblocks the next batch.

**If hardware testing is blocked, three good paths:**

1. **seL4 dev investigation (item A)** -- name all long-lived
   threads via `seL4_DebugNameThread`. Pure win, no kernel patch,
   ~1 hour. Every fault print becomes legible
   (`"child of: 'dash@pid7'"` instead of
   `"child of: 'rootserver'"`). See `docs/SEL4_DEVInvestigation.md`.

2. **COW Step 3 wc/shutdown fix** -- mechanism is proven
   (parent_promotions count, no kernel errors with the gate on),
   blocker is finding which `do_fork` failure path fires
   post-promotion. Repro is one-line (flip `COW_STRIP_PARENT` in
   `src/process/cow.c`). Detailed plan in
   `docs/NEXT_20260503a.md`. Item B of SEL4_DEVInvestigation
   (verbose cap-fault dump) would directly help.

3. **file-backed mmap** -- the next POSIX VM piece. Extends
   `PIPE_MMAP_ANON` with file path + offset; fs_server reads the
   page; caller maps. `msync` write-back is the hard bit. See
   `BACKLOG.md`.

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
