# RPi4 Hardware Test Plan -- v0.4.134

First boot of an AIOS build that has never been validated on real
hardware. This file tells you what to set up, what each phase should
look like on the serial console, what to log, and what to do when
something doesn't match.

The artefact under test is `disk/sdcard-rpi4.img` (193 MB at v0.4.134).
Flashing produced it via `scripts/flash-rpi4.sh /dev/diskN`.

> **UPDATE 2026-06-02 -- first boot done (v0.4.135).** SMP did NOT come
> up: v0.4.134 (SMP=4) hangs at the firmware-to-kernel handoff, so we
> fell back to single-core (`KernelMaxNumNodes=1` = v0.4.135), which
> boots to login. Pipes are broken on RPi4 (platform-specific, not the
> fallback). Flashing gotcha: the macOS built-in SDXC reader trips the
> `flash-rpi4.sh` internal-disk guard -- use balenaEtcher and hash-gate
> the card before booting. Full record: `docs/NEXT_20260602a.md`.

---

## Physical setup

* Raspberry Pi 4B (any RAM size, 2GB or 4GB tested target).
* SD card flashed with `disk/sdcard-rpi4.img`. Anything 256 MB or
  larger works; the image is 193 MB.
* USB-to-serial UART adapter (3.3 V TTL, e.g. an FTDI or CP2102 cable).
* Optional: HDMI monitor + USB keyboard. The tty driver on RPi4 has
  worked over HDMI in past sessions but the documented serial path is
  the reliable one.

### Serial wiring (RPi4 GPIO header)

| RPi4 pin | label  | adapter side |
|----------|--------|--------------|
| 6        | GND    | GND          |
| 8        | TXD    | RX           |
| 10       | RXD    | TX           |

Do **not** connect adapter VCC to a Pi pin. Power the Pi from its USB-C
supply.

### Console settings

```
115200 8N1, no flow control
```

On macOS:

```
ls /dev/tty.usb*           # find the adapter, e.g. /dev/tty.usbserial-XXXX
screen /dev/tty.usbserial-XXXX 115200
# Exit: Ctrl-A, k, y
```

To capture the boot to a file:

```
screen -L -Logfile ~/aios-boot-$(date +%s).log /dev/tty.usbserial-XXXX 115200
```

---

## Boot phases

Phases are listed in the order you should see them after power-on. If
the actual output stops earlier than the listed phase, jump to the
"diagnosis" section keyed by where it stopped.

### Phase 0 -- power-on

