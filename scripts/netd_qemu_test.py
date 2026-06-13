#!/usr/bin/env python3
"""QEMU test for the netd Stage 3 CUTOVER (DESIGN_NETD s3/s8/s10, AIOS_NETD=ON).

Builds a dedicated AIOS_NETD=ON kernel (build-netd/, so build-04 stays flag-OFF),
boots it, and verifies that the net stack now runs in the MMU-isolated netd
process AND that a netd crash is contained + recovered. Everything is driven over
the QEMU SERIAL console -- NOT over netconsole, because the crash demo destroys
the very net path netconsole rides on (s10): netconsole/sshd live in blocking
accept inside netd, so when netd dies the reply-sweep errors them out.

What it asserts (from serial):
  bring-up (s3/s8): netd spawns, self-binds, plat_net_init runs in netd, the root
    listener gets DEVD_READY -> publishes net_ep, netd's own DHCP gets a lease;
  /proc/net (s6): the IPC-free stats page shows a live heartbeat + dev_init_done;
  serverstats: the SRV_NET row is fed by the heartbeat (no SVC_PING to netd);
  crash containment + recovery (s10): cat /proc/netd.crash faults netd; the root
    fault listener logs the fault + runs the reply-slot sweep + clears the IRQ;
    the shell/fs/pipe keep serving (echo round-trips); serverstats renders net
    "dead". A wedged-or-crashed netd never takes the system down.

Per qemu-test-hygiene: PRIVATE disk copies in a tempdir, a test-unique serial
socket, and only this script's own QEMU is torn down.
"""
import importlib.util
import os
import shutil
import socket
import subprocess
import sys
import tempfile
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


def ensure_build():
    """Configure (if needed) + build the AIOS_NETD=ON kernel in build-netd/."""
    if os.path.exists(KERNEL) and os.environ.get("AIOS_NETD_KERNEL"):
        return True
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
    # netd needs a NIC to provision; user-mode net answers DHCP.
    cmd += ["-netdev", "user,id=n0", "-device", "virtio-net-device,netdev=n0"]
    return cmd


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
    sock = None
    logpath = os.path.join(tmp, "serial.log")
    logf = open(logpath, "w")
    try:
        proc = subprocess.Popen(qemu_cmd(disk, logdisk))
        sock = ac.connect_qemu_socket(SOCK, timeout=20)
        sess = ac.Console(sock.fileno(), logfile=logf, echo=False)

        # --- bring-up (s3/s8): netd spawns, READY published, netd does DHCP. Boot
        # is slow under VKA/morecore pressure, so be generous. ---
        print("=== waiting for netd bring-up (boot can take ~120s) ===", flush=True)
        pat, _ = sess.read_until("net_ep published", 240)
        check("netd READY -> net_ep published (s3/s8)", pat is not None)
        pat, _ = sess.read_until(["DHCP: lease acquired", "DHCP result: bound=1"], 90)
        check("netd DHCP lease (net stack runs in netd)", pat is not None)

        # --- login over serial. Let boot FULLY settle first: while the system is
        # under VKA/morecore pressure, pipe_server prints "[pipe] SLOW msg" lines
        # that interleave with -- and split -- getty's "Password:" prompt (the
        # documented serial-login fragility). Drain ~30s so IPCs are fast + the
        # prompt is contiguous, then log in (retry once for a stray tlbi line). ---
        sess.read_until("\x00settle\x00", 30)   # drain; no match -> just waits
        logged_in = False
        for _attempt in range(3):
            sess.buf = ""
            try:
                sess.ensure_shell("root", "root", 60, nudge=True, settle=1.0)
                logged_in = True
                break
            except (TimeoutError, RuntimeError):
                time.sleep(3)
        check("serial login (post-settle)", logged_in)
        if not logged_in:
            raise RuntimeError("serial login failed after retries")

        # --- /proc/net (s6): the IPC-free heartbeat page is live ---
        out = sess.run("cat /proc/net", 15)
        check("/proc/net heartbeat live (s6)",
              "heartbeat:" in out and "dev_init_done: 1" in out,
              "" if "heartbeat:" in out else "(no heartbeat line)")

        # --- serverstats: SRV_NET fed by the heartbeat, shown ok pre-crash ---
        out = sess.run("cat /proc/serverstats", 15)
        net_row = [ln for ln in out.splitlines() if ln.startswith("net ")]
        pre_ok = bool(net_row) and (" ok " in net_row[0] or net_row[0].split()[1] == "ok")
        check("serverstats net ok pre-crash", pre_ok,
              net_row[0] if net_row else "(no net row)")

        # --- crash containment + recovery (s10). The fault + sweep happen so fast
        # that the listener's serial output INTERLEAVES with the cat command's own
        # output, so per-command substring matching is unreliable -- assert against
        # the full captured serial log instead. ---
        sess.run("cat /proc/netd.crash", 15)       # trigger (output interleaves)
        sess.read_until("\x00drain\x00", 8)         # let the listener finish printing
        logf.flush()
        with open(logpath) as f:
            full = f.read()
        check("crash trigger reached netd", "NET_DIAG crash trigger" in full)
        check("netd fault contained (s10)", "netd-listener] FAULT" in full)
        check("reply-slot sweep ran + IRQ cleared (s10)",
              "swept" in full and "IRQ cleared" in full)

        # shell/fs/pipe still serve after netd died
        token = "NETD_ALIVE_%d" % os.getpid()
        out = sess.run("echo %s" % token, 15)
        check("system alive after netd crash (shell/fs/pipe)", token in out)

        # serverstats renders the net row dead (net_ep_cap zeroed by the listener)
        out = sess.run("cat /proc/serverstats", 15)
        net_row = [ln for ln in out.splitlines() if ln.startswith("net ")]
        dead = bool(net_row) and "dead" in net_row[0]
        check("serverstats net dead post-crash", dead,
              net_row[0] if net_row else "(no net row)")

    except Exception as e:   # noqa: BLE001
        print("EXCEPTION: %r" % e)
        check("driver", False, repr(e))
    finally:
        try:
            logf.close()
        except OSError:
            pass
        if sock is not None:
            try:
                sock.close()
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
        print("serial log: %s" % os.path.join(tmp, "serial.log"))
        # keep the tmp serial log on failure for postmortem; clean on success
        npass_ = sum(1 for _, ok, _ in results if ok)
        if npass_ == len(results) and results:
            shutil.rmtree(tmp, ignore_errors=True)

    npass = sum(1 for _, ok, _ in results if ok)
    ntot = len(results)
    print("\n=== netd Stage-3 cutover test: %d/%d passed ===" % (npass, ntot))
    return 0 if npass == ntot and ntot > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
