#!/usr/bin/env python3
"""QEMU test: reboot flushes the write-back cache via the fs_thread (FS_SYNC).

Regression test for the pipe_server-thread flush bug: aios_system_reboot and
aios_system_shutdown used to call blk_cache_flush() directly from the
pipe_server thread (PIPE_SHUTDOWN handler), racing in-flight fs IO -- the
block cache and DMA ring are fs-thread-serialized (see flush_server.c). The
fix routes the final flush through seL4_Call(fs_ep, FS_SYNC).

Two-phase test against ONE persistent disk image:
  Boot 1: log in, start a background writer hammering the disk, write a probe
          file, run /bin/reboot. The reboot banner only prints after the
          FS_SYNC call returns, so reaching it proves the flush completed
          while the fs thread was busy with the background writer.
  Boot 2: same disk image; log in, read the probe file back. The probe was
          dirty in the write-back cache when reboot ran, so surviving the
          power cycle proves the flush wrote it out (and the clean boot
          proves no block-layer corruption from the concurrent writer).

Uses the serial console only (no network needed). On QEMU, reboot falls back
to a clean halt, so phase 2 is a fresh QEMU process on the same image.
"""
import importlib.util
import os
import random
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")
SOCK = "/tmp/aios-rbflush-test.sock"

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


def main():
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""), flush=True)

    nonce = "rbflush_%08x" % random.getrandbits(32)
    proc = None
    try:
        # ---- Boot 1: dirty the cache, reboot under concurrent fs IO ----
        print("=== boot 1: write probe + reboot under load ===", flush=True)
        proc, con = boot_and_login()

        # Background writer keeps the fs thread busy with block IO across the
        # reboot, exercising the old race window.
        con.run("sh -c 'while :; do echo bgline >> /bgwrite.txt; done' &", 15)
        time.sleep(2)

        # Probe file: freshly dirty in the write-back cache when reboot runs.
        con.run("echo %s > /rbtest.txt" % nonce, 15)
        out = con.run("cat /rbtest.txt", 15)
        check("probe written (boot 1)", nonce in out, repr(out.strip()[:60]))

        # /bin/reboot -> PIPE_SHUTDOWN(1) -> aios_system_reboot. The banner
        # prints only after the FS_SYNC seL4_Call returns; on QEMU the reboot
        # path then halts instead of resetting.
        con.sendline("reboot")
        out = con.read_until(["AIOS reboot -- resetting board"], 30)
        check("reboot banner after flush", "resetting board" in out,
              repr(out.strip()[-80:]))
        time.sleep(1)
        stop_qemu(proc)
        proc = None

        # ---- Boot 2: same disk image, probe must have survived ----
        print("\n=== boot 2: verify probe survived the reboot ===", flush=True)
        proc, con = boot_and_login()
        out = con.run("cat /rbtest.txt", 15)
        check("probe survived reboot", nonce in out, repr(out.strip()[:60]))
        out = con.run("ls -l /bgwrite.txt", 15)
        check("bgwriter file present (fs intact)", "bgwrite.txt" in out,
              repr(out.strip()[:80]))

        # Tidy the persistent image for the next test run.
        con.run("rm -f /rbtest.txt /bgwrite.txt", 15)

    except Exception as e:
        import traceback
        traceback.print_exc()
        check("harness exception", False, "%s: %s" % (type(e).__name__, e))
    finally:
        if proc is not None:
            stop_qemu(proc)
        if os.path.exists(SOCK):
            os.unlink(SOCK)

    npass = sum(1 for _, ok, _ in results if ok)
    print("\n=== reboot-flush QEMU test: %d/%d passed ===" % (npass, len(results)),
          flush=True)
    return 0 if npass == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())
