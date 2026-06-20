#!/usr/bin/env python3
"""USB Mass Storage (BOT/SCSI over xHCI) test on QEMU -- v0.4.255 Stages 1-2.

Boots build-04 with -device qemu-xhci + -device usb-storage backed by a raw disk
image carrying a known LBA0 pattern, and checks the xHCI driver's enumeration +
SCSI self-tests in the serial log:

  Stage 1: enumerate the bulk endpoints, INQUIRY, READ_CAPACITY (capacity matches)
  Stage 2: READ(10) of LBA0 returns the planted pattern; WRITE(10) self-test PASSes
           (QEMU-disk-gated, destructive on LBA1) AND the write persists in the image.

PASS = all of: capacity == 131072 sectors, LBA0 == 'AIOSMSC!', WRITE self-test PASS,
and the on-disk image at offset 512 holds the 0xA5^i pattern AIOS wrote.
"""
import os, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_KERNEL", os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/usb_msc_qemu_%d.log" % os.getpid()
STICK = "/tmp/usb_msc_stick_%d.img" % os.getpid()
PATTERN0 = b"AIOSMSC!"               # planted at LBA0 -> READ(10) must echo it
EXPECT_LBA0 = "41 49 4f 53 4d 53 43 21"
EXPECT_SECTORS = 131072              # 64 MB / 512


def main():
    if not os.path.exists(KERNEL):
        print("FAIL: kernel not found:", KERNEL); return 2
    # 64 MB raw "USB stick" with a known LBA0 pattern.
    with open(STICK, "wb") as f:
        f.truncate(64 * 1024 * 1024)
        f.seek(0); f.write(PATTERN0)
    for p in (LOG,):
        if os.path.exists(p):
            os.remove(p)

    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-no-reboot", "-serial", "file:" + LOG,
           "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
           "-device", "virtio-blk-device,drive=hd0"]
    if os.path.exists(LOGDISK):
        cmd += ["-drive", "file=%s,format=raw,if=none,id=hd1" % LOGDISK,
                "-device", "virtio-blk-device,drive=hd1"]
    cmd += ["-device", "qemu-xhci,id=xhci",
            "-drive", "file=%s,format=raw,if=none,id=stick" % STICK,
            "-device", "usb-storage,bus=xhci.0,drive=stick",
            "-kernel", KERNEL]

    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(70):
            time.sleep(1)
            if os.path.exists(LOG):
                d = open(LOG, errors="replace").read()
                if "WRITE(10) self-test" in d or "READ_CAPACITY failed" in d:
                    time.sleep(1)
                    break
    finally:
        p.terminate()
        try: p.wait(5)
        except Exception: p.kill()

    log = open(LOG, errors="replace").read() if os.path.exists(LOG) else ""
    # offline: did AIOS's WRITE(10) self-test land the 0xA5^i pattern at LBA1?
    img = open(STICK, "rb").read(1024) if os.path.exists(STICK) else b""
    lba1_ok = len(img) >= 514 and img[512] == 0xA5 and img[513] == 0xA4

    results = []
    def chk(name, ok, detail=""):
        results.append(ok); print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))

    chk("enumerated (bulk endpoints)", "USB MSC: slot=" in log)
    chk("READ_CAPACITY (%d sectors)" % EXPECT_SECTORS, "%d sectors" % EXPECT_SECTORS in log)
    chk("READ(10) LBA0 == 'AIOSMSC!'", ("USB MSC LBA0: " + EXPECT_LBA0) in log)
    chk("WRITE(10) self-test PASS", "WRITE(10) self-test (LBA1, QEMU disk): PASS" in log)
    chk("multi-sector READ(8) self-test PASS (Stage 6)", "multi-sector READ(8) self-test: PASS" in log)
    chk("WRITE persisted to image (LBA1)", lba1_ok)

    for f in (LOG, STICK):
        try: os.remove(f)
        except OSError: pass

    npass = sum(1 for r in results if r)
    print("\n=== usb msc suite: %d/%d passed ===" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
