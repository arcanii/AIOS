#!/usr/bin/env python3
"""QEMU functional test for the Stage-4 /bin/netdiag NET_DIAG path (DESIGN_NETD s6).

Boots the flag-ON netd build (build-netd) and drives the userland `netdiag` tool
over netconsole, exercising the full client -> net_ep -> NET_DIAG(103) -> netd ->
plat_net_diag -> reply -> netdiag round-trip that Stage 4 adds:

  - netdiag            : liveness probe still REACHABLE (no-arg path unchanged)
  - netdiag peek 0     : reads virtio-mmio reg 0 -> the magic 0x74726976 ("virt")
  - netdiag mac        : reads the device MAC (QEMU virtio default 52:54:00:...)
  - netdiag mr 1 0     : MDIO read is GENET-only -> ret -2 + "not supported"
  - netdiag tx         : sends one broadcast test frame -> ret 0

The virtio plat_net_diag only implements peek/poke/tx/mac (no MDIO/PHY), so the
mr check also proves the unsupported-op reply path. The GENET ops (mr/mw/reinit/
irqon/irqoff) are HW-verified separately on the Pi.

Per qemu-test-hygiene: PRIVATE disk copies, a test-unique serial socket + host
port, tears down only its own QEMU. Run after build_apps.py (needs the new
netdiag on the disk) and a build-netd kernel.
"""
import importlib.util
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get(
    "AIOS_NETD_KERNEL",
    os.path.join(REPO, "build-netd/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)

SOCK = "/tmp/aios-netdiag-%d.sock" % os.getpid()
P_NETCON = 36000 + (os.getpid() % 1500)


class Netcon:
    def __init__(self, host, port, timeout=20):
        self.s = socket.create_connection((host, port), timeout=15)
        self.s.settimeout(1.0)
        self.buf = ""
        self.timeout = timeout

    def expect(self, pat, timeout=None):
        deadline = time.time() + (timeout or self.timeout)
        while True:
            i = self.buf.find(pat)
            if i != -1:
                out = self.buf[:i + len(pat)]
                self.buf = self.buf[i + len(pat):]
                return out
            if time.time() > deadline:
                raise TimeoutError("expect %r timed out; buf=%r" % (pat, self.buf[-200:]))
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                continue
            if not d:
                raise EOFError("netconsole closed; buf=%r" % self.buf[-200:])
            self.buf += d.decode("utf-8", "replace")

    def run(self, cmd, timeout=20):
        self.s.sendall((cmd + "\n").encode())
        return self.expect("aios# ", timeout)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def qemu_cmd(disk, logdisk):
    cmd = [
        "qemu-system-aarch64",
        "-machine", "virt,virtualization=on",
        "-cpu", "cortex-a53", "-smp", "4", "-m", "2G",
        "-display", "none", "-monitor", "none", "-no-reboot",
        "-serial", "unix:%s,server" % SOCK,
        "-kernel", KERNEL,
    ]
    for i, path in enumerate([disk, logdisk]):
        if path and os.path.exists(path):
            cmd += ["-drive", "file=%s,format=raw,if=none,id=hd%d" % (path, i),
                    "-device", "virtio-blk-device,drive=hd%d" % i]
    cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:2323" % P_NETCON,
            "-device", "virtio-net-device,netdev=n0"]
    return cmd


def _drain(sk, stop):
    sk.settimeout(0.5)
    while not stop.is_set():
        try:
            if not sk.recv(4096):
                return
        except (socket.timeout, OSError):
            continue


def main():
    if not os.path.exists(KERNEL):
        print("FAIL: kernel not found: %s (build build-netd)" % KERNEL); return 2
    if not os.path.exists(DISK):
        print("FAIL: disk not found: %s (run build_apps.py)" % DISK); return 2

    tmp = tempfile.mkdtemp(prefix="aios-netdiag-")
    disk = os.path.join(tmp, "disk.img"); shutil.copy(DISK, disk)
    logdisk = os.path.join(tmp, "log.img")
    if os.path.exists(LOGDISK):
        shutil.copy(LOGDISK, logdisk)
    else:
        logdisk = None

    if os.path.exists(SOCK):
        os.unlink(SOCK)

    stop = threading.Event()
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)

    proc = serial = nc = None
    try:
        proc = subprocess.Popen(qemu_cmd(disk, logdisk))
        serial = ac.connect_qemu_socket(SOCK, timeout=20)
        threading.Thread(target=_drain, args=(serial, stop), daemon=True).start()

        print("=== waiting for netconsole (boot can take ~90s) ===", flush=True)
        deadline = time.time() + 240
        while time.time() < deadline:
            try:
                nc = Netcon("127.0.0.1", P_NETCON)
                nc.expect("aios# ", timeout=8)
                break
            except (OSError, TimeoutError, EOFError):
                nc = None
                time.sleep(2.0)
        check("netconsole reachable (boot ok)", nc is not None)
        if nc is None:
            return 1

        # no-arg liveness probe (existing behavior must survive the rewrite)
        out = nc.run("netdiag", 20)
        check("liveness probe REACHABLE", "net server REACHABLE" in out,
              "" if "REACHABLE" in out else repr(out.replace("aios# ", "").strip()[:160]))

        # peek 0 -> virtio-mmio magic 0x74726976. This is the core round-trip:
        # userland Call -> NET_DIAG -> netd plat_net_diag -> reply MR1 -> print.
        out = nc.run("netdiag peek 0", 20)
        m = re.search(r"\[00000\]\s*=\s*([0-9a-fA-F]+)", out)
        val = m.group(1).lower() if m else None
        check("peek 0 = virtio magic", val == "74726976", "(got %s)" % val)

        # mac -> a non-zero, well-formed MAC read from virtio config space.
        out = nc.run("netdiag mac", 20)
        m = re.search(r"mac = ([0-9a-f:]{17})", out)
        mac = m.group(1) if m else None
        check("mac well-formed + non-zero", bool(mac) and mac != "00:00:00:00:00:00",
              "(got %s)" % mac)

        # mr (MDIO) is GENET-only -> the virtio default branch returns -2.
        out = nc.run("netdiag mr 1 0", 20)
        check("mdio unsupported on virtio (-2)",
              "ret -2" in out or "not supported" in out,
              "" if ("-2" in out or "not supported" in out)
              else repr(out.replace("aios# ", "").strip()[:160]))

        # tx -> send one broadcast frame; virtio plat_net_tx returns 0.
        out = nc.run("netdiag tx", 20)
        m = re.search(r"tx ret=(-?\d+)", out)
        txr = int(m.group(1)) if m else None
        check("tx test frame sent (ret 0)", txr == 0, "(ret=%s)" % txr)

    except Exception as e:   # noqa: BLE001
        print("EXCEPTION: %r" % e)
        check("driver", False, repr(e))
    finally:
        stop.set()
        if nc is not None:
            nc.close()
        if serial is not None:
            try:
                serial.close()
            except OSError:
                pass
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        if os.path.exists(SOCK):
            os.unlink(SOCK)
        shutil.rmtree(tmp, ignore_errors=True)

    npass = sum(1 for _, ok, _ in results if ok)
    ntot = len(results)
    print("\n=== netdiag NET_DIAG suite: %d/%d passed ===" % (npass, ntot))
    return 0 if npass == ntot and ntot > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
