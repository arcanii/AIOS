#!/usr/bin/env python3
"""USB HID keyboard test on QEMU (USB HID arc, docs/DESIGN_USB_HID.md).

Boots build-04 with -device qemu-xhci -device usb-kbd, waits for the xHCI driver
to report the keyboard ready, then injects keystrokes via the QEMU HMP monitor
(sendkey) and checks the serial log for the decoded "[xhci-kbd] key=" lines.

PASS = the controller enumerated the keyboard AND every injected key was decoded.
"""
import os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/xhci_key_qemu.log"
MON = 55557
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
print("ready=%s decoded=%r" % (ready, decoded))
print("=== %s ===" % ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
