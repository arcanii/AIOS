#!/usr/bin/env python3
"""QEMU test for the thread_server deferred-join rework (v0.4.191).

  - test_threads: 2 mutex workers, shared==2000 (create/join correctness).
  - test_join: deferred path (join before exit), zombie path (exit before
    join), retval propagation, and slot reuse.
Both must print PASS. This exercises the new event-loop fault routing, deferred
reply caps, and cap cleanup that replaced the head-of-line-blocking join.
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
SOCK = "/tmp/aios-thread-test.sock"

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)


def qemu_cmd():
    cmd = ["qemu-system-aarch64", "-machine", "virt,virtualization=on",
           "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
           "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", "unix:%s,server" % SOCK, "-kernel", KERNEL]
    for i, path in enumerate([DISK, LOGDISK]):
        if os.path.exists(path):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    return cmd


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
        con.read_until(["__drain__"], 3)
        con.ensure_shell("root", "root", 60, nudge=True, settle=1.0)

        print("\n=== test_threads (mutex, shared=2000) ===", flush=True)
        out = con.run("test_threads", 40)
        check("test_threads PASS", "PASS" in out and "shared=2000" in out,
              repr(out.strip()[-80:]))

        print("\n=== test_join (deferred + zombie + retval + reuse) ===", flush=True)
        out = con.run("test_join", 40)
        check("test_join PASS", "PASS" in out, repr(out.strip()[-90:]))
        check("deferred retval correct", "deferred retval=0x1235" in out,
              repr(out.strip()[-90:]))
        check("zombie retval correct", "zombie retval=0x5679" in out,
              repr(out.strip()[-90:]))

        # Re-run test_join to confirm the server is still healthy (no cap leak
        # wedge) after the first run reaped several threads.
        print("\n=== test_join again (server still healthy) ===", flush=True)
        out = con.run("test_join", 40)
        check("test_join PASS (2nd run)", "PASS" in out, repr(out.strip()[-90:]))

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
    print("\n=== thread_server QEMU test: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())
