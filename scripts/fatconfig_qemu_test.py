#!/usr/bin/env python3
"""QEMU test: FAT32 config-over-network (read + write an existing boot-partition
file, src/fat32.c).

Boots AIOS from a PARTITIONED disk (MBR + FAT32 boot + ext2 root, like the real
Pi card -- blk_virtio.c parses the MBR) and exercises the round-trip:
  fatswap --read config.txt           -> dump the FAT file
  ... append a marker line ...
  fatswap /tmp/c config.txt           -> write it back (crash-safe, sha-verified)
  fatswap --read config.txt           -> the marker persists

Needs the partitioned test image disk/sdcard-test.img (built via mksdcard if
absent -- macOS/local, like the HW tests). Each run operates on a fresh copy so
the marker append does not accumulate.
"""
import os, sys, shutil, subprocess, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
SDCARD = os.path.join(REPO, "disk/sdcard-test.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
MARKER = "FATTEST_ROUNDTRIP_OK"

def ensure_sdcard():
    if os.path.exists(SDCARD):
        return True
    print("=== building disk/sdcard-test.img (mksdcard, one-time) ===", flush=True)
    krn = os.path.join(REPO, "build-rpi4-netd/images/aios_root-image-arm-bcm2711")
    rc = subprocess.call(["python3", os.path.join(REPO, "scripts/mksdcard.py"),
                          "--kernel", krn, "--output", SDCARD])
    return rc == 0 and os.path.exists(SDCARD)

def main():
    if not ensure_sdcard():
        print("FAIL: could not obtain disk/sdcard-test.img"); return 2

    tmp = tempfile.mkdtemp(prefix="aios-fatcfg-")
    disk = os.path.join(tmp, "sdcard.img")
    shutil.copy(SDCARD, disk)
    log = os.path.join(tmp, "serial.log")

    # Plain pipelines only -- a command substitution $(... | wc) is 3 forks and
    # hits "Cannot fork" under load; the marker round-trip is the real proof.
    cmds = [
        "fatswap --read config.txt | grep -q arm_64bit && echo READ_OK",
        "fatswap --read config.txt > /tmp/c; printf '%%s\\n' '# %s' >> /tmp/c; "
        "fatswap /tmp/c config.txt | grep -q OK && echo SWAP_REPORTED_OK" % MARKER,
        "fatswap --read config.txt | grep -q '%s' && echo MARKER_PERSISTS" % MARKER,
    ]
    argv = ["python3", os.path.join(REPO, "scripts/aios_console.py"), "qemu",
            "--kernel", KERNEL, "--smp", "4", "--mem", "2G",
            "--disk", disk, "--disk", LOGDISK,
            "--shutdown", "--boot-timeout", "180", "--cmd-timeout", "30",
            "--log", log]
    for c in cmds:
        argv += ["--cmd", c]
    subprocess.call(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    text = open(log, errors="replace").read().replace("\r", "")

    results = []
    def check(label, ok, detail=""):
        results.append(ok)
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", label,
                               ("  -- " + detail) if detail else ""), flush=True)

    check("read config.txt (FAT read works)", "READ_OK" in text)
    check("write reported OK (sha-verified)", "SWAP_REPORTED_OK" in text)
    check("marker persists in the FAT after write", "MARKER_PERSISTS" in text)

    shutil.rmtree(tmp, ignore_errors=True)
    npass = sum(1 for ok in results if ok)
    print("\n=== FAT config round-trip test: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) else 1

if __name__ == "__main__":
    sys.exit(main())
