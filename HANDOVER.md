# AIOS HANDOVER

Self-contained briefing for a fresh development session. Read this end-to-end,
then the latest `docs/NEXT_*.md` and the memory index (`MEMORY.md`) for deeper
background. Older session arcs (v0.4.110 -> v0.4.168) live in
`docs/HANDOVER_HISTORY.md`.

---

## Quick orientation

* **Project**: AIOS (Open Aries) -- microkernel research OS on seL4.
* **Repo**: `~/Desktop/github_repos/AIOS`, branch `main`, at **v0.4.174**.
* **Target**: AArch64 (qemu-system-aarch64 + Raspberry Pi 4).
* **Host**: macOS Apple Silicon, cross-compile to aarch64-linux-gnu.
* **Developer**: Bryan -- prefers Python patch scripts over sed/heredocs; no
  apostrophes in C comments (zsh copy-paste breaks); commits via GitHub Desktop
  (commit only when asked; never amend / force-push / skip hooks).

---

## Workflow discipline -- READ THIS FIRST

The project goal is **"deploy over the network, flash only for major milestones."**
Honor it; this session learned the hard way what happens when you do not.

* **Develop + verify on QEMU.** The QEMU net harness NATs UDP, so even SNTP works.
  Smoke: `python3 scripts/netcon_qemu_test.py`; boot command under "Build and boot".
* **Push userspace over the LAN.** `python3 scripts/pi_filexfer.py push <local>
  /bin/<tool> 192.168.0.8`, then run it over netconsole. Reboot the Pi IN PLACE
  (`reboot` over netconsole -> BCM2711 watchdog). NO reflash for userspace apps.
* **Flash only for KERNEL / root-task changes, and only at real milestones.** Batch
  several changes into ONE flash. A userspace-only app does NOT bump `version.h`.
* **When HW debugging needs iteration, use SERIAL -- never flash-iteration.** This
  session burned 4 flashes chasing a netconsole-v2 HW bug because netconsole lives
  on disk AND its wedge killed network access (the one case that breaks in-place
  update). Bail to QEMU / serial early.
* **QEMU cannot model:** RPi4 cache attributes, the VC mailbox, eMMC single-block
  write latency, GENET timing, and the fork/pipe/socket event-loop path. Verify
  those on the Pi -- but via push-over-net + serial, not reflashes.

---

## Where we left off (v0.4.172 -> v0.4.174)

Net result this session: a big WRITE-SPEED win (write-back cache, HW-verified), a
quick `ls -l` mtime win (QEMU), and a netconsole multi-session rewrite that FAILED
on hardware and was REVERTED. Full lesson: `netconsole-push-speed-hw` memory.

* **Write-back block cache (v0.4.172, `7f7b4a9`, HW-VERIFIED).** File writes were
  ~21-23 KB/s: `blk_cache.c` was write-through and `blk_emmc.c` did one single-block
  CMD24 per 512 B sector (~1000+ synchronous flash writes per 296 KB; the inode
  rewritten on every `ext2_pwrite`). A LOCAL `cp` was as slow as a network push --
  the WRITE path, not the network, was the wall (proven by a local-cp test; the
  first guess blaming netconsole / the 32 KB rx ring was WRONG). Fix: drive-0
  WRITE-BACK (dirty bit; flush at a 16-line/64 KB threshold + on eviction + on
  shutdown/reboot; drive 1 log stays write-through for crash durability) +
  `plat_blk_write_multi` = CMD25 multi-block (one eMMC transfer per 4 KB line) +
  flush-before-shutdown/reboot in `aios_root.c`. HW: local cp 296 KB **12.6 s ->
  2.8 s (4.5x)**; CMD25 bytes correct (cp of /bin/dash byte-identical after a cold
  reboot); persistence works.
* **`/proc/version` real (v0.4.172).** Was hardcoded "0.4.x"; now `AIOS_VERSION_FULL`
  + build + date (the same macros `uname` uses, so they cannot drift). `uname -r`
  was already real (`fs_server` FS_UNAME -- a separate path).
* **netconsole v2 -- ATTEMPTED + REVERTED (v0.4.173 `023b5b7` -> revert `769d634`).**
  A non-blocking MULTI-SESSION event-loop rewrite (`docs/DESIGN_NETCONSOLE_V2.md`
  Option B) + a `net_server` non-blocking accept (NET_ACCEPT EAGAIN). Passed EVERY
  QEMU test (smoke 9/9, reconnect-stress 10/10, concurrency, no-wedge) but STALLED
  EVERY COMMAND on the real RPi4 -- the forked-dash -> output-pipe -> socket relay
  never delivered over GENET/A72. Found + fixed one real bug (the forked child
  closed fork-shared session sockets via NET_CLOSE_SOCK, tearing down the parent's
  connections) but it did NOT restore HW function. After 4 flashes, REVERTED to the
  v1 single-client netconsole. **Retry needs SERIAL debugging, not flash-iteration.**