* Red PWR LED solid (it's on as long as USB-C is supplying power).
* Green ACT LED blinks while firmware reads the FAT32 partition,
  then settles to off.
* Within 1-2 seconds you should see firmware traffic on the serial
  console.

If green ACT stays on solid for more than ~10 seconds: SD card not
booting. Re-check that the FAT32 partition is the first partition
and contains `start4.elf`, `bcm2711-rpi-4-b.dtb`, `config.txt`,
`kernel8.img`. `dd if=/dev/rdiskN bs=512 count=1 | xxd | head` should
show a valid MBR with partition type 0x0C at the appropriate offset.

### Phase 1 -- firmware

You may see:

```
MESS:00:00:00.000000:0: brfs: File read: /mfs/sd/config.txt
MESS:00:00:00.000000:0: brfs: File read: 162 bytes
...
MESS:00:00:00.000000:0: HDMI:EDID error reading EDID block 0 attempt 0
```

HDMI EDID errors are normal when no monitor is connected. Add
`uart_2ndstage=1` to `config.txt` (in the FAT32 partition) to enable
firmware debug messages on this UART if you want more detail. Reflash
not required -- mount the FAT32 partition and edit in place.

### Phase 2 -- elfloader

```
Loaded DTB from <addr>
   paddr=[<range>]
Boot cpu id = 0x0, index=0
Core 1 is up with logic id 1
Core 2 is up with logic id 2
Core 3 is up with logic id 3
Enabling hypervisor MMU and jumping to entry point...
```

**This is the SMP smoke test.** v0.4.134 is the first build where
`KernelMaxNumNodes=4`. We need all four "Boot cpu id ... Core N is up"
lines.

If only "Boot cpu id" appears and there's no "Core 1 is up": only the
boot core came up. See "SMP fallback" in diagnosis. The system *can*
still continue if the kernel doesn't strictly require all configured
cores, but it usually does.

If the elfloader is silent (no DTB line, no Boot cpu): the kernel
relocator stub is running but elfloader printf isn't reaching this
UART. See "elfloader silent" in diagnosis.

### Phase 3 -- kernel + root task

```
Bootstrapping kernel
available phys memory regions: ...
Booting all finished, dropped to user space

============================================
  AIOS v0.4.134 (build 1693)
============================================

[hw] DTB: parsed
[hw] CPU: 4 core(s), arm,cortex-a72
[hw] RAM: 4096 MB @ 0x...        (<- depends on the model)
[hw] UART: 0xfe215040 IRQ ...    (<- mini UART, AUX 0xFE215000 + 0x40)
```

Check that `[hw] CPU: 4 core(s)` matches the SMP boot above. If
`[hw] CPU: 1 core(s)` despite the elfloader showing 4, the kernel is
configured single-core somehow -- shouldn't happen at v0.4.134 but
worth flagging.

Then in order:

```
[plat] ...                          # block device probe
[INF] blk: ...
[fs] ...
[vfs] Mounted /
[vfs] Mounted /proc
[INF] boot: Filesystems mounted
[boot] hostname: aios
[INF] boot: Crypto server started
[INF] srvstat: serverstats probe thread started
[crypto] server started, initial seed collected
[disp] Display server ready (...)
[INF] auth: server ready
[INF] warmup: ...                   # boot prefetch
[exec] /bin/aios/getty: ...
[INF] exec: loaded elf bytes=...
[INF] exec: BSS lazy pages=...
[INF] root: UART IRQ bound (notification) irq=...
```

If the boot stops at `[plat] Virtio probe` or similar: the platform
driver expects something the BCM2711 doesn't provide. RPi4 uses EMMC
for SD card, not virtio-blk; the platform driver under PLAT_RPI4
should be the EMMC driver. The boot artefact ships with the RPi4 plat
sources baked in -- if those crash, see "platform driver crash" in
diagnosis.

### Phase 4 -- login

```
============================================
  AIOS v0.4.134 (build 1693)
============================================

AIOS login: _
```

Type `root`, password `root`. You should land in dash with the `# `
prompt.

---

## Functional checklist (run these once you're at the shell)

Each line is one command; the expected output is below it. If any line
deviates, log the full transcript and note which line.

```
ls /bin
```
Expected: list of 38 sbase tools (cat, cp, mv, ls, etc.).

```
cat /proc/hw
```
Expected: 4 cores, cortex-a72, RAM in MB. Confirms SMP and CPU
identification work.

```
cat /proc/cmdline
```
Expected (v0.4.131): a line with `platform=rpi4 ...` and the kernel
load address. Confirms platform-aware /proc/cmdline works on RPi4.

```
cat /proc/serverstats
```
Expected (v0.4.121): a table with pipe/fs/thread/crypto in `ok` state,
non-zero `pings` and `last_ok_us` (latencies in the tens of
microseconds). Confirms the in-process server probe runs on this
hardware.

```
echo SMOKE-START
ls /bin > /tmp/o
wc -c /tmp/o
echo abc | wc -c
cat /etc/passwd | head -1
echo SMOKE-DONE
```
This is the same smoke driver we run under qemu-virt. All five lines
should produce normal output (608 bytes from `wc -c /tmp/o`, "4" from
`echo abc | wc -c`, etc.).

```
test_mprotect
```
Expected (v0.4.126/127):
```
OK: mmap returned 0x...
OK: pre-mprotect read/write works
mprotect(R/O) rc=0
OK: post-mprotect read works
mprotect(R/W) rc=0
OK: post-restore write works
mprotect(NONE) rc=0
mprotect(R/W after NONE) rc=0
OK: PROT_NONE round-trip works
mprotect(R/X) rc=0
OK: PROT_EXEC accepted
munmap rc=0
OK: munmap succeeded
OK: re-mmap after munmap works
MPROTECT-DONE
```
Confirms the entire mprotect + munmap + mmap round-trip works on
real hardware.

```
zsh
```
Expected: zsh prompt (`aios#` with a few ANSI escapes from ZLE) and a
warning `zsh: failed to load module: zsh/compctl` (cosmetic, known
since v0.4.99). Run `ls /bin | head -3` to verify forks/execs from
inside zsh, then `exit` back to dash.

```
shutdown
```
Expected: `Requesting system shutdown...` then `[shutdown] AIOS
powering off...` and the ACT LED stays off. Pulling power is then
safe.

---

## Diagnosis

### "Green ACT solid for >10 s, no serial output"

Firmware can't load `kernel8.img`. Common causes:

* FAT32 layout is wrong (regression in `mksdcard.py`). Cross-check
  that the image was built with v0.4.132+ (newfs_msdos path); the
  fallback mtools path is documented-broken.
* `kernel8.img` is missing from the FAT32 partition. Mount it on
  another machine and `ls /Volumes/AIOSBOOT/`.
* `config.txt` is wrong. Should contain `kernel=kernel8.img` and an
  appropriate `kernel_address=`.

### "Elfloader silent"

The kernel relocator runs, the kernel takes over, but no elfloader
prints reach the UART. Either the elfloader's printf is targeting the
wrong UART address, or it's been disabled in the build. Inspect
`build-rpi4/elfloader-tool` build artefacts; the bcm-uart driver is
at `deps/seL4_tools/elfloader-tool/src/drivers/uart/bcm-uart.c`. The
hw/rpi4/BOOT_NOTES.md has historically said "elfloader is silent" --
verify with this hardware whether that's still the case.

### "Boot cpu seen, no Core 1 is up"

Spin-table secondary cores aren't coming up. Most likely DTB
`enable-method` or `cpu-release-addr` mismatch. Fall back:

```
# In settings-rpi4.cmake:
set(KernelMaxNumNodes 1 CACHE STRING "" FORCE)
```

Rebuild build-rpi4, regenerate SD, retest. Once single-core boot is
confirmed working, return to multi-core and instrument
`elfloader-tool/src/arch-arm/drivers/smp-spin-table.c:smp_spin_table_cpu_on`
to log the address being written and the cpu_id.

### "Platform driver crash"

Boot stops in plat probe. Likely candidates:

* EMMC driver -- v0.4.93 had it working; may have bit-rotted.
* GENET (network) -- known broken, MMIO mapping crash; see "Remaining
  Issues" in BOOT_NOTES.md.
* VideoCore (display) -- known broken, VKA assertion.

For network and display, the platform shouldn't fault out -- it
should print a warning and continue. If it doesn't, that's a bug.
Capture the panic and we have a real reproducer.

### "Login fails"

* `auth: login DENIED`: the on-disk `/etc/passwd` may not have the
  hashed root password. The default disk image has it. If you've
  rebuilt the disk and lost it, see `disk/rootfs/etc/passwd`.
* No prompt at all after the banner: getty failed to start. Check the
  last `[exec]` line for an error.

### Capturing for a bug report

```
screen -L -Logfile boot.log /dev/tty.usbserial-XXXX 115200
# (let it run through whatever phase fails)
# Ctrl-A, k, y to exit
```

Attach `boot.log`, the v0.4.x version, and the SHA of the commit
you flashed. The commit SHA is in `[INF] root: ...` of any prior
working boot, but for fresh boots use `git rev-parse HEAD` from the
repo you flashed from.

---

## Fallback ladder

If anything in this checklist fails, work down the ladder until you
find a baseline that boots:

1. v0.4.134 multi-core (this build)
2. v0.4.134 with `KernelMaxNumNodes=1` (single core, full feature set)
3. v0.4.131 (same feature set, before SMP enable)
4. v0.4.93 (last documented working RPi4 boot; HDMI text + interactive
   login)

The git log between any two points tells you what changed. None of
v0.4.121-134 touched the RPi4 boot path directly except v0.4.134's
SMP bump and v0.4.132's mksdcard.py. The fallbacks are cheap to test.
