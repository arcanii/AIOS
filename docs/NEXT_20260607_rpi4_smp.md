# NEXT 2026-06-07 -- RPi4 SMP bring-up  [RESOLVED v0.4.179, HW-VERIFIED]

## RESOLVED -- 4-core SMP works on real hardware (2026-06-07).

`KernelMaxNumNodes=4` boots all 4 A72 cores via the elfloader spin-table
(`Boot cpu id = 0x0` -> `Core 1/2/3 is up`), the kernel bootstraps 4-core SMP,
AIOS boots, DHCP 192.168.0.8, ping 0% loss; `/proc/hw` cores=4, `/proc/version`
says "4-core SMP".

**The `smp_boot.c:119` "hang" this doc chases was a GHOST -- never an SMP bug.**
The bring-up was INVISIBLE for two unrelated reasons, and the plan below (move
the trace to the PL011 via `disable-bt`) was WRONG:
- The elfloader had **no registered console**: `bcm-uart.c bcm2835_uart_init`
  skipped `uart_set_out(dev)`, so `plat_console_putchar` stayed NULL and every
  elfloader printf no-op'd. Un-gating `common.c` printf was necessary but not
  sufficient.
- The elfloader console is **serial1 = the mini-UART** (build-time DTB
  stdout-path), NOT the PL011. So `dtoverlay=disable-bt` only DISCONNECTED the
  mini-UART trace from the cable AND broke the kernel boot (the root task drives
  the mini-UART at 0xFE215000) -> Pi totally unreachable, which masqueraded as a
  SMP hang. It also skewed the PL011 baud to 103448 (a red herring).

**Fix (committed v0.4.179):** (1) `bcm-uart.c bcm2835_uart_init` now calls
`uart_set_out(dev)` -- the elfloader prints on the mini-UART; its putchar is
already bounded (`for t<100000` on MU_LSR TXIDLE) and `uart_2ndstage=1` keeps it
clocked, so no hang. (2) REVERTED disable-bt -> known-good mini-UART @115200,
`core_freq=250` (`mksdcard.py` + `hw/rpi4/config.txt`). (3) `KernelMaxNumNodes=4`.
Now the elfloader trace, kernel boot and login all land on the SAME cable.
Capture at 115200: `scripts/aios_console.py monitor /dev/cu.usbserial-0001`.
Static audit had already cleared the real suspects (CPU table + release addrs
0xd8/e0/e8/f0 correct, driver binds, relocator runs the whole elfloader at link
addr so `&secondary_startup` is valid, and the handshake runs MMU-OFF so it is
coherent -- cache coherency was a red herring too). The TRACKED commit is
`settings-rpi4.cmake` + `version.h` + config/docs; the elfloader edits are
gitignored `deps/` (re-apply if a deps reset wipes them). See the
`project_rpi4_smp` memory.

---

## ORIGINAL SEED (in-progress, 2026-06-07) -- kept for history; the disable-bt/PL011 plan below was WRONG (see RESOLVED above)

Seed for a focused HW-serial session. SSH work (self-heal + scp/sftp) is DONE,
committed, and deployed. The active task is **RPi4 SMP** -- we just made the
elfloader's secondary-core bring-up visible on serial and need to read the
trace. Today's date when this was written: 2026-06-07.

## TL;DR -- the one thing to do next
1. The baud-fixed SD image is built (`disk/sdcard-rpi4.img`). Re-flash it
   (balenaEtcher) OR just edit the card's `config.txt` (see below), boot the Pi,
   and **capture the elfloader trace on serial at 115200 8N1**.
2. Look for: `Boot cpu id = 0x0, index=0` then `Core 1 is up ...` /
   `Core 2 ...` / `Core 3 ...`.
   - All three "Core N is up" -> **SMP secondary bring-up WORKS.** Serial then
     goes quiet (kernel+root-task are on the mini-UART, disconnected by
     disable-bt) -- confirm AIOS booted via netconsole/ssh (192.168.0.8). Then
     commit the SMP work + decide on a version bump.
   - `Boot cpu id...` then STOPS (no "Core 1 is up") -> the hang is at
     `smp_boot.c:119` `while(!is_core_up(num_cpus))`. Note which core. Debug per
     the hypotheses below.

## What is set up (this session)
- **SMP enabled:** `settings-rpi4.cmake` `KernelMaxNumNodes 1 -> 4` (UNCOMMITTED).
  Verified: `build-rpi4/kernel/gen_config/.../gen_config.h` has
  `CONFIG_MAX_NUM_NODES 4` + `CONFIG_ENABLE_SMP_SUPPORT 1`. build-rpi4 rebuilt.
- **Elfloader trace made visible** -- the elfloader was silent on RPi4 by design
  (mini-UART hangs its CPU bus). Two edits in **gitignored `deps/` (UNTRACKED --
  re-apply if a deps reset wipes them; they ARE baked into the current
  build-rpi4 image):**
  1. `deps/seL4_tools/elfloader-tool/src/common.c` -- REMOVED the
     `#ifdef CONFIG_PLAT_BCM2711 / #undef printf / #define printf(...) ((void)0)`
     gate (was lines ~36-41). printf is safe now: with disable-bt the pl011
     driver drives the PL011; with no PL011 no UART registers and printf no-ops
     via the NULL plat_console_putchar check.
  2. `deps/seL4_tools/elfloader-tool/src/drivers/uart/pl011-uart.c` --
     `pl011_uart_putchar` now has a BOUNDED wait (`for t<100000`) instead of
     `while(TXFF)`, so a not-ready PL011 degrades to a no-op instead of hanging.
