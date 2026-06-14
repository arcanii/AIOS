#!/usr/bin/env python3
"""QEMU test: sequential read-ahead prefetches cold cache lines (blk_cache.c).

Read-ahead only fires on a sustained sequential read against a COLD cache, so
this uses the two-boot pattern (like reboot_flush_qemu_test):
  Boot 1: create a large contiguous file, sync it to disk, reboot.
  Boot 2: same disk image (cold cache); read the file end-to-end and check
          /proc/cachestats prefetch climbed, and the file reads back at full
          size (the multi-fill + prefetch path returns correct data).
The mixed-workload sync test shows prefetch stays 0 on scattered access, so the
two together bound the behavior: fires on sequential, quiet on random.

Serial console only (no network). OK/FAIL output, nonzero exit on FAIL.
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
SOCK = "/tmp/aios-readahead-test.sock"

FILE_KB = 512                       # 0.5 MB = 128 cache lines
FILE_BYTES = FILE_KB * 1024

# Private disk copies: both boots reuse the SAME mutated image without dirtying
# the shared dev image or colliding with another QEMU write lock.
WORKDIR = tempfile.mkdtemp(prefix="aios-readahead-")
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


def boot_and_login(timeout=120):
    if os.path.exists(SOCK):
        os.unlink(SOCK)
    proc = subprocess.Popen(qemu_cmd())
    sock = ac.connect_qemu_socket(SOCK)
    con = ac.Console(sock.fileno(), echo=True)
    con.read_until(["AIOS login:"], timeout)
    time.sleep(3)
    con.read_until(["__quiesce_never__"], 3)
    con.ensure_shell("root", "root", 60, nudge=True, settle=1.0)
    return proc, con


def stop_qemu(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


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

    proc = None
    try:
        # ---- Boot 1: create a large contiguous file, sync, reboot ----
        print("=== boot 1: create %dKB file + sync + reboot ===" % FILE_KB, flush=True)
        proc, con = boot_and_login()
        con.run("dd if=/dev/zero of=/ratest bs=4096 count=%d 2>&1" % (FILE_BYTES // 4096), 60)
        out = con.run("ls -l /ratest", 30)
        check("big file created (boot 1)", str(FILE_BYTES) in out, repr(out.strip()[:90]))
        con.sendline("sync; echo %s > /rasync; reboot" % "ra_synced")
        pat, out = con.read_until(["AIOS reboot -- resetting board"], 60)
        check("reboot banner after sync", pat is not None, repr(out.strip()[-80:]))
        time.sleep(1)
        stop_qemu(proc)
        proc = None

        # ---- Boot 2: cold cache -- sequential read must prefetch ----
        print("\n=== boot 2: cold sequential read triggers prefetch ===", flush=True)
        proc, con = boot_and_login()
        out = con.run("ls -l /ratest", 30)
        check("file persisted at full size", str(FILE_BYTES) in out, repr(out.strip()[:90]))
        before = stat_field(con.run("cat /proc/cachestats", 15), "prefetch")
        con.run("wc -c /ratest", 30)               # sequential end-to-end cold read (reads all bytes)
        cs = con.run("cat /proc/cachestats", 15)
        after = stat_field(cs, "prefetch")
        rm = stat_field(cs, "read_multi")
        check("prefetch climbed on sequential read", after > before,
              "before=%d after=%d" % (before, after))
        check("multi-fill path used", rm > 0, "read_multi=%d" % rm)

    except Exception as e:
        import traceback
        traceback.print_exc()
        check("harness exception", False, "%s: %s" % (type(e).__name__, e))
    finally:
        if proc is not None:
            stop_qemu(proc)
        if os.path.exists(SOCK):
            os.unlink(SOCK)
        shutil.rmtree(WORKDIR, ignore_errors=True)

    npass = sum(1 for _, ok, _ in results if ok)
    print("\n=== read-ahead QEMU test: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())
