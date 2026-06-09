#!/usr/bin/env python3
"""USB Ctrl-key decoding test on QEMU (USB HID arc, docs/DESIGN_USB_HID.md).

Regression for "Left Ctrl + C did not generate Ctrl-C on real HW": process_kbd_report
only applied the Shift modifier, so Ctrl+letter sent the plain letter instead of the
terminal control code. The fix folds Ctrl+letter (and @[\]^_?) to a control byte.

Boots build-04 with a USB keyboard and injects Ctrl combos via the HMP monitor
(sendkey ctrl-c / ctrl-d / ctrl-a), plus a plain key, then checks the driver's decode
log. The driver prints the resulting byte as "(0xNN)", so Ctrl-C must decode to 0x03.

PASS = ctrl-c->0x03, ctrl-d->0x04, ctrl-a->0x01, and a plain 'x' still ->0x78.
"""
import os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/xhci_ctrl_qemu.log"
MON = 55571
# (HMP sendkey combo, expected decoded byte)
CASES = [("ctrl-c", 0x03), ("ctrl-d", 0x04), ("ctrl-a", 0x01), ("x", 0x78)]

if os.path.exists(LOG):
    os.remove(LOG)
cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
       "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
       "-display", "none", "-no-reboot", "-serial", "file:" + LOG,
       "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON,
       "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
       "-device", "virtio-blk-device,drive=hd0",
       "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd",
       "-kernel", KERNEL]
if os.path.exists(LOGDISK):
    cmd += ["-drive", "file=%s,format=raw,if=none,id=hd1" % LOGDISK,
            "-device", "virtio-blk-device,drive=hd1"]
p = subprocess.Popen(cmd)

ready = False
for _ in range(35):
    time.sleep(1)
    if os.path.exists(LOG) and "HID keyboard ready" in open(LOG, errors="replace").read():
        ready = True
        break

if ready:
    try:
        s = socket.create_connection(("127.0.0.1", MON), timeout=5)
        time.sleep(0.5); s.recv(8192)
        for combo, _ in CASES:
            s.send(("sendkey %s\n" % combo).encode())
            time.sleep(0.4)
        time.sleep(1.0)
        s.close()
    except Exception as ex:
        print("monitor error:", ex)

time.sleep(1)
p.terminate()
try:
    p.wait(5)
except Exception:
    p.kill()

data = open(LOG, errors="replace").read()
bytes_seen = [ln for ln in data.splitlines() if "[xhci-kbd] key=" in ln]
ok = ready
for combo, want in CASES:
    hit = any(("(0x%02x)" % want) in ln for ln in bytes_seen)
    print("  [%s] %-7s -> 0x%02x" % ("PASS" if hit else "FAIL", combo, want))
    ok = ok and hit
for ln in bytes_seen:
    print("    " + ln.strip())
print("ready=%s" % ready)
print("=== %s ===" % ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