- **config.txt: PL011 on the pins.** `scripts/mksdcard.py:create_config_txt`
  now emits `dtoverlay=disable-bt` (PL011 -> GPIO 14/15) + `init_uart_clock=48000000`
  and DROPS `core_freq=250`/`core_freq_min=250` (UNCOMMITTED). `hw/rpi4/config.txt`
  also got disable-bt (but mksdcard generates its own, so that file is moot).

## The baud gotcha (already fixed -- do not re-debug)
First flash booted and we SAW firmware output -> **disable-bt works, PL011 is on
the cable.** But the firmware logged `uart: Set PL011 baud rate to 103448.3 Hz`
-- `core_freq=250` (a mini-UART-era hack; the mini UART baud tracks core_freq)
dragged the PL011 clock to ~43 MHz so 115200 became ~103448, garbling the
elfloader trace at a 115200 terminal. Fix = drop core_freq + pin
`init_uart_clock=48000000` (done in mksdcard.py + the regenerated image). The PL011
has its own clock; this restores a clean 115200. If the trace is STILL garbled
at 115200 after the new image, the fallback is to have the elfloader's
pl011_uart_init program the divisor itself (IBRD/FBRD for 115200 @ 48 MHz).

## The hang (if it hangs) + hypotheses
`smp_boot.c:113` calls `plat_cpu_on` per secondary, then `:119`
`while(!is_core_up(num_cpus))` busy-waits forever. `smp_spin_table_cpu_on`
(`drivers/smp-spin-table.c`) writes `&secondary_startup` to the DTB
`cpu-release-addr`, dmb/dsb/sev, returns. A secondary that never runs
secondary_startup (or crashes in it) -> is_core_up stays false -> hang.
Test once the trace is readable:
- (a) **wrong/absent `cpu-release-addr`** in the build-rpi4 DTB (firmware passes
  it in x0; with disable-bt check the cpu nodes have spin-table + release addrs).
- (b) **firmware not parking secondaries** in spin-table mode (config / arm_64bit).
- (c) **secondary crashes in `secondary_startup`** (`64/crt0.S`) -- stack/MMU.
Cache-coherency of the release write is LESS likely (driver does dmb+dsb to PoC
on the coherent A72). seL4 upstream supports bcm2711 SMP, so suspect AIOS's
relocator stub / DTB / config first.

## Uncommitted changes to commit (if SMP works) or revert (if abandoning)
Tracked: `settings-rpi4.cmake` (MAX_NUM_NODES 4), `scripts/mksdcard.py`
(disable-bt + init_uart_clock - core_freq), `hw/rpi4/config.txt` (disable-bt),
`BACKLOG.md`. Untracked (deps/, document-don't-rely-on-git): the two elfloader
edits above.

## Recovery if it will not boot
Revert to the known-good single-core image: `settings-rpi4.cmake`
KernelMaxNumNodes -> 1; in `mksdcard.py` restore `core_freq=250` +
`core_freq_min=250` and remove disable-bt + init_uart_clock; (optionally restore
the common.c printf gate); `ninja -C build-rpi4`; `python3 scripts/mksdcard.py`;
reflash. The Pi was happily single-core at v0.4.176 + pushed sshd before this.

## Done this session (committed + deployed -- context, not todo)
- SSH **reconnect** fixed (654d722, ab27f84): the cause was NOT the fork (fork-free
  spawn was built + reverted) -- it was 2 userspace leaks: O_NONBLOCK fd-slot
  reuse (aios_fd_alloc now zeroes slots) + auth session not released (sshd now
  AUTH_LOGOUT). 6/6 on `scripts/ssh_qemu_reconnect.py`.
- SSH **self-heal** + **scp/sftp** (8df0a58, 6698db7): relay SIGKILLs a hung
  shell on disconnect (no more wedge); new `src/ssh/ssh_sftp.c` SFTP v3
  subsystem (sftp + scp work; sftp rc=0 on HW, scp transfers byte-verified but
  rc=1 on HW from the deferred drain race). Also fixed `pwrite64`/`pread64`
  (were capping at 3000 + packing all MRs -> KERNEL HALT on >1KB pwrite; ignored
  offset). All userspace, deployed to the Pi by push (no flash).
- DEFERRED (BACKLOG): SSH output-drain race (A72, one-shot output loss; also the
  cause of scp rc=1 on HW) + zsh-over-SSH termios + the sshd self-heal is the
  fix for the zsh wedge.

Test scripts (in /tmp this session, not committed): ssh_selfheal_test.py,
sftp_qemu_test.py, scp_qemu_test.py, pi_scpsftp.py, pi_selfheal.py. The
committed regression test is `scripts/ssh_qemu_reconnect.py`.
