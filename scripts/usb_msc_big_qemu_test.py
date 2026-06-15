#!/usr/bin/env python3
"""USB Mass Storage >2TB (64-bit LBA) test on QEMU -- v0.4.255 READ_CAPACITY(16)+READ(16).

Attaches a SPARSE ~2.36 TB raw image as a qemu-xhci usb-storage drive. A drive that big
saturates the 32-bit READ_CAPACITY(10) last-LBA at 0xFFFFFFFF, so the driver must fall
back to READ_CAPACITY(16) for the true capacity, and the enumeration last-LBA self-test
must use READ(16) (LBA > 2^32). The image is sparse (truncate), so it costs ~no disk.

PASS = all of:
  - capacity reported > the 2TB saturation value (proves READ_CAPACITY(16) was used)
  - the READ(16) last-LBA self-test reports OK and echoes the planted pattern
  - no MSC failure warnings
"""
import os, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_KERNEL", os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/usb_msc_big_%d.log" % os.getpid()
STICK = "/tmp/usb_msc_big_%d.img" % os.getpid()

SIZE_GIB = 2200                                  # > 2 TiB -> saturates READ_CAPACITY(10)
SIZE_BYTES = SIZE_GIB * (1 << 30)
NSECTORS = SIZE_BYTES // 512                      # 4,613,734,400 > 2^32
LAST_LBA = NSECTORS - 1
LAST_OFF = LAST_LBA * 512
SAT_MB = (0x100000000 * 512) >> 20                # 2097152 MB = the 2TB saturation readout
PAT = b"L16!"                                      # planted at the last LBA
EXPECT_LAST = "OK 4c 31 36 21"                     # 'L16!' bytes echoed by the self-test


def main():
    if not os.path.exists(KERNEL):
        print("FAIL: kernel not found:", KERNEL); return 2
    # sparse 2.36 TB image; plant a pattern at LBA0 and at the LAST LBA (sparse-friendly).
    with open(STICK, "wb") as f:
        f.truncate(SIZE_BYTES)
        f.seek(0); f.write(b"BIG0")
        f.seek(LAST_OFF); f.write(PAT)
    if os.path.exists(LOG):
        os.remove(LOG)

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
                if "last-LBA(16)" in d or "READ(16) high-LBA self-test failed" in d \
                   or "READ_CAPACITY failed" in d:
                    time.sleep(1)
                    break
    finally:
        p.terminate()
        try: p.wait(5)
        except Exception: p.kill()

    log = open(LOG, errors="replace").read() if os.path.exists(LOG) else ""

    # parse the "USB MSC ready: N sectors x 512 bytes = M MB" capacity readout
    cap_mb = None
    for ln in log.splitlines():
        if "USB MSC ready:" in ln and "MB" in ln:
            try:
                cap_mb = int(ln.split("=")[1].strip().split()[0])
            except (IndexError, ValueError):
                pass

    results = []
    def chk(name, ok, detail=""):
        results.append(ok); print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))

    chk("enumerated (bulk endpoints)", "USB MSC: slot=" in log)
    chk("READ_CAPACITY(16) used (cap > 2TB saturation)",
        cap_mb is not None and cap_mb > SAT_MB,
        "cap=%s MB (sat=%d)" % (cap_mb, SAT_MB))
    chk("READ(16) high-LBA self-test OK + pattern",
        ("last-LBA(16)" in log) and (EXPECT_LAST in log))
    chk("no MSC failure warnings",
        "READ(16) high-LBA self-test failed" not in log and "READ_CAPACITY failed" not in log)

    for f in (LOG, STICK):
        try: os.remove(f)
        except OSError: pass

    npass = sum(1 for r in results if r)
    print("\n=== usb msc >2TB suite: %d/%d passed ===" % (npass, len(results)))
    if npass != len(results):
        # surface the relevant serial lines on failure
        for ln in log.splitlines():
            if any(k in ln for k in ("USB MSC", "MSC ", "xhci] USB")):
                print("   |", ln.rstrip())
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
