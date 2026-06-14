# AIOS BACKLOG

Items deferred out of HANDOVER's "What is pending" so the active table
stays focused. Each entry lists what it would ship, the rough size, and
the most relevant reference. Promote items back into HANDOVER when
they're up next.

---

## Process requirement -- deeper pre-flash smoke (queued 2026-06-14)

**Before flashing ANY kernel to the real Pi, run the FULL QEMU gate suite**, not a
targeted smoke: `netd_qemu_test.py` 10/10, `net_socket_qemu_test.py` 8/8 (flag-ON
AND flag-OFF), `ssh_qemu_test.py` 6/6 (+ `smp_qemu_test.py` for the >=30-pipeline
ceiling when capacity could move). The DVFS Phase-0 flash (v0.4.241 build 2217,
2026-06-14) shipped with only a `cat /proc/cpufreq` smoke -- consciously, a research
kernel under heat pressure -- but root-task changes (the main-loop counter + procfs)
can regress boot/net/fork in ways a targeted smoke misses. Make the full suite the
default gate; skip it only with an explicit "research kernel, proceeding" call and
say so in the deploy notes.

---

## Next up -- recommended order (queued 2026-06-03)

Execution order set after the v0.4.143 pipe-EOF fix shipped: reliability
first, then a clean feature, then efficiency, then the high-risk/hardware
item. Intended to be `/schedule`-d as one-per-day sessions; recorded here so
the order survives regardless of the scheduler.

1. **Harden pipes under load -- INVESTIGATED 2026-06-03, DEFERRED (resource
   ceiling).** Root cause is NOT the pipe path: it is a **resource ceiling** --
   `MAX_ACTIVE_PROCS = 16` (root_shared.h, BSS-shift hazard to change), VKA pool
   8000 pages, morecore 6 MB/proc -> ~16 concurrent procs max. Under heavy
   concurrency the failures CASCADE: VKA/slot pressure -> PIPE_EXEC/do_fork fail
   (EPERM / "Cannot fork") -> a reader that fails to exec leaves the writer with
   no reader -> the writer's bytes are dropped (PIPE_WRITE `written<wlen`). 3+
   concurrent QEMU boots also overwhelm the host. These are largely artifacts of
   ARTIFICIAL multi-QEMU host-CPU contention; single-instance + real RPi4 work
   reliably. A secondary, genuine pipe-write **data-loss** bug exists (client
   advances `sent += chunk`, ignoring server `written`; 4096 ring drops overflow
   when the reader lags). A client busy-yield fix was tried (v0.4.144) and
   REVERTED -- it busy-spins on a full ring, adding pressure and deadlocking late
   readers. The only safe data-loss fix is server-side NON-spinning writer
   blocking (mirror pipe_read_blocked -> pipe_write_blocked, stash + resume on
   drain, EPIPE on read_closed) -- but it does NOT fix the load ceiling. A real
   "harden" needs capacity/admission work (swap, footprint reduction, careful
   limit raising) -- large, low payoff for real use. See docs/NEXT_20260603b.md.
   Repro: 2-3 concurrent QEMU `--smp 4` on separate disk copies (`--no-mirror`);
   an unclean QEMU kill corrupts `disk_ext2.img` -- regenerate via mkdisk.py.
   - **UPDATE 2026-06-07 (ceiling RAISED):** bumped `MAX_ACTIVE_PROCS` 16->48,
     `MAX_PIPES`/`MAX_ZOMBIES` 16->48, `PROC_MAX` 32->80, `MAX_WAIT_PENDING` 8->16.
     The feared "BSS-shift hazard" did NOT materialize -- QEMU -smp 4 boots clean
     and the parallel-pipeline ceiling went 6 -> 22 (verified by
     `scripts/smp_qemu_test.py`: fork-width probe clean to W=22, races exact). The
     binding limit at 48 is still the `active_procs` table (W=24 = 48 procs +
     system overflows it). See the ceiling investigation in HANDOVER for how high
     it can go and the next wall (VKA pool / per-proc footprint).
   - **UPDATE v0.4.181 (footprint -- ELF demand-text DONE, Phase 1):** the wall
     above the table is per-proc resident footprint (NOT eager morecore -- that
     is already demand-paged: `BSS lazy pages=1580`). `pipe_server.c`
     `setup_demand_text` now demand-pages the read-only ELF text from the
     executable file (reusing the v0.4.146 file-fault engine), so a proc keeps
     resident only the code it executes (`text lazy pages=75` per storm proc),
     not the whole statically-linked binary. Boots clean, executes correctly
     (executability via Default_VMAttributes). Table raised 48->64 (ceiling ~30;
     W=24 now fully clean). **Phase 2 (bigger win, not yet done): SHARE one
     read-only `.text` copy across same-binary procs** (root keeps a {binary ->
     text frames} cache) -- one 75-page copy for N procs instead of N. Also still
     open: the netconsole relay stall caps how WIDE you can drive (separate item
     above), and a per-proc resource leak under sustained storms (the Race-B
     cascade in `scripts/smp_qemu_test.py`).

