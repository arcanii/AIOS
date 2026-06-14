#!/usr/bin/env python3
"""QEMU test: ext2_truncate frees + discards blocks beyond the new size.

Runs the on-device test_ftruncate app, which now exercises the indirect-block
free paths: it writes a 320KB file (direct + single-indirect + double-indirect),
ftruncates it to 20KB and verifies the kept bytes are byte-exact (a wrong-block
free would corrupt them), truncates to 0, then writes + verifies a fresh 256KB
file to prove the freed blocks are reclaimable and the fs is uncorrupted. Also
checks /proc/cachestats discarded climbed (truncate fed the discard path).

Serial console only. OK/FAIL output, nonzero exit on FAIL.
"""
import importlib.util
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-truncate-test.sock"

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
    for i, path in enumerate([DISK, LOGDISK]):
        if os.path.exists(path):
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

        before = stat_field(con.run("cat /proc/cachestats", 15), "discarded")
        print("\n=== running test_ftruncate (small + big/indirect) ===", flush=True)
        out = con.run("test_ftruncate", 90)
        check("small truncate to 50", "truncated to 50 bytes" in out, repr(out.strip()[-80:]))
        check("big truncate kept data intact", "big truncate kept" in out,
              repr([l for l in out.splitlines() if "FAIL" in l or "big" in l][-2:]))
        check("freed blocks reclaimed, fs intact", "freed blocks reclaimed" in out,
              repr([l for l in out.splitlines() if "FAIL" in l or "reclaim" in l][-2:]))
        check("test completed (FTRUNC-DONE)", "FTRUNC-DONE" in out, repr(out.strip()[-60:]))
        after = stat_field(con.run("cat /proc/cachestats", 15), "discarded")
        check("truncate fed the discard path", after > before,
              "discarded before=%d after=%d" % (before, after))

        print("\n=== fs intact after ===", flush=True)
        out = con.run("ls / | head", 15)
        check("ls / works", "bin" in out, repr(out.strip()[:80]))

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

    npass = sum(1 for _, ok, _ in results if ok)
    print("\n=== truncate QEMU test: %d/%d passed ===" % (npass, len(results)), flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())
