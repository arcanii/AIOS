#!/usr/bin/env python3
"""USB HID keyboard BEHIND A HUB on QEMU (USB HID arc, docs/DESIGN_USB_HID.md).

Mirrors the real RPi4 topology: on a Pi 4 every USB-A port hangs off the VL805's
internal USB 2.0 hub, so the keyboard is never on a root port -- the xHCI driver
must enumerate the hub (class 9) and then the device behind it (route string +
parent-hub TT). QEMU models this with -device usb-hub + a usb-kbd behind it.

Boots build-04 with the keyboard behind a hub, waits for the driver to report the
keyboard ready, injects keystrokes via the HMP monitor, and checks the serial log
for the decoded "[xhci-kbd] key=" lines.

PASS = the controller enumerated the hub + the keyboard behind it AND every
injected key was decoded.
"""
import os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/xhci_hub_key_qemu.log"
MON = 55561
KEYS = ["h", "e", "l", "l", "o", "spc", "a", "i", "o", "s", "ret"]
EXPECT = "hello aios"   # the printable chars we expect decoded, in order

if os.path.exists(LOG):
    os.remove(LOG)
cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
       "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
       "-display", "none", "-no-reboot", "-serial", "file:" + LOG,
       "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON,
       "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
       "-device", "virtio-blk-device,drive=hd0",
       "-device", "qemu-xhci,id=xhci",
       "-device", "usb-hub,bus=xhci.0,port=1",            # hub on xHCI root port 1
       "-device", "usb-kbd,bus=xhci.0,port=1.1",          # keyboard on hub port 1
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
        time.sleep(0.5)
        s.recv(8192)
        for k in KEYS:
            s.send(("sendkey %s\n" % k).encode())
            time.sleep(0.35)
        time.sleep(1.5)
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
decoded = "".join(ln.split("'")[1] for ln in data.splitlines()
                  if "[xhci-kbd] key=" in ln and "'" in ln)
ok = ready and EXPECT in decoded.replace("?", "")  # '?' = non-printable (Enter)
# surface the hub-path lines for debugging
for ln in data.splitlines():
    if any(t in ln for t in ("hub", "[xhci] device:", "no HID")):
        print("  " + ln.strip())
print("ready=%s decoded=%r" % (ready, decoded))
print("=== %s ===" % ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
