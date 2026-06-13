#!/usr/bin/env python3
"""QEMU selftest for the netd skeleton (DESIGN_NETD Stage 2, AIOS_NETD=ON).

Builds a dedicated AIOS_NETD=ON kernel (build-netd/, so build-04 stays flag-OFF),
boots it, and checks the boot-time selftest that spawn_netd drives over the
serial console. The skeleton runs on its OWN test endpoint; the real net stack
still serves net_ep in the root task, so networking (netconsole) keeps working --
which is itself the crash-containment proof.

What it asserts (all from spawn_netd.c / netd.c serial output):
  - the isolated netd process spawns + parses its argv caps + self-binds its ntfn
  - it announces DEVD_READY; the dedicated root fault listener receives it
  - NETD_PING round-trips                                        (liveness)
  - NETD_BLOCK_KICK: netd defers the reply into its OWN cnode and self-wakes the
    caller on a badge-2 kick  (child-cnode SaveCaller + Send + Delete)  -> 0x600d
  - NETD_BLOCK_SWEEP: root CNode_Moves netd's saved reply cap out and Sends it
    -> 0xd00d  == THE KERNEL BET (DESIGN_NETD s10 reply-sweep) PROVEN
  - NETD_CRASH: the fault is delivered to the listener, decoded, and CONTAINED;
    the system still comes up to a working netconsole shell.

Per qemu-test-hygiene: PRIVATE disk copies in a tempdir, a test-unique serial
socket + host ports, and only this script's own QEMU is torn down.
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
BUILD = os.environ.get("AIOS_NETD_BUILD", os.path.join(REPO, "build-netd"))
KERNEL = os.environ.get(
    "AIOS_NETD_KERNEL",
    os.path.join(BUILD, "images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)

SOCK = "/tmp/aios-netd-%d.sock" % os.getpid()
PORT_BASE = 36600 + (os.getpid() % 1500)
P_NETCON = PORT_BASE + 0


def ensure_build():
    """Configure (if needed) + build the AIOS_NETD=ON kernel in build-netd/."""
    if os.path.exists(KERNEL) and os.environ.get("AIOS_NETD_KERNEL"):
        return True   # caller supplied a prebuilt kernel
    os.makedirs(BUILD, exist_ok=True)
    if not os.path.exists(os.path.join(BUILD, "CMakeCache.txt")):
        print("=== configuring %s (AIOS_NETD=ON) ===" % BUILD, flush=True)
        rc = subprocess.call(
            ["cmake", "-G", "Ninja",
             "-DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake",
             "-DCROSS_COMPILER_PREFIX=aarch64-linux-gnu-",
             "-DAIOS_NETD=ON", ".."],
            cwd=BUILD)
        if rc != 0:
            print("FAIL: cmake configure returned %d" % rc)
            return False
    print("=== building netd kernel (ninja -C %s) ===" % BUILD, flush=True)
    rc = subprocess.call(["ninja"], cwd=BUILD)
    if rc != 0:
        print("FAIL: ninja returned %d" % rc)
        return False
    return os.path.exists(KERNEL)


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


class SerialCapture:
    """Continuously drain the guest serial socket (so the TTY never blocks) while
    accumulating everything for substring checks."""
    def __init__(self, sk):
        self.sk = sk
        self.buf = ""
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        self.sk.settimeout(0.5)
        while not self.stop.is_set():
            try:
                d = self.sk.recv(4096)
            except (socket.timeout, OSError):
                continue
            if not d:
                return
            with self.lock:
                self.buf += d.decode("utf-8", "replace")

    def text(self):
        with self.lock:
            return self.buf

    def wait_for(self, pat, deadline):
        while time.time() < deadline:
            if pat in self.text():
                return True
            time.sleep(0.5)
        return pat in self.text()

    def close(self):
        self.stop.set()


def main():
    if not ensure_build():
        return 2
    if not os.path.exists(DISK):
        print("FAIL: disk not found: %s (run build_apps.py first)" % DISK)
        return 2

    tmp = tempfile.mkdtemp(prefix="aios-netd-")
    disk = os.path.join(tmp, "disk.img"); shutil.copy(DISK, disk)
    logdisk = os.path.join(tmp, "log.img")
    if os.path.exists(LOGDISK):
        shutil.copy(LOGDISK, logdisk)
    else:
        logdisk = None
    if os.path.exists(SOCK):
        os.unlink(SOCK)

    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)

    proc = None
    cap = None
    try:
        proc = subprocess.Popen(qemu_cmd(disk, logdisk))
        serial = ac.connect_qemu_socket(SOCK, timeout=20)
        cap = SerialCapture(serial)

        # The selftest runs during boot (before getty). Wait out the slow boot
        # for the final containment line, then evaluate every check against the
        # captured serial. Boot is slow under VKA/morecore pressure.
        print("=== waiting for netd selftest (boot can take ~120s) ===", flush=True)
        deadline = time.time() + 240
        got_fault = cap.wait_for("[netd-listener] FAULT", deadline)

        txt = cap.text()

        def has(pat):
            return pat in txt

        check("spawn + argv + self-bind", has("[netd] skeleton up:"))
        check("DEVD_READY -> listener", has("skeleton READY"))
        check("NETD_PING round-trip", has("[netd-test] PING rc=0"))
        check("in-netd park + badge-2 self-wake (KICK)",
              has("[netd-test] KICK woke rc=0x600d"))
        check("reply-sweep CNode_Move (err=0)",
              bool(re.search(r"\[netd-sweep\] CNode_Move .*err=0", txt)))
        check("reply-sweep Send issued", has("[netd-sweep] reply Send issued"))

        # THE BET: did the root reply-sweep wake netd's parked caller?
        swept = has("[netd-test] SWEEP woke rc=0xd00d")
        sweep_detail = "" if swept else (
            "BET KILLED: Send issued but caller never woke"
            if has("[netd-sweep] reply Send issued")
            else "(no sweep output captured)")
        check("REPLY-SWEEP PROVEN (root woke a netd-parked caller)",
              swept, sweep_detail)

        check("crash delivered + CONTAINED",
              got_fault and has("CONTAINED"))

        # Post-crash liveness: the in-root net stack + shell must still work after
        # the netd skeleton died mid-boot. netconsole auto-starts (getty forks it).
        nc = None
        ncd = time.time() + 120
        while time.time() < ncd:
            try:
                nc = socket.create_connection(("127.0.0.1", P_NETCON), timeout=10)
                nc.settimeout(2.0)
                break
            except OSError:
                nc = None
                time.sleep(2.0)
        alive = False
        if nc is not None:
            try:
                # read the banner/prompt, then echo a unique token
                _drain_until(nc, "aios# ", 8)
                token = "NETD_ALIVE_%d" % os.getpid()
                nc.sendall(("echo %s\n" % token).encode())
                alive = _drain_until(nc, token, 10)
            except (OSError, TimeoutError):
                alive = False
            finally:
                try:
                    nc.close()
                except OSError:
                    pass
        check("system alive after netd crash (netconsole shell)", alive)

    except Exception as e:   # noqa: BLE001
        print("EXCEPTION: %r" % e)
        check("driver", False, repr(e))
    finally:
        if cap is not None:
            cap.close()
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
    print("\n=== netd Stage-2 selftest: %d/%d passed ===" % (npass, ntot))
    return 0 if npass == ntot and ntot > 0 else 1


def _drain_until(sk, pat, timeout):
    """Read from a socket until `pat` is seen or timeout; return True if seen."""
    buf = ""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            d = sk.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            return False
        if not d:
            return pat in buf
        buf += d.decode("utf-8", "replace")
        if pat in buf:
            return True
    return pat in buf


if __name__ == "__main__":
    sys.exit(main())
