#!/usr/bin/env python3
"""Typematic runaway guard test on QEMU (v0.4.197).

Reproduces the HW failure mode: a keyboard that dies BETWEEN a press and its
release never sends the release, so typematic would repeat the key forever
(the "rrrr" storm that saturated core 0 on real HW). Simulation: hold a key
via HMP (sendkey z 8000), then device_del the keyboard ~1.5s into the hold --
the release never arrives, but the hot-remove posts a PORT_STATUS_CHANGE
event, which must DISARM typematic (evt_dispatch hook).

PASS = repeats happened while held (typematic alive), then STOPPED after the
device removal (no unbounded growth).
"""
import os, re, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/xhci_runaway_qemu.log"
MON = 55563

if os.path.exists(LOG):
    os.remove(LOG)
cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
       "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
       "-display", "none", "-no-reboot", "-serial", "file:" + LOG,
       "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON,
       "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
       "-device", "virtio-blk-device,drive=hd0",
       "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,id=kbd0",
       "-kernel", KERNEL]
if os.path.exists(LOGDISK):
    cmd += ["-drive", "file=%s,format=raw,if=none,id=hd1" % LOGDISK,
            "-device", "virtio-blk-device,drive=hd1"]
p = subprocess.Popen(cmd)

def log():
    return open(LOG, errors="replace").read() if os.path.exists(LOG) else ""

ready = False
for _ in range(40):
    time.sleep(1)
    if "AIOS login" in log() and "HID keyboard ready" in log():
        ready = True
        break

had_repeats = False
stopped = False
disarmed = False
if ready:
    try:
        s = socket.create_connection(("127.0.0.1", MON), timeout=5)
        time.sleep(0.5)
        s.recv(8192)
        s.send(b"sendkey z 8000\n")    # hold z; release would come at t+8s
        time.sleep(1.5)                # 0.5s delay + ~1s of repeats
        n_before_del = log().count("z")
        s.send(b"device_del kbd0\n")   # keyboard dies mid-hold: release LOST
        time.sleep(2.0)                # port-change must disarm typematic
        n_after_del = log().count("z")
        time.sleep(2.0)                # any further growth = runaway
        n_final = log().count("z")
        s.close()
        had_repeats = n_before_del >= 4          # press + >=3 repeats while held
        stopped = (n_final - n_after_del) <= 1   # at most one in-flight emit
        disarmed = "typematic disarmed" in log()
        print("counts: held=%d after_del=%d final=%d disarmed_log=%s"
              % (n_before_del, n_after_del, n_final, disarmed))
    except Exception as ex:
        print("monitor error:", ex)

p.terminate()
try:
    p.wait(5)
except Exception:
    p.kill()

ok = ready and had_repeats and stopped
print("ready=%s had_repeats=%s stopped_after_removal=%s" % (ready, had_repeats, stopped))
print("=== %s ===" % ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