**Netconsole relay stalls under heavy concurrent output** (debug transport, LOW
priority; found 2026-06-07 building the SMP test). Driving a `>~8`-wide
`seq | wc -l &` storm THROUGH netconsole: every wc output arrives CORRECTLY, but
the trailing `aios# ` prompt never comes (the relay does not deliver the prompt /
detect the `dash -c` exit under the burst). NOT a fork/SMP bug -- the fork-width
probe runs clean to W=22 and the outputs are correct; only netconsole's prompt
framing stalls. Likely the relay's EOF/drain handling under a burst of concurrent
writers (same family as the netconsole receive bottleneck, [[feedback_netconsole_push_speed]]).
Test workaround: drive repeated rounds at width<=8 with a settle. Real fix:
harden the netconsole relay (bulk drain + reliable end-of-command framing) --
`src/apps/netconsole*.c` + the pipe relay.

2. **file-backed mmap** -- new POSIX VM feature; see the Medium-risk entry
   below. Self-contained, QEMU-testable, no hardware. ~300 LOC.

3. **COW Step 3 -- wc/shutdown post-promotion EPERM** -- efficiency win; see
   the Medium-risk entry below. One focused session, ~30 LOC + tracing.

4. **RPi4 SMP bring-up -- DONE v0.4.179 (2026-06-07, HW-VERIFIED).** SMP=4
   (`settings-rpi4.cmake KernelMaxNumNodes=4`) boots all 4 A72 cores via the
   elfloader spin-table (`Boot cpu id = 0x0` -> `Core 1/2/3 is up`); the kernel
   bootstraps 4-core SMP, AIOS boots, DHCP 192.168.0.8, ping 0% loss; `/proc/hw`
   cores=4, `/proc/version` says "4-core SMP".
   - The long-blamed `smp_boot.c:119` `while (!is_core_up(num_cpus))` hang was a
     **GHOST -- never an SMP bug.** The bring-up was INVISIBLE: the elfloader
     aux-uart driver (`bcm-uart.c` `bcm2835_uart_init`) skipped
     `uart_set_out(dev)`, so `plat_console_putchar` stayed NULL and every
     elfloader printf no-op'd. The v0.4.178 `dtoverlay=disable-bt` "make it
     visible on PL011" attempt made it WORSE: the elfloader console is
     serial1=the mini-UART (build-time DTB stdout-path), so disable-bt only
     disconnected the trace from the cable AND broke the kernel boot (root task
     drives the mini-UART) -> Pi unreachable, masquerading as a SMP hang.
   - **Fix:** register the elfloader mini-UART console (gitignored deps
     `bcm-uart.c` `uart_set_out`; its putchar is already bounded) + revert
     disable-bt -> known-good mini-UART @115200, `core_freq=250` (`mksdcard.py`
     + `hw/rpi4/config.txt`). Then elfloader + kernel + login all land on the
     same cable. See the `project_rpi4_smp` memory + `hw/rpi4/BOOT_NOTES.md`.

---

## Medium-risk

### RPi4 power/thermal -- DVFS (lower ARM clock at idle, not WFI)
- **Symptom**: the Pi runs very hot. The cause is AIOS's own idle policy, not the
  firmware: to keep the v0.4.228 TLBI/DVM stall cured, all 4 A72 cores idle-SPIN
  (no WFE/WFI) so the SCU stays clocked ([settings-rpi4.cmake:33-41](settings-rpi4.cmake);
  the root idle is `while(1){ seL4_Yield(); }` at [aios_root.c:582](src/aios_root.c:582)).
  So the cores never enter low-power idle -> ~full draw -> heat. The obvious fix
  (WFI at idle) is exactly what RE-OPENS the stall, so it is off the table until
  the stall is re-cracked (hard, separate; residual spawn-storm stall already open).
