#!/usr/bin/env python3
"""USB multi-device test on QEMU (USB HID arc, docs/DESIGN_USB_HID.md).

Boots build-04 with BOTH a keyboard and a mouse behind a hub (mirrors the RPi4
topology: every Pi USB-A port funnels through the VL805 internal hub). Verifies the
multi-device driver (struct usb_dev array) enumerates BOTH devices and routes each
one's interrupt-IN reports to the right decoder, by injecting keystrokes (HMP sendkey)
and mouse motion/clicks (HMP mouse_move / mouse_button) and checking the serial log.

PASS = the hub enumerated, BOTH a keyboard and a mouse came up, the keyboard decoded
the typed text, and the mouse decoded motion + a click.
"""
import os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/xhci_multidev_qemu.log"
MON = 55565

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
       "-device", "usb-mouse,bus=xhci.0,port=1.2",
       "-kernel", KERNEL]
if os.path.exists(LOGDISK):
    cmd += ["-drive", "file=%s,format=raw,if=none,id=hd1" % LOGDISK,
            "-device", "virtio-blk-device,drive=hd1"]
p = subprocess.Popen(cmd)

# Wait until BOTH HID devices report ready (the driver prints "HID <kind> ready").
both = False
for _ in range(40):
    time.sleep(1)
    if os.path.exists(LOG):
        d = open(LOG, errors="replace").read()
        if "HID keyboard ready" in d and "HID mouse ready" in d:
            both = True
            break

if both:
    try:
        s = socket.create_connection(("127.0.0.1", MON), timeout=5)
        time.sleep(0.5)
        s.recv(8192)
        for k in ["h", "i", "spc", "a", "i", "o", "s"]:    # keyboard
            s.send(("sendkey %s\n" % k).encode())
            time.sleep(0.3)
        for cmdline in ["mouse_move 20 10", "mouse_move -5 7",  # mouse motion
                        "mouse_button 1", "mouse_button 0"]:    # left click + release
            s.send((cmdline + "\n").encode())
            time.sleep(0.3)
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
kbd_ready = "HID keyboard ready" in data
mouse_ready = "HID mouse ready" in data
kbd_decoded = "".join(ln.split("'")[1] for ln in data.splitlines()
                      if "[xhci-kbd] key=" in ln and "'" in ln)
mouse_lines = [ln.strip() for ln in data.splitlines() if "[xhci-mouse]" in ln]
# A click report has btn != 0 in at least one mouse line.
mouse_click = any("btn=0x1" in ln for ln in mouse_lines)
kbd_ok = kbd_ready and "hi aios" in kbd_decoded.replace("?", "")
mouse_ok = mouse_ready and len(mouse_lines) > 0 and mouse_click

for ln in data.splitlines():
    if any(t in ln for t in ("HID keyboard ready", "HID mouse ready", "hub:")):
        print("  " + ln.strip())
print("  mouse reports: %d, e.g. %r" % (len(mouse_lines), mouse_lines[:3]))
print("kbd_ready=%s mouse_ready=%s kbd_decoded=%r mouse_click=%s"
      % (kbd_ready, mouse_ready, kbd_decoded, mouse_click))
ok = kbd_ok and mouse_ok
print("=== %s ===" % ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