* **`ls -l` mtimes -- READ path (v0.4.174, `48b28aa`, QEMU-verified).** v0.4.171
  WRITES `i_mtime` on create/mkdir; now the READ path shows it. Threaded mtime
  through `fs_stat`/`vfs_stat` -> `ext2_vfs_stat` (`i_mtime`) -> `fs_server` FS_STAT
  (MR3, was a reserved 0) -> libaios `fetch_stat_m` -> `statx`/`fstatat` fill. `ls
  -l` now shows real 2026 dates, no epoch. Backward-compatible (old binaries ignore
  MR3); only sbase rebuilt. No flash -- rides the next milestone image.

**Current state:** the Pi runs **v0.4.172** (write-back + old netconsole) on the LAN
at **192.168.0.8:2323** -- working; drive it with ~4 s settle delays between
connections. The repo is at **v0.4.174** (+ `/proc/version`, mtimes). The mtime
change and any future work batch into the NEXT milestone flash.

---

## Where we left off (v0.4.169 -> v0.4.171) -- COLOUR + 3D + NETWORK DEPLOY (condensed)

The display + network-deploy arc before this session (committed through `38d1f6c`):
RPi4 HDMI colour fix (`SET_PIXEL_ORDER=0`/BGR -- we write LE 0x00RRGGBB), a 1024x768
logo + a CPU software 3D spinning cube (`fbshow --cube`), then a large-file
network-deploy stack: netconsole robustness (the surgical single-client subset --
non-blocking + per-op deadlines), a net_server TCP receive flow-control fix, ext2
double-indirect WRITE (files >268 KB), a 32 KB rx-ring (the "speed" bump that this
session proved does NOT help HW push -- the wall was the write path), and
`aios_console.py monitor` (passive serial tap). HW-verified except that 32 KB-ring
speed claim. Detail: git history + the memories.

---

## Earlier arcs (v0.4.110 -> v0.4.168)

RPi4 HDMI (Phase B VC mailbox), GENET networking (DHCP + bidirectional ping),
network control (netconsole, watchdog reboot, file push/pull, SNTP wall-clock), COW
fork, demand-paged BSS, block-layer cache, TCC self-host, and more -- condensed
records are in **`docs/HANDOVER_HISTORY.md`**, with full per-session detail in
**`docs/NEXT_*.md`** and the memory index.

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
* `docs/HANDOVER_HISTORY.md` -- older session arcs (v0.4.110 -> v0.4.168)
* `docs/DESIGN_NETCONSOLE_V2.md` -- multi-session rewrite (reverted; retry WITH serial)
* `docs/DESIGN_RPI4_3D.md` -- V3D hardware-3D plan; `DESIGN_COW_FORK.md` -- VM design
* `include/aios/root_shared.h` -- IPC labels, active_proc_t
* `include/aios/blk_cache.h` -- block cache (write-back) interface
* `src/blk_cache.c` + `src/plat/rpi4/blk_emmc.c` -- write-back + CMD25 (v0.4.172)
* `src/servers/fs_server.c` + `src/ext2.c` + `src/lib/posix_stat.c` -- fs + stat/mtime
* `src/apps/netconsole.c` -- v1 single-client netconsole (current; v2 in git history)
* `src/servers/net_server.c` -- TCP/UDP socket server
* `src/servers/pipe_server.c` -- central IPC hub, fault dispatcher
* `src/process/fork.c` -- eager-copy fork

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
* **Block cache is WRITE-BACK on drive 0** (v0.4.172). Dirty pages flush at a
  16-line/64 KB threshold + on eviction + on shutdown/reboot (`blk_cache_flush`).
  A HARD power-cut loses the last unflushed writes (expected); `reboot`/`shutdown`
  flush first. eMMC line flushes use CMD25 multi-block. Drive 1 (log) stays
  write-through for crash-log durability.
* **Drive the Pi over netconsole GENTLY.** The v1 single-client netconsole wedges
  under RAPID back-to-back connections: use ONE held-open connection for many
  commands and a ~4 s settle between SEPARATE connections (push/pull/reboot). A
  fresh connection per command, or retry-without-close, reliably wedges it -- and a
  wedge blocks network access, so recovery needs a power-cycle.
* **close() on a socket fd sends NET_CLOSE_SOCK, and fork shares socket_id**
  (`posix_file.c`). A forked child closing a session socket tears down the PARENT's
  connection -- the netconsole-v2 HW trap. dash's EXIT drops inherited fds WITHOUT
  NET_CLOSE_SOCK, so a child INHERITING sockets is fine; CLOSING them is not.
* **QEMU transfer/write speed lies.** The 32 KB rx-ring "speedup" (v0.4.171) was
  QEMU-only; on HW the write path (now fixed) then the receive path are the walls,
  not the TCP window. Measure transfer/write SPEED on the Pi, never trust QEMU.