- **What ships**: cut dynamic power WITHOUT deep idle by lowering the A72 CLOCK --
  cores keep spinning at a lower freq, so the SCU stays clocked (stall-safe), and
  the firmware drops voltage with frequency (power ~ f*V^2 falls well). Two tiers:
  (a) a STATIC boot-time ARM-clock cap (immediate heat cut, throughput trade);
  (b) a LOAD-DRIVEN governor -- drop to min when the root idle loop is hot, raise
  to max under load. Reconciling rule: idle == LOW CLOCK, never WFI.
- **Already half-built**: the VC-mailbox clock path exists --
  [src/gpu/v3d.c:284-348](src/gpu/v3d.c:347) has VC_TAG_SET_CLOCK_RATE /
  GET_CLOCK_RATE + a working `v3d_vc_tag()` helper. Reuse it with the ARM clock id
  (CLK_ARM = 3). A static cap is a few LOC; the governor needs a load signal + a
  small control loop. Temp read is just as easy (GET_TEMPERATURE tag 0x00030006).
- **Size**: static cap ~1 session; load-driven governor ~1-2 sessions + real-Pi
  thermal tuning.
- **Risk/verify**: HW-only. Confirm a lower clock does NOT re-trigger the stall
  (cores still spin, so it should hold) and does not disturb the mini-UART baud
  (tied to the SEPARATE core_freq=250). Firmware already throttles ~80-85C, so
  this is about running cool, not safety.

### COW Step 3 -- wc/shutdown post-promotion EPERM
- **What ships**: enables `COW_STRIP_PARENT 1`. With strip on, dash forks
  for `wc`/`shutdown` post-promotion fail with EPERM. Mechanism is
  proven (parent_promotions counts, no kernel errors); a downstream
  state divergence kills subsequent fork+exec.
- **Repro**: flip the gate in [src/process/cow.c:44](src/process/cow.c).
- **Plan**: instrument `do_fork`'s 12 `return -1` paths to find which
  fires post-promotion. Most likely culprits: cap allocation interacting
  with the orphaned parent_cap, or a child cspace cap copy that ends
  up wrong.
- **Size**: ~30 LOC of fix on top of the diagnostic; one focused session.
- **See**: [docs/NEXT_20260503a.md](docs/NEXT_20260503a.md).

### Block cache write-back
- **What ships**: switch from write-through to write-back, with periodic
  flush. AIOS fs traffic is currently too low to make this measurable.
- **Size**: ~150 LOC.

### file-backed mmap
- **What ships**: `MAP_SHARED` on a regular file. Extends `PIPE_MMAP_ANON`
  with a file path + offset, fs_server reads the page into a fresh frame,
  caller maps it. `msync` for write-back is the hard bit.
- **Size**: ~300 LOC.

### COW Step 4 -- stack COW
- **What ships**: probe parent's stack tightly, share via `cow_setup_segment`.
  Previous attempt (NEXT_20260502b) collided with child's IPC buffer;
  bound the probe to the actual stack range.
- **Size**: ~200 LOC. Depends on Step 3 working in production.

### COW Step 5 -- parent-dies safety
- **What ships**: today, child holds R/O dups of parent's frames; if parent
  dies and `vspace_tear_down` frees the underlying frames, child caps
  dangle. Needs cookie-ownership transfer at fork time (or refcount-driven
  free in `cow_frame_release`).
- **Size**: uncertain, touches sel4utils internals.

---

## High-risk

### Server health probes -- full (with auto-restart)
- **What ships**: extends v0.4.121 ping probe with restart on stale
  age. Detecting death is easy; restoring server state across restart
  is the hard part (BSS-resident state, in-flight reply caps,
  registered clients).
- **Size**: ~400 LOC.

---

## Long-term research

### Swap / paging out
- **What ships**: anonymous-page eviction to disk + page-in on fault.
  Needs a swap area, an LRU policy across active_procs vspaces, and
  fault-handler integration.

---

## Known bugs & limitations (low-priority)

