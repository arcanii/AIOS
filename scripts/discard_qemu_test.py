#!/usr/bin/env python3
"""QEMU test: ext2 unlink discards freed blocks through the block cache.

On QEMU the virtio discard backend is a no-op, but the FS->cache discard plumbing
still runs, so this verifies the path end to end without HW:
  1. create a multi-line file, sync (its 4KB cache lines are resident + clean).
  2. rm it -- ext2_unlink frees the data blocks, coalesces them into runs, and
     calls blk_cache_discard, which invalidates the fully-covered lines.
  3. /proc/cachestats discarded climbs, and the fs is intact afterwards (a new
     file reads back, ls works) -- the discard path did not corrupt anything.
Real CMD38 erase is verified separately on the RPi4.

Serial console only. OK/FAIL output, nonzero exit on FAIL.
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-discard-test.sock"

FILE_BYTES = 128 * 1024     # 128 KB = 32 cache lines

WORKDIR = tempfile.mkdtemp(prefix="aios-discard-")
DISKS = []
for _src in [DISK, LOGDISK]:
    if os.path.exists(_src):
        _dst = os.path.join(WORKDIR, os.path.basename(_src))
        shutil.copyfile(_src, _dst)
        DISKS.append(_dst)

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)


def qemu_cmd():
    cmd = [
        "qemu-system-aarch64",
        "-machine", "virt,virtualization=on",
        "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
        "-display", "none", "-monitor", "none", "-no-reboot",
        "-serial", "unix:%s,server" % SOCK,
        "-kernel", KERNEL,
    ]
    for i, path in enumerate(DISKS):
        cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                "-device", "virtio-blk-device,drive=hd%d" % i]
    return cmd


def stat_field(out, key):
    for ln in out.replace("\r", "").split("\n"):
        ln = ln.strip()
        if ln.startswith(key + ":"):
            try:
                return int(ln.split(":", 1)[1].strip())
            except ValueError:
                return -1
    return -1


def main():
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""), flush=True)

    if os.path.exists(SOCK):
        os.unlink(SOCK)
    proc = subprocess.Popen(qemu_cmd())
    con = None
    try:
        sock = ac.connect_qemu_socket(SOCK)
        con = ac.Console(sock.fileno(), echo=True)
        con.read_until(["AIOS login:"], 120)
        time.sleep(3)
        con.read_until(["__quiesce_never__"], 3)
        con.ensure_shell("root", "root", 60, nudge=True, settle=1.0)

        print("\n=== create + sync + rm ===", flush=True)
        con.run("dd if=/dev/zero of=/dtest bs=4096 count=%d 2>&1" % (FILE_BYTES // 4096), 60)
        out = con.run("ls -l /dtest", 20)
        check("file created", str(FILE_BYTES) in out, repr(out.strip()[:90]))
        con.run("sync", 20)
        before = stat_field(con.run("cat /proc/cachestats", 15), "discarded")
        con.run("rm /dtest", 20)
        after = stat_field(con.run("cat /proc/cachestats", 15), "discarded")
        check("discarded climbed on rm", after > before,
              "before=%d after=%d" % (before, after))

        print("\n=== fs intact after discard ===", flush=True)
        out = con.run("ls /dtest 2>&1", 15)
        check("file is gone", "dtest" not in out or "No such" in out or "not found" in out.lower(),
              repr(out.strip()[:80]))
        con.run("echo postdiscard_ok > /dtest2", 15)
        out = con.run("cat /dtest2", 15)
        check("new file reads back (fs intact)", "postdiscard_ok" in out, repr(out.strip()[:80]))
        out = con.run("ls / | head", 15)
        check("ls / works", "dtest2" in out, repr(out.strip()[:80]))

    except Exception as e:
        import traceback
        traceback.print_exc()
        check("harness exception", False, "%s: %s" % (type(e).__name__, e))
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        if os.path.exists(SOCK):
            os.unlink(SOCK)
        shutil.rmtree(WORKDIR, ignore_errors=True)

    npass = sum(1 for _, ok, _ in results if ok)
    print("\n=== discard QEMU test: %d/%d passed ===" % (npass, len(results)), flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())