---

## What works "out of the box" right now

After boot + login:

```
ls                              # 104 sbase tools in /bin
ls -l /tmp/somefile             # REAL mtimes now (v0.4.174), not the epoch
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

**This session (v0.4.172->174): write-back cache (HW-verified, 4.5x faster writes),
`/proc/version`, `ls -l` mtimes (QEMU), and a netconsole-v2 rewrite that FAILED on HW and
was REVERTED. Top picks for next -- pick one:**

1. **netconsole v2 retry -- WITH SERIAL.** The multi-session event-loop rewrite is in git
   history (`023b5b7`, reverted by `769d634`) + `docs/DESIGN_NETCONSOLE_V2.md`. It is
   QEMU-clean but stalls EVERY command on HW (the forked-dash -> output-pipe -> socket relay
   never delivers over GENET/A72). DO NOT flash-iterate. Capture serial on the first boot
   (`python3 scripts/aios_console.py serial /dev/cu.usbserial-0001 --login --log /tmp/boot.log`
   -- Pi OFF first, power on ~5s after starting it), trigger ONE command, watch the actual
   failure (fault? fork? pipe read stuck?). See `netconsole-push-speed-hw` memory. The one bug
   already found + fixed (the forked child closing fork-shared sockets via NET_CLOSE_SOCK) is
   correct but insufficient.
2. **Deploy PUSH speed.** Still ~21 KB/s -- the netconsole RECEIVE path (900 B socket reads +
   per-read window-ACK chatter), NEVER solved (write-back fixed the WRITE side; the receive
   side is now the wall). Fix = a `net_server` bulk-receive (shared frame so netconsole reads
   KBs per syscall, mirroring `__get`) and/or throttle the per-900 B window update
   (`net_server.c:572`). HW-only to verify. Needs a WORKING netconsole first (so: after #1).
3. **Milestone flash.** The Pi runs v0.4.172 (write-back). The next flash should bring mtimes
   (v0.4.174) + whatever else lands -- batch changes. `ninja -C build-04 && ninja -C build-rpi4`
   -> rebuild userspace (sbase/dash/zsh/netconsole if libaios changed) -> mkdisk -> mksdcard ->
   balenaEtcher.
4. **getty netconsole auto-respawn.** Tried + REVERTED (AIOS fork-of-fork fails). Needs a getty
   `waitpid(-1)` event loop that does not block on serial login-auth.
5. **kernel-over-network** -- write `kernel8.img` to the FAT boot partition + reboot (the last
   flash-elimination piece). Needs FAT-partition WRITE (AIOS mounts/writes only ext2). Meaty.
6. **hardware 3D (V3D)** -- `docs/DESIGN_RPI4_3D.md` (~3-6 weeks; minimal register-level driver;
   the IV-vs-VI trap + A72<->V3D cache coherency are the load-bearing risks).
7. **RPi4 SMP bring-up** -- v0.4.135's SMP=4 hangs in the elfloader spin-table. HW-gated.

**Lower-priority:** scp/sftp (blocked on the lost mbedTLS; the SSH server exists);
Bluetooth/HCI (`docs/DESIGN_BLUETOOTH_HCI.md`, console-safe PL011 UART but needs a blob + stack).
The deferred VM backlog (COW Steps 3-5, block-cache write-back-for-log, swap) is in
[BACKLOG.md](BACKLOG.md).

**If hardware is unavailable:** most logic is QEMU-testable (the net harness NATs UDP, even
SNTP works) -- write-back correctness, mtimes, the netconsole protocol, fs/VM. But the
fork/pipe/socket relay, eMMC write speed, GENET timing, and cache attributes are HW-only.

---

## Final notes

The system is in a strong place: stable boot on QEMU + real RPi4, demand paging,
real GENET networking (DHCP, ping), a netconsole control channel (drive the Pi over
the LAN -- run commands, push/pull files, reboot), real wall-clock time via SNTP, a
write-back block cache (4.5x faster file writes), `ls -l` mtimes, working shell + ZLE,
TCC for simple programs. Drive the live Pi over `scripts/pi_filexfer.py` / a held-open
socket to `192.168.0.8:2323` (with settle delays) instead of the lossy mini-UART. The
deploy PUSH is functional but slow (~21 KB/s, receive-path bound -- the next target);
PULL is fast (~1 MB/s).

When in doubt:
* Check `cat /proc/log` and `cat /var/log/aios.log` for traces
* `cat /proc/vka` to see if memory pressure is the culprit
* `cat /proc/genet.ip` (RPi4) for one-line network status
* Look at `[INF]` / `[WRN]` / `[ERR]` tagged lines on serial -- the
  module name (boot, fs, blk, exec, pipe, vka, gpu, net, etc.) tells you
  which subsystem to read

Good luck.