### GENET real-MAC read fails -> Pi takes .127 not .8 (HARMLESS)
- **Symptom**: on the real RPi4 the board takes DHCP lease `.127` (the fake
  fallback MAC dc:a6:32:01:02:03) instead of `.8` (the real MAC). HARMLESS -- the
  Pi works at either IP; check both. Appears consistent since HDMI+v3d were
  enabled (v0.4.168+); the older note in [[genet-umac-swinit]] called it
  "intermittent".
- **Root cause (HW-narrowed, build 2171)**: net_genet's mailbox MAC read
  (`genet_mbox_call` / `read_mac_from_mailbox`) returns `ret=-1` EVERY time --
  confirmed by a fully-settled post-boot `cat /proc/genet.mac` (so it is NOT a
  boot-timing race). Yet `display_vc`'s `mbox_call` to the SAME VC property
  mailbox (channel 8) SUCCEEDS at boot ("Display server ready 1024x768"). The
  mailbox HW works; net_genet's CALL is broken.
- **Ruled out** (all checked on HW): high DMA address (genet_dma is low ~4MB);
  display contention (net inits at aios_root.c:384, before display at :387);
  VC-not-ready / read-too-early (it fails fully post-boot too).
- **Prime suspect**: tag-buffer region or cache coherency. `display_vc` PINS its
  tag buffer low at `MBOX_TAG_PADDR=0x3A000000` (the v0.4.168 HDMI fix);
  net_genet uses `genet_dma+0x10000`. Compare the two `mbox_call` paths: tag
  placement, the `|0xC0000000` bus alias, cached-vs-cleaned tag write. Likely
  fix: give net_genet a VC-reachable (pinned-low, non-cached, cleaned) tag buffer
  like display_vc's.
- **First step**: instrument `genet_mbox_call` (log WHICH poll fails + `buf[1]`),
  or just point `read_mac_from_mailbox` at display_vc's proven tag region, then
  one reflash.
- **CLEANUP owed**: the v0.4.234 retry (genet_init, 3x) + v0.4.235 deferred
  re-read (net_server, 5x) each spin the ~2s mailbox timeout for nothing (~14s of
  wasted boot polling, all failing). REVERT both to a single attempt as part of
  the real fix.
- **Size**: ~1 instrumented reflash + ~20 LOC. **See**: [[genet-umac-swinit]].

### PTY/SSH: last command before `exit` can lose its output (queued 2026-06-06)
- **Symptom**: in an interactive PTY session (observed over SSH), the LAST
  command's stdout immediately before `exit` can be dropped -- the client
  never receives it. Identical pipelines earlier in the SAME session work.
- **Repro (automated)**: SSH in and feed one input blob
  `echo abc | wc -c\nls /bin | wc -l\necho hello | rev\nexit\n`. The first two
  print (`4`, `114`); the third (`echo hello | rev`, last before `exit`) prints
  nothing client-side. Reordering it earlier makes it print -- so it is
  position-before-exit, not `rev`-specific.
- **Hypothesis**: a relay-teardown drain race. When dash runs the last command
  then `exit`, it writes output to its stdout pipe and exits ~immediately; the
  registered-writer EXIT latches the pipe `write_closed`, and the relay's
  non-blocking `read()==0` (EOF) path in `ssh_channel.c:channel_relay` may fire
  and tear the channel down before the final buffered bytes are drained +
  framed to the socket. (Pipe semantics SHOULD return buffered data before EOF,
  so this needs confirming -- it may instead be a dash exit-flush issue, or a
  test-capture timing artifact.) Same family as the rc=255 cosmetic (sshd never
  sends `exit-status` before CHANNEL_CLOSE per RFC 4254 6.10).
- **Where to look**: `src/ssh/ssh_channel.c` `channel_relay` -- on `read()==0`,
  do a final drain of any remaining pipe bytes before send_chan_eof/close; and
  dash's stdout flush on `exit`. Also check the A72 pipe-SHM coherency window
  (the relay may observe the writer EOF before the last written bytes are
  coherently visible -- QEMU cannot model it).
