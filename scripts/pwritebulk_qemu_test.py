#!/usr/bin/env python3
"""Bulk file write (PIPE_PWRITE_BULK) QEMU correctness gate -- v0.4.298.

The bulk path (src/servers/pipe_server.c + src/lib/posix_file.c) page-bounces a
large write() buffer into pipe_server and vfs_pwrites it, replacing the ~800B per
FS_PWRITE MR loop. It is armed at runtime via `cat /proc/pwritebulk.1` and ships
DEFAULT OFF. This test proves, in QEMU:

  1. byte-exactness with the feature OFF (legacy path, baseline)   -- /bin/bulkwrite
  2. byte-exactness with the feature ON  (bulk path)               -- /bin/bulkwrite
  3. the ON file is sha256-IDENTICAL to the OFF file (bulk == legacy, byte for byte)
  4. /proc/pwritebulk shows the bulk path actually engaged (calls>0, bytes>0,
     denied=0) when ON, and stays inert (calls=0) when OFF.

QEMU cannot model the A72 cacheable-mismatch coherency risk ([[feedback_pipe_shm_cache]])
-- that needs an HW soak. This gate validates LOGIC + byte-exactness only. Runs on a
PRIVATE copy of the disk (the test writes /tmp files) with a unique serial socket, so
it never clobbers the shared image or a concurrent QEMU. Pure stdlib; reuses
scripts/aios_console.Console.
"""
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_KERNEL",
    os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt"))
SRC_DISK = os.path.join(REPO, "disk/disk_ext2.img")
DISK = "/tmp/aios-pwritebulk-disk.img"          # private copy (the test writes files)
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-pwritebulk.sock"
SMP = int(os.environ.get("AIOS_SMP", "4"))

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)


def qemu_cmd():
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", str(SMP), "-m", "2G",
           "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", "unix:%s,server" % SOCK, "-kernel", KERNEL]
    for i, path in enumerate([DISK, LOGDISK]):
        if os.path.exists(path):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    cmd += ["-netdev", "user,id=n0", "-device", "virtio-net-device,netdev=n0"]
    return cmd


def sha_of(con, path):
    out = con.run("sha256sum %s" % path, 60)
    m = re.search(r"\b([0-9a-f]{64})\b", out)
    return m.group(1) if m else None


def counters(con):
    """cat /proc/pwritebulk -> dict(enabled, calls, bytes, mapfail, denied)."""
    out = con.run("cat /proc/pwritebulk", 20)
    d = {}
    for k in ("enabled", "calls", "bytes", "mapfail", "denied"):
        m = re.search(k + r"=(\d+)", out)
        d[k] = int(m.group(1)) if m else None
    return d, out


def main():
    results = []

    def check(name, ok, detail=""):
        results.append(bool(ok))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""), flush=True)

    shutil.copy(SRC_DISK, DISK)
    if os.path.exists(SOCK):
        os.unlink(SOCK)
    proc = subprocess.Popen(qemu_cmd())
    con = None
    try:
        sock = ac.connect_qemu_socket(SOCK)
        con = ac.Console(sock.fileno(), echo=True)
        print("=== booting QEMU -smp %d, serial login ===" % SMP, flush=True)
        con.read_until([ac.LOGIN_PROMPT], 120)
        time.sleep(5)
        con.read_until(["__drain__"], 3)
        con.ensure_shell("root", "root", 60, nudge=True, settle=1.0)

        # --- 1. baseline: feature OFF (default) -- byte-exact via the legacy path ---
        d0, raw0 = counters(con)
        check("default OFF (enabled=0, calls=0)",
              d0["enabled"] == 0 and d0["calls"] == 0, repr(raw0.strip()[:80]))
        out_off = con.run("bulkwrite", 120)
        check("bulkwrite OFF byte-exact (legacy path)",
              "BULKWRITE: PASS" in out_off, repr(out_off.strip()[-90:]))
        sha_off = sha_of(con, "/tmp/bw_aligned")
        check("OFF file sha read", sha_off is not None, str(sha_off))

        # --- 2. arm the bulk path ---
        arm = con.run("cat /proc/pwritebulk.1", 20)
        check("arm /proc/pwritebulk.1 (enabled=1)", "enabled=1" in arm,
              repr(arm.strip()[:80]))

        # --- 3. feature ON -- byte-exact via the bulk path ---
        out_on = con.run("bulkwrite", 120)
        check("bulkwrite ON byte-exact (bulk path)",
              "BULKWRITE: PASS" in out_on, repr(out_on.strip()[-90:]))
        sha_on = sha_of(con, "/tmp/bw_aligned")

        # --- 4. the bulk-written file is byte-IDENTICAL to the legacy-written file ---
        check("bulk file sha == legacy file sha (byte-for-byte)",
              sha_off is not None and sha_off == sha_on,
              "off=%s on=%s" % (sha_off, sha_on))

        # --- 5. the bulk path actually engaged ---
        d1, raw1 = counters(con)
        check("bulk engaged (calls>0, bytes>0, denied=0)",
              d1["calls"] and d1["calls"] > 0 and d1["bytes"] and d1["bytes"] > 0
              and d1["denied"] == 0, repr(raw1.strip()[:90]))

        # --- 6. kill switch: disarm restores OFF ---
        dis = con.run("cat /proc/pwritebulk.0", 20)
        check("disarm /proc/pwritebulk.0 (enabled=0)", "enabled=0" in dis,
              repr(dis.strip()[:80]))
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
        for p in (SOCK, DISK):
            if os.path.exists(p):
                os.unlink(p)

    npass = sum(1 for ok in results if ok)
    print("\n=== pwritebulk QEMU test: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if results and npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
