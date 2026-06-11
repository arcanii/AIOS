#!/usr/bin/env python3
"""USB HID keyboard typematic (host-side key repeat) test on QEMU (v0.4.192).

Boots build-04 with -device qemu-xhci -device usb-kbd, waits for the login
prompt, then HOLDS a key for 1.5s via the HMP monitor (sendkey z 1500). The
driver arms repeat after 500ms and re-emits at ~15cps, so the held key must
echo as a run of the same char at the prompt. After release, the run must stop
growing (disarm on the release report).

PASS = a run of >= 5 echoed repeats AND no further growth after release.
"""
import os, socket, subprocess, sys, time, re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/xhci_typematic_qemu.log"
MON = 55561

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

def log():
    return open(LOG, errors="replace").read() if os.path.exists(LOG) else ""

ready = False
for _ in range(40):
    time.sleep(1)
    if "AIOS login" in log() and "HID keyboard ready" in log():
        ready = True
        break

longest = 0
stable = False
if ready:
    try:
        s = socket.create_connection(("127.0.0.1", MON), timeout=5)
        time.sleep(0.5)
        s.recv(8192)
        s.send(b"sendkey z 1500\n")   # hold z for 1.5s -> ~1 press + ~15 repeats
        time.sleep(2.5)               # hold + a margin past release
        n1 = log().count("z")
        time.sleep(1.2)               # repeats must have STOPPED after release
        n2 = log().count("z")
        stable = (n2 == n1)
        s.close()
    except Exception as ex:
        print("monitor error:", ex)
    runs = re.findall(r"z+", log())
    longest = max((len(r) for r in runs), default=0)

p.terminate()
try:
    p.wait(5)
except Exception:
    p.kill()

ok = ready and longest >= 5 and stable
print("ready=%s longest_run=%d stopped_after_release=%s" % (ready, longest, stable))
print("=== %s ===" % ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
