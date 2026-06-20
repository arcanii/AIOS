#!/usr/bin/env python3
"""USB Mass Storage bulk-STALL RECOVERY test on QEMU -- v0.4.257.

Exercises the BOT/SCSI bulk-endpoint STALL recovery added for item 1 of
docs/NEXT_20260616_usb_hotplug_hw.md. On real HW a SuperSpeed drive's FIRST replug
STALLs (cc=6) the READ_CAPACITY data phase; before the fix the endpoint stayed Halted
so the CSW + the next command's CBW read cc=-1 ("READ_CAPACITY failed") and the drive
only enumerated on a second slot. The fix clears the halt on both the xHC (Reset
Endpoint + Set TR Dequeue) AND the device (ClearFeature ENDPOINT_HALT), then reads the
CSW -- so a single STALL no longer wedges the pipe.

QEMU's usb-storage NEVER STALLs a bulk endpoint, so a fault-injection poke makes the
recovery path reachable + regression-testable: /proc/xhci.stalltest.1 arms ONE faked
data-phase STALL. The fake is raised AFTER the real transfer completes, so the ring /
dequeue state stays consistent (recovery's Set TR Dequeue lands on the live cursor) and
the I/O still returns correct data -- i.e. a clean STALL is fully transparent.

Sequence: boot with an ext2 USB drive (mounts at /mnt/usb) -> arm one injection ->
the FIRST (cache-missing) read of /mnt/usb/etc/passwd triggers a real MSC data transfer
that the injection fake-STALLs -> recovery runs -> the read STILL returns passwd -> a
fresh write round-trips -> the controller is not wedged.

PASS = poke armed + the injection fired + the recovery path ran (serial) + the injected
read returned correct data + a post-recovery write round-trips + no wedge warning.
"""
import os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_KERNEL", os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/usb_msc_stall_%d.log" % os.getpid()
STICK = "/tmp/usb_msc_stall_stick_%d.img" % os.getpid()
PORT = 43000 + (os.getpid() % 2000)
MARK = "STALLRT_%d" % os.getpid()


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


def logtext():
    return open(LOG, errors="replace").read() if os.path.exists(LOG) else ""


def main():
    if not os.path.exists(KERNEL) or not os.path.exists(DISK):
        print("FAIL: kernel/disk missing"); return 2
    with open(DISK, "rb") as a, open(STICK, "wb") as b:
        b.write(a.read())
    if os.path.exists(LOG): os.remove(LOG)

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
    poke = cat1 = wr = alive = xstat = ""
    mounted = None
    try:
        for _ in range(70):
            time.sleep(1)
            d = logtext()
            if "mounted at /mnt/usb" in d: mounted = True; break
            if "not ext2" in d: mounted = False; break
        if mounted:
            # netconsole ready -- do NOT touch /mnt/usb yet (keep /etc uncached).
            for _ in range(40):
                time.sleep(2)
                try:
                    if "RDY" in nc("echo RDY"): break
                except Exception: pass
            # arm ONE faked data-phase STALL, then the first (cache-missing) passwd read
            # triggers a real MSC transfer that the injection fake-STALLs -> recovery.
            poke = nc("cat /proc/xhci.stalltest.1")
            cat1 = nc("cat /mnt/usb/etc/passwd")
            # prove the pipe is healthy after recovery: a fresh write round-trips.
            nc("echo %s > /mnt/usb/stall_rt.txt" % MARK)
            wr = nc("cat /mnt/usb/stall_rt.txt")
            alive = nc("echo STILLALIVE")
            xstat = nc("cat /proc/xhci")   # v0.4.273: confirm the persistent recovery counter
    finally:
        p.terminate()
        try: p.wait(5)
        except Exception: p.kill()

    log = logtext()
    img = open(STICK, "rb").read() if os.path.exists(STICK) else b""
    results = []
    def chk(n, ok, det=""):
        results.append(ok); print("  [%s] %s %s" % ("PASS" if ok else "FAIL", n, det))

    chk("drive enumerated + mounted", mounted is True and "USB MSC ready" in log)
    chk("stalltest poke armed (inject=1)", "inject = 1" in poke)
    chk("injected STALL fired (serial)", "MSC data STALL INJECTED" in log)
    chk("recovery path ran: clear halt + read CSW (serial)",
        "MSC data STALL -- clearing halt, reading CSW" in log)
    chk("injected read STILL returned correct data", "root:" in cat1)
    chk("write round-trips after recovery", MARK in wr)
    chk("write persisted to USB image (offline)", MARK.encode() in img)
    chk("controller not wedged", "controller wedged" not in log and "READ_CAPACITY failed" not in log)
    chk("system still responsive after recovery", "STILLALIVE" in alive)
    chk("recovery counter incremented (/proc/xhci: inject consumed, recoveries=1)",
        "inject=0 recoveries=1" in xstat)

    for f in (LOG, STICK):
        try: os.remove(f)
        except OSError: pass
    npass = sum(1 for r in results if r)
    print("\n=== usb msc stall-recovery suite: %d/%d passed ===" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
