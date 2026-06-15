#!/usr/bin/env python3
"""USB Mass Storage runtime HOTPLUG test on QEMU -- v0.4.255 Path A.

Unlike usb_msc_mount_qemu_test.py (drive present at BOOT, mounted on the boot thread),
this boots with NO usb-storage device attached, lets the system come all the way up
(driver thread running, g_msc_driver_running=1), then HOT-ADDS the drive over the QEMU
HMP monitor -- the real insert-after-boot path. It checks the Path A additions:
  - the drive enumerates at RUNTIME via the existing root-port hotswap (setup_device ->
    setup_msc), which now flags g_msc_mount_pending because the driver thread is up
  - the driver loop mounts it at /mnt/usb WITHOUT deadlocking (g_msc_mount_inline makes
    the mount block I/O transfer directly instead of posting to its own request queue)
  - ls / cat / a write round-trip work over netconsole (FS-thread -> driver-thread queue)
  - device_del tears the drive down (slot + DMA reclaimed)
  - a replug re-enumerates + re-mounts (ext2_usb refreshed in place, no duplicate mount)

The drive backend (-drive id=stick) is defined at boot but the usb-storage DEVICE is
added at runtime via `device_add` so the mount is purely the runtime path. netconsole
(TCP 2323 via SLIRP hostfwd) drives shell commands; the HMP monitor does the hotplug.

PASS = runtime-enumerate + runtime-mount + ls/cat/write + teardown + replug re-mount.
"""
import os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_KERNEL", os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/usb_msc_hotplug_%d.log" % os.getpid()
STICK = "/tmp/usb_msc_hotplug_stick_%d.img" % os.getpid()
PORT = 47000 + (os.getpid() % 2000)
MON = 49000 + (os.getpid() % 2000)
MARK = "HOTPLUG_USB_%d" % os.getpid()


def logtext():
    return open(LOG, errors="replace").read() if os.path.exists(LOG) else ""


def wait_for(substr, count, timeout):
    """Wait until the serial log contains >= count occurrences of substr."""
    for _ in range(int(timeout * 2)):
        if logtext().count(substr) >= count:
            return True
        time.sleep(0.5)
    return False


def hmp(sock, line):
    sock.send((line + "\n").encode())
    time.sleep(0.5)
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
    with open(DISK, "rb") as a, open(STICK, "wb") as b:
        b.write(a.read())
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
            # drive backend only -- the usb-storage DEVICE is hot-added at runtime
            "-drive", "file=%s,format=raw,if=none,id=stick" % STICK,
            "-kernel", KERNEL]

    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    boot = ready = False
    enum1 = mount1 = ls1 = cat1 = wr = torn = enum2 = mount2 = ls2 = ""
    pre_mount_clean = False
    try:
        # 1. boot all the way to a usable netconsole shell (no drive yet)
        for _ in range(60):
            time.sleep(2)
            try:
                if "RDY" in nc("echo RDY"): ready = True; break
            except Exception: pass
        boot = ready
        if not ready:
            raise SystemExit
        # the drive must NOT be mounted before we hot-add it (runtime-only path)
        pre_mount_clean = "mounted at /mnt/usb" not in logtext()

        s = socket.create_connection(("127.0.0.1", MON), timeout=5)
        time.sleep(0.5); s.recv(8192)

        # 2. HOT-ADD the drive (insert-after-boot)
        print("[hotplug] device_add usb-storage (runtime insert)", flush=True)
        hmp(s, "device_add usb-storage,bus=xhci.0,drive=stick,id=msc0")
        enum1 = "yes" if wait_for("USB MSC ready", 1, 15) else ""
        mount1 = "yes" if wait_for("runtime mount: mounted at /mnt/usb", 1, 15) else ""

        if mount1:
            ls1 = nc("ls /mnt/usb")
            cat1 = nc("cat /mnt/usb/etc/passwd")
            nc("echo %s > /mnt/usb/hotplug.txt" % MARK)
            wr = nc("cat /mnt/usb/hotplug.txt")

        # 3. UNPLUG -> teardown
        print("[hotplug] device_del msc0 (unplug)", flush=True)
        hmp(s, "device_del msc0")
        torn = "yes" if wait_for("torn down", 1, 10) else ""

        # 4. REPLUG -> re-enumerate + re-mount. NOTE: QEMU auto-deletes a -drive backend
        # when its device is device_del'd, so re-present a fresh drive (drive_add) + device.
        # On real HW a physical replug is just a new connect; this is purely a QEMU quirk.
        print("[hotplug] drive_add + device_add usb-storage (replug)", flush=True)
        hmp(s, "drive_add 0 if=none,id=stick2,file=%s,format=raw" % STICK)
        hmp(s, "device_add usb-storage,bus=xhci.0,drive=stick2,id=msc1")
        enum2 = "yes" if wait_for("USB MSC ready", 2, 15) else ""
        mount2 = "yes" if wait_for("runtime mount: mounted at /mnt/usb", 2, 15) else ""
        if mount2:
            ls2 = nc("ls /mnt/usb")
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
    chk("boot reached netconsole (no drive)", boot)
    chk("not mounted before hot-add (runtime-only)", pre_mount_clean)
    chk("hot-add: drive enumerated at runtime", enum1 == "yes")
    chk("hot-add: runtime mount at /mnt/usb (no deadlock)", mount1 == "yes")
    chk("ls /mnt/usb shows entries", "bin" in ls1 and "etc" in ls1)
    chk("cat /mnt/usb/etc/passwd", "root:" in cat1)
    chk("write through mount round-trips", MARK in wr)
    chk("write persisted to USB image (offline)", MARK.encode() in img)
    chk("device_del -> torn down", torn == "yes")
    chk("replug -> re-enumerated", enum2 == "yes")
    chk("replug -> re-mounted", mount2 == "yes")
    chk("replug: ls /mnt/usb still works", "bin" in ls2 and "etc" in ls2)

    for f in (LOG, STICK):
        try: os.remove(f)
        except OSError: pass
    npass = sum(1 for r in results if r)
    print("\n=== usb msc hotplug suite: %d/%d passed ===" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
