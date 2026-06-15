#!/usr/bin/env python3
"""USB Mass Storage DRIVE-SWAP cache-coherency test on QEMU -- v0.4.256.

Regression test for the blk_cache drive-2 stale-cache bug: when a USB drive is unplugged
and a DIFFERENT drive is plugged into the same slot within one boot, reads must come from
the NEW drive, not the old drive's cached sectors. The fix is a per-drive generation bumped
by blk_cache_invalidate() in device_teardown.

Two distinct ext2 drives (both start as copies of disk_ext2.img; AIOS writes a marker to A
so they differ on-media):
  1. hot-add drive A, mount, `echo <MARKA> > /mnt/usb/whoami` (persists to A; caches A's
     superblock + root-dir + inode blocks), read it back.
  2. unplug A (device_teardown -> blk_cache_invalidate(2)).
  3. hot-add drive B (a pristine copy, NO whoami), re-mount.
  4. `cat /mnt/usb/whoami` MUST NOT return <MARKA> -- B has no such file. WITHOUT the fix the
     cached root-dir/inode blocks from A still resolve whoami -> stale <MARKA> (silent
     corruption). WITH the fix the blocks are re-read from B -> the file is absent.
  5. prove B is genuinely mounted: write <MARKB> to /mnt/usb/whoami_b on B, read it back.

PASS = A read its own marker, B mounted, B does NOT serve A's stale whoami, B reads/writes
its own data, and the offline images match (A has MARKA, B has MARKB but not MARKA).
"""
import os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_KERNEL", os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
LOG = "/tmp/usb_msc_swap_%d.log" % os.getpid()
STICKA = "/tmp/usb_msc_swap_A_%d.img" % os.getpid()
STICKB = "/tmp/usb_msc_swap_B_%d.img" % os.getpid()
PORT = 47000 + ((os.getpid() + 311) % 2000)
MON = 49000 + ((os.getpid() + 311) % 2000)
MARKA = "DRIVEA_%d" % os.getpid()
MARKB = "DRIVEB_%d" % os.getpid()


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
    for st in (STICKA, STICKB):
        with open(DISK, "rb") as a, open(st, "wb") as b:
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
            "-drive", "file=%s,format=raw,if=none,id=stickA" % STICKA,
            "-kernel", KERNEL]

    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    boot = False; mountA = catA = torn = mountB = catB_stale = lsB = wrB = ""
    try:
        for _ in range(60):
            time.sleep(2)
            try:
                if "RDY" in nc("echo RDY"): boot = True; break
            except Exception: pass
        if not boot:
            raise SystemExit

        s = socket.create_connection(("127.0.0.1", MON), timeout=5); time.sleep(0.5); s.recv(8192)

        # 1. drive A: hot-add, mount, write+read a marker (caches A's dir/inode blocks)
        print("[swap] add drive A", flush=True)
        hmp(s, "device_add usb-storage,bus=xhci.0,drive=stickA,id=mscA")
        mountA = "yes" if wait_for("runtime mount: mounted at /mnt/usb", 1, 15) else ""
        if mountA:
            nc("echo %s > /mnt/usb/whoami" % MARKA)
            catA = nc("cat /mnt/usb/whoami")
            nc("ls /mnt/usb")                       # cache A's root dir block

        # 2. unplug A (-> blk_cache_invalidate(2))
        print("[swap] del drive A", flush=True)
        hmp(s, "device_del mscA")
        torn = "yes" if wait_for("torn down", 1, 10) else ""

        # 3. drive B: a pristine copy with NO whoami, hot-add into the same slot
        print("[swap] add drive B (different drive, same slot)", flush=True)
        hmp(s, "drive_add 0 if=none,id=stickB,file=%s,format=raw" % STICKB)
        hmp(s, "device_add usb-storage,bus=xhci.0,drive=stickB,id=mscB")
        mountB = "yes" if wait_for("runtime mount: mounted at /mnt/usb", 2, 15) else ""

        # 4. THE KEY CHECK: B must NOT serve A's cached whoami
        if mountB:
            catB_stale = nc("cat /mnt/usb/whoami")
            # 5. prove B is genuinely the mounted fs
            lsB = nc("ls /mnt/usb")
            nc("echo %s > /mnt/usb/whoami_b" % MARKB)
            wrB = nc("cat /mnt/usb/whoami_b")
        s.close()
    except Exception as ex:
        print("error:", ex)
    finally:
        time.sleep(1)
        p.terminate()
        try: p.wait(5)
        except Exception: p.kill()

    imgA = open(STICKA, "rb").read() if os.path.exists(STICKA) else b""
    imgB = open(STICKB, "rb").read() if os.path.exists(STICKB) else b""
    results = []
    def chk(n, ok, det=""):
        results.append(ok); print("  [%s] %s %s" % ("PASS" if ok else "FAIL", n, det))
    chk("boot reached netconsole", boot)
    chk("drive A mounted", mountA == "yes")
    chk("drive A read its own marker", MARKA in catA)
    chk("unplug A -> torn down (cache invalidated)", torn == "yes")
    chk("drive B mounted after swap", mountB == "yes")
    chk("B does NOT serve A's stale whoami data (the fix)", MARKA not in catB_stale,
        "(got: %r)" % catB_stale.strip()[:40])
    # the robust signal: the ROOT DIR block. If EITHER cache (ext2 front or blk_cache back)
    # serves A's stale root dir, ls lists whoami -- which B does not have. Catches both layers
    # regardless of whether the whoami data block happened to be evicted.
    chk("B's dir is fresh -- ls does NOT show A's whoami", "whoami" not in lsB.split(),
        "(ls: %r)" % " ".join(lsB.split())[:60])
    chk("drive B is genuinely mounted (ls works)", "bin" in lsB and "etc" in lsB)
    chk("drive B reads+writes its own data", MARKB in wrB)
    chk("offline: A holds MARKA", MARKA.encode() in imgA)
    chk("offline: B holds MARKB but NOT MARKA", MARKB.encode() in imgB and MARKA.encode() not in imgB)

    for f in (LOG, STICKA, STICKB):
        try: os.remove(f)
        except OSError: pass
    npass = sum(1 for r in results if r)
    print("\n=== usb msc swap suite: %d/%d passed ===" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
