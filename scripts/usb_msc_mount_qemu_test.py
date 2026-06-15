#!/usr/bin/env python3
"""USB Mass Storage MOUNT test on QEMU -- v0.4.255 Stage 4.

Attaches an EXT2 image (a copy of disk_ext2.img) as a USB drive via qemu-xhci +
usb-storage, and checks the full stack:
  - the drive enumerates + READ_CAPACITY (Stages 1-2)
  - boot mounts it at /mnt/usb (ext2_init reads the superblock via DIRECT block I/O
    on the boot thread -- the mount path)
  - at RUNTIME, `ls /mnt/usb` + `cat /mnt/usb/etc/passwd` read directory/file blocks
    through the FS-thread -> xHCI-driver-thread request queue (the concurrency path)
  - a write through the mount (echo > /mnt/usb/...) persists to the image

Drives commands over the auto-started netconsole (TCP 2323 via a SLIRP hostfwd).
PASS = mounted + ls shows entries + a known file reads + a fresh write round-trips
and is found in the on-disk image.
"""
import os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_KERNEL", os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/usb_msc_mount_%d.log" % os.getpid()
STICK = "/tmp/usb_msc_mount_stick_%d.img" % os.getpid()
PORT = 45000 + (os.getpid() % 2000)
MARK = "HELLO_USB_%d" % os.getpid()


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
    # a valid ext2 image to mount as the USB drive (copy of the system disk)
    with open(DISK, "rb") as a, open(STICK, "wb") as b:
        b.write(a.read())
    for p in (LOG,):
        if os.path.exists(p): os.remove(p)

    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-no-reboot", "-serial", "file:" + LOG,
           "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
           "-device", "virtio-blk-device,drive=hd0"]
    if os.path.exists(LOGDISK):
        cmd += ["-drive", "file=%s,format=raw,if=none,id=hd1" % LOGDISK,
                "-device", "virtio-blk-device,drive=hd1"]
    cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:2323" % PORT,
            "-device", "virtio-net-device,netdev=n0",
            "-device", "qemu-xhci,id=xhci",
            "-drive", "file=%s,format=raw,if=none,id=stick" % STICK,
            "-device", "usb-storage,bus=xhci.0,drive=stick",
            "-kernel", KERNEL]

    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ls = cat = wr = ""
    mounted = None
    try:
        for _ in range(70):
            time.sleep(1)
            if os.path.exists(LOG):
                d = open(LOG, errors="replace").read()
                if "mounted at /mnt/usb" in d: mounted = True; break
                if "not ext2" in d: mounted = False; break
        if mounted:
            for _ in range(40):
                time.sleep(2)
                try:
                    if "RDY" in nc("echo RDY"): break
                except Exception: pass
            ls = nc("ls /mnt/usb")
            cat = nc("cat /mnt/usb/etc/passwd")
            nc("echo %s > /mnt/usb/usbmnttest.txt" % MARK)
            wr = nc("cat /mnt/usb/usbmnttest.txt")
    finally:
        p.terminate()
        try: p.wait(5)
        except Exception: p.kill()

    img = open(STICK, "rb").read() if os.path.exists(STICK) else b""
    results = []
    def chk(n, ok, det=""):
        results.append(ok); print("  [%s] %s %s" % ("PASS" if ok else "FAIL", n, det))
    chk("drive enumerated + capacity", os.path.exists(LOG) and "USB MSC ready" in open(LOG, errors="replace").read())
    chk("mounted at /mnt/usb", mounted is True)
    chk("ls /mnt/usb shows entries (runtime read)", "bin" in ls and "etc" in ls)
    chk("cat /mnt/usb/etc/passwd (runtime read)", "root:" in cat)
    chk("write through mount round-trips", MARK in wr)
    chk("write persisted to USB image (offline)", MARK.encode() in img)

    for f in (LOG, STICK):
        try: os.remove(f)
        except OSError: pass
    npass = sum(1 for r in results if r)
    print("\n=== usb msc mount suite: %d/%d passed ===" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
