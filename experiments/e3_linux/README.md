# E3 -- does the broadcast TLBI DVM-Sync hang after idle under Linux?

Linux on this Pi is **immune** to the AIOS ~32.4s freeze, yet it's a **full runtime**. Bare-metal E1
(minimal runtime) never reproduced the freeze even doing the exact broadcast DVM-Sync after 240s idle ->
hypothesis: the BCM2711 SCB only quiesces under a full VideoCore-firmware runtime. This module forces all
cores quiet (stop_machine, CNTVCT-only spin) for `idle_s`, then CPU 0 times `tlbi vmalle1is; dsb sy` (the
EL1 IS-broadcast DVM-Sync = Linux's analog of AIOS's `tlbi vae1is`). Same test as E1, but under Linux.

- **Hangs ~32.4s** -> the SCB DOES quiesce under a full runtime + the TLBI triggers it. A reference
  reproducer; Linux's immunity is just that it never naturally does this when quiesced.
- **Stays ~0ms** -> Linux keeps the SCB warm even under forced idle -> *that's* its immunity; the cure is
  to keep the SCB warm the way Linux does (re-opens the keep-warm idea with the right target).

## Restore Raspberry Pi OS on the card first
On the Mac (card mounted), undo the E1 boot swap:
```
cp /Volumes/bootfs/config.txt.rpios  /Volumes/bootfs/config.txt
cp /Volumes/bootfs/kernel8.img.rpios /Volumes/bootfs/kernel8.img
sync && diskutil eject /dev/disk16
```
Boot the Pi into Raspberry Pi OS (serial console is already on: cmdline has `console=serial0,115200`, so
`[E3]` lines also appear on sercap).

## Build + run (on the Pi)
```
sudo apt update && sudo apt install -y raspberrypi-kernel-headers
# copy this directory to the Pi (scp, or git), then:
make
echo 0 | sudo tee /proc/sys/kernel/watchdog     # IMPORTANT: a repro hangs all cores ~32.4s
sudo dmesg -C
sudo insmod e3_dvm_test.ko idle_s=20
dmesg | grep '\[E3\]'
sudo rmmod e3_dvm_test
```
Sweep the idle duration by reloading: `sudo rmmod e3_dvm_test; sudo insmod e3_dvm_test.ko idle_s=60` etc.
Try, e.g., 5, 20, 60, 120.

## Notes / risks
- Holds ALL cores with IRQs off for `idle_s` (and +~32.4s if it reproduces). The lockup detector MUST be
  off, else it may panic/reboot. If the Pi wedges (permanent, not self-recovering), power-cycle -- it's the
  spare card, RPi OS, nothing precious.
- `tlbi vmalle1is` flushes the whole EL1&0 TLB (Linux refills automatically -- a perf blip, not a crash).
- To make the SCB most likely to quiesce, run from a quiet system: boot to multi-user (no desktop), and
  optionally stop networking just before loading (`sudo systemctl stop NetworkManager` / unplug ethernet).