- **CONFIRMED ON HW (v0.4.178 deploy).** Once reconnect was fixed and many
  sequential SSH sessions ran on the real RPi4, this race became visible:
  ~35% of short sessions (`echo X; exit`) intermittently disconnect with
  "session ended" and NO output. QEMU got 6/6 (no cache lag); the Pi got 5/8,
  failing on conn 3/4/7 but PASSING 5/6 after -- so sshd RECOVERS, it is not a
  hard limit. This is now the main reconnect-reliability gap on hardware (the
  two reconnect leaks themselves are fixed). Interactive humans type commands
  then `exit` separately, so they still see output; the loss only bites the
  last-command-immediately-before-exit / one-shot pattern. NOT yet investigated.
- **Also makes `scp` return rc=1 on HW (v0.4.178 scp/sftp).** The transfer
  itself is correct (byte-verified both ways), but the channel `exit-status 0`
  packet is among the last-bytes-before-close that the A72 drops, so scp (which
  treats a missing exit-status as failure) exits non-zero. On QEMU it arrives ->
  scp rc=0. `sftp` is lenient and is rc=0 even on HW. So fixing this race also
  cleans up scp's exit code on hardware.

### zsh hangs over SSH -- ZLE raw-mode / termios not honored by the relay (queued 2026-06-06)
- **Symptom**: launching `zsh` inside an SSH session wedges -- typed commands do
  not run, `Ctrl-C` and `exit` do nothing. dash over SSH is completely
  unaffected. (Also: zsh emits OSC color / DA terminal queries whose replies the
  SSH relay echoes back as visible `]11;rgb:.../[?1;2c` noise, and `zsh/compctl`
  fails to load -- AIOS zsh is static, no loadable modules.)
- **IMPACT (worse than cosmetic): it takes sshd DOWN for ALL future connections.**
  sshd is one-connection-at-a-time, and `channel_relay` ends with a BLOCKING
  `waitpid(shell)`. With zsh hung, the `sshd -> dash -> zsh` chain never exits,
  so sshd blocks forever and never returns to `accept()` -- every later
  `ssh` just fails to connect. Client-side `~.` does NOT help (it closes the
  client; sshd is still stuck in waitpid). **Recovery: over netconsole 2323,
  kill the shell chain (`kill -9 <zsh-pid> <dash-pid>`, find them in
  `/proc/status`) -- killing BOTH is required (killing only zsh leaves dash, and
  sshd still waits on dash); or reboot.** Verified 2026-06-06.
- **Robustness sub-fix (independent of the termios work)**: sshd should not wedge
  the whole service on one hung shell -- on client disconnect, kill the shell
  (and reap) rather than block in `waitpid`; consider `waitpid(WNOHANG)` + a kill
  escalation in `channel_relay`. Small, high-value: makes sshd self-heal.
- **Cause**: `ssh_channel.c` (`channel_relay` + `process_input`) implements a
  FIXED cooked-mode server-side line discipline -- it echoes chars, line-buffers,
  and sends a whole line to the shell on Enter. dash expects exactly that. zsh
  drives ZLE, which sets the PTY to RAW mode via termios and wants
  character-at-a-time input + cursor control; the SSH relay IGNORES the shell's
  termios request and keeps line-buffering/echoing, so ZLE and the relay fight ->
  hang (input never reaches zsh as it expects; Ctrl-C/exit included). zsh works
  on the LOCAL console because that path honors termios via IPC (tty_server,
  [[is_tty_routing]] / v0.4.99 ZLE) -- only the SSH relay lacks it.
- **Fix direction**: make the SSH channel termios-aware. When the shell puts the
  PTY in raw mode, drop the server-side echo + line-buffering and relay raw bytes
  both directions, letting the shell own echo/line-editing (mirror the local
  tty_server termios path; sshd would need a termios channel to the shell, or to
  honor the pty-req modes + a SET_TERMIOS hook). Medium effort; also unlocks any
  raw-mode / full-screen app over SSH (vi-like editors, pagers, top-like UIs).
- **Workaround**: use dash (`#`) over SSH; do not launch zsh. Found v0.4.178.

## Tooling polish (small but deferred)

### Smoke-driver flakiness
- The python smoke driver occasionally fails to reach the dash prompt
  on first run (timing race with QEMU + getty + login). Workaround:
  retry once. Worth investigating with explicit prompt polling rather
  than fixed sleeps.

### SIGSEGV / fault-observation harness
- Would let us actually verify `mprotect(PROT_NONE)` faults reads, and
  `mprotect(R/X)` clears XN. Today the IPC return is real but the user
  has no way to observe the page-fault outcome.
