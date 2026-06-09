#!/usr/bin/env python3
"""USB lock-LED regression test on QEMU (USB HID arc, docs/DESIGN_USB_HID.md).

Reproduces the exact scenario that made the physical lock LED get deferred in
v0.4.185: on a real low-speed keyboard behind the VL805 TT, sending the SET_REPORT
output report at RUNTIME from the polling driver thread (on a lock-key press) stopped
all input. The fix is the endpoint-aware control_transfer (the event dispatcher keeps
the interrupt-IN stream alive across a control transfer) plus the EP0 ring Link-TRB.

This boots the keyboard BEHIND A HUB (mirrors the Pi topology), then types with
Num Lock and Caps Lock presses interspersed. Each lock press triggers a SET_REPORT
from the driver thread -- the regression was that typing died after the FIRST one.
QEMU's usb-kbd accepts SET_REPORT (no visible LED), so this verifies the control
path does not disrupt the keyboard. The real LED toggling is HW-only (see
/proc/xhci.led on a Pi).

PASS = the keyboard enumerated, every printable char typed AFTER the lock presses was
decoded, and no SET_REPORT error ([xhci-led]) line appeared.
"""
import os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/xhci_led_qemu.log"
MON = 55563
# num_lock / caps_lock presses are interspersed: each fires a SET_REPORT from the
# driver thread. Letters are immune to num_lock, and we pair the caps_lock presses so
# the letters between them are the only uppercased ones -- keeps EXPECT predictable.
KEYS = ["h", "i", "num_lock", "a", "i", "o", "s", "num_lock", "ret",
        "caps_lock", "caps_lock", "spc", "o", "k", "ret"]
EXPECT = "hiaios"   # printable chars; survives both num_lock toggles
EXPECT2 = "ok"      # typed after the caps_lock pair (more SET_REPORTs)

if os.path.exists(LOG):
    os.remove(LOG)
cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
       "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
       "-display", "none", "-no-reboot", "-serial", "file:" + LOG,
       "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON,
       "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
       "-device", "virtio-blk-device,drive=hd0",
       "-device", "qemu-xhci,id=xhci",
       "-device", "usb-hub,bus=xhci.0,port=1",
       "-device", "usb-kbd,bus=xhci.0,port=1.1",
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
led_errs = [ln.strip() for ln in data.splitlines() if "[xhci-led]" in ln]
clean = decoded.replace("?", "")
ok = (ready and EXPECT in clean and EXPECT2 in clean and not led_errs)
for ln in led_errs:
    print("  LED ERROR: " + ln)
print("ready=%s decoded=%r led_errs=%d" % (ready, decoded, len(led_errs)))
print("=== %s ===" % ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
