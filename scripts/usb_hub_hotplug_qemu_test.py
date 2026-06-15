#!/usr/bin/env python3
"""USB HUB-DOWNSTREAM hotplug test on QEMU -- v0.4.256 Path B.

The RPi4 keyboard (and a USB-2 drive) hang off the VL805 hub, whose DOWNSTREAM port changes
are NOT root Port Status Change Events -- the hub reports them on its own interrupt-IN status
pipe. This test boots with a usb-hub (+ a keyboard behind it, mirroring the Pi), then HOT-ADDS
a usb-storage device BEHIND THE HUB and checks the full Path B path:
  - setup_hub arms the hub interrupt-IN status pipe at enumeration
  - a runtime device_add behind the hub fires a status-change transfer -> handle_hub_changes
    parses the port bitmap, enumerates the new downstream device (hub_enumerate_port -> setup_msc)
  - the MSC drive mounts at /mnt/usb (g_msc_mount_pending -> msc_runtime_mount)
  - ls / cat / write work; device_del tears it down; a replug re-enumerates + re-mounts

QEMU models the hub status pipe (verified), so this exercises Path B end-to-end. The real VL805
timing is still HW-pending. PASS = arm + downstream-enumerate + mount + ls/cat/write + teardown.
"""
import os, socket, subprocess, sys, time, shutil

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_KERNEL", os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/usb_hub_hotplug_%d.log" % os.getpid()
STICK = "/tmp/usb_hub_stick_%d.img" % os.getpid()
PORT = 47000 + ((os.getpid() + 707) % 2000)
MON = 49000 + ((os.getpid() + 707) % 2000)
MARK = "HUBPLUG_%d" % os.getpid()


def logtext():
    return open(LOG, errors="replace").read() if os.path.exists(LOG) else ""


def wait_for(substr, count, timeout):
    for _ in range(int(timeout * 2)):
        if logtext().count(substr) >= count:
            return True
        time.sleep(0.5)
    return False


def hmp(sock, line):
    sock.send((line + "\n").encode()); time.sleep(0.5)
    try: return sock.recv(8192).decode(errors="replace")
    except Exception: return ""


def nc(cmd, wait=3.0):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=12); s.settimeout(6); time.sleep(0.8)
    try: s.recv(4096)
    except socket.timeout: pass
    s.sendall((cmd + "\n").encode()); time.sleep(wait)
    out = b""
    try:
        while True:
            x = s.recv(4096)
            if not x: break
            out += x
            if out.rstrip().endswith(b"aios#"): break
    except socket.timeout: pass
    s.close()
    return out.decode("utf-8", "replace").replace("aios# ", "")


def main():
    if not os.path.exists(KERNEL) or not os.path.exists(DISK):
        print("FAIL: kernel/disk missing"); return 2
    shutil.copy(DISK, STICK)
    if os.path.exists(LOG): os.remove(LOG)

    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-no-reboot", "-serial", "file:" + LOG,
           "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON,
           "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
           "-device", "virtio-blk-device,drive=hd0"]
    if os.path.exists(LOGDISK):
        cmd += ["-drive", "file=%s,format=raw,if=none,id=hd1" % LOGDISK,
                "-device", "virtio-blk-device,drive=hd1"]
    cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:2323" % PORT,
            "-device", "virtio-net-device,netdev=n0",
            "-device", "qemu-xhci,id=xhci",
            "-device", "usb-hub,bus=xhci.0,port=1,id=hub0",          # the hub (like the VL805)
            "-device", "usb-kbd,bus=xhci.0,port=1.1,id=kbd0",        # a kbd behind it (Pi topology)
            "-drive", "file=%s,format=raw,if=none,id=stick" % STICK, # drive backend, device hot-added
            "-kernel", KERNEL]

    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    boot = ready = False
    armed = enum1 = mount1 = ls1 = cat1 = wr = torn = enum2 = mount2 = ""
    try:
        for _ in range(60):
            time.sleep(2)
            try:
                if "RDY" in nc("echo RDY"): ready = True; break
            except Exception: pass
        boot = ready
        if not ready:
            raise SystemExit
        # Path B ships OFF (inert) so the keyboard-on-the-hub path is undisturbed on real HW.
        # Enable it live (as a HW serial-capture session would): the driver thread then arms
        # every enumerated hub's status pipe.
        nc("cat /proc/xhci.hub.1")
        time.sleep(1.5)
        armed = "yes" if "hub status pipe armed" in logtext() else ""

        s = socket.create_connection(("127.0.0.1", MON), timeout=5); time.sleep(0.5); s.recv(8192)

        # hot-add a USB drive BEHIND the hub (downstream port 2)
        print("[hub] device_add usb-storage behind the hub (port 1.2)", flush=True)
        hmp(s, "device_add usb-storage,bus=xhci.0,port=1.2,drive=stick,id=msc0")
        enum1 = "yes" if wait_for("USB MSC ready", 1, 18) else ""
        mount1 = "yes" if wait_for("runtime mount: mounted at /mnt/usb", 1, 18) else ""
        if mount1:
            ls1 = nc("ls /mnt/usb")
            cat1 = nc("cat /mnt/usb/etc/passwd")
            nc("echo %s > /mnt/usb/hub.txt" % MARK)
            wr = nc("cat /mnt/usb/hub.txt")

        # unplug behind the hub -> teardown
        print("[hub] device_del msc0 (unplug behind hub)", flush=True)
        hmp(s, "device_del msc0")
        torn = "yes" if wait_for("torn down", 1, 12) else ""

        # replug behind the hub -> re-enumerate + re-mount
        print("[hub] drive_add + device_add (replug behind hub)", flush=True)
        hmp(s, "drive_add 0 if=none,id=stick2,file=%s,format=raw" % STICK)
        hmp(s, "device_add usb-storage,bus=xhci.0,port=1.2,drive=stick2,id=msc1")
        enum2 = "yes" if wait_for("USB MSC ready", 2, 18) else ""
        mount2 = "yes" if wait_for("runtime mount: mounted at /mnt/usb", 2, 18) else ""
        s.close()
    except Exception as ex:
        print("error:", ex)
    finally:
        time.sleep(1)
        p.terminate()
        try: p.wait(5)
        except Exception: p.kill()

    img = open(STICK, "rb").read() if os.path.exists(STICK) else b""
    results = []
    def chk(n, ok, det=""):
        results.append(ok); print("  [%s] %s %s" % ("PASS" if ok else "FAIL", n, det))
    chk("boot (hub + kbd behind it)", boot)
    chk("hub interrupt-IN status pipe armed", armed == "yes")
    chk("downstream device_add enumerated (behind hub)", enum1 == "yes")
    chk("MSC mounted at /mnt/usb (behind hub)", mount1 == "yes")
    chk("ls /mnt/usb shows entries", "bin" in ls1 and "etc" in ls1)
    chk("cat /mnt/usb/etc/passwd", "root:" in cat1)
    chk("write through mount round-trips", MARK in wr)
    chk("write persisted to image (offline)", MARK.encode() in img)
    chk("device_del behind hub -> torn down", torn == "yes")
    chk("replug behind hub -> re-enumerated", enum2 == "yes")
    chk("replug behind hub -> re-mounted", mount2 == "yes")

    for f in (LOG, STICK):
        try: os.remove(f)
        except OSError: pass
    npass = sum(1 for r in results if r)
    print("\n=== usb hub hotplug suite: %d/%d passed ===" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
