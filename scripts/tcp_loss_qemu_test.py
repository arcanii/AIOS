#!/usr/bin/env python3
"""QEMU LOSSY TCP regression test for the v0.4.253 sender-retransmit / graceful
close (commit 3e3e26a) and its HW regression.

WHY THIS EXISTS: QEMU SLIRP is lossless + instant, so the FULL socket/ssh/netd
suites pass while NEVER exercising the retransmit / deferred-close / RTO give-up
paths. Commit 3e3e26a passed all of them and then RST-regressed the real Pi
network (SSH 0/20, netconsole "Connection reset by peer"). This test injects the
missing loss via two new /proc/netstat knobs so the close machinery actually runs
on QEMU (see feedback_qemu_cannot_model_loss):

  /proc/netstat.ackdrop.1  -- drop EVERY inbound pure-ACK. A guest socket that
        sends data + close() then never sees the peer ACK its FIN -> snd_una
        stalls -> the deferred close cannot complete -> the RTO give-up fires.
        This deterministically reproduces the give-up path that, in the broken
        code, sends a TCP_RST (tcp_rst_sent climbs) and poisons the slot.
  /proc/netstat.txdrop.N   -- drop every Nth OUTBOUND data/FIN segment, so the
        SENDER retransmit path runs (the feature must still deliver the data).

ASSERTIONS:
  R1 no-loss baseline: nettest connect echo round-trips; ZERO give-ups/RSTs.
  R2 give-up sends NO RST: under ackdrop, a guest close() hits the give-up, but
        the FIXED code frees the slot SILENTLY (tcp_rst_sent == 0). The BROKEN
        code sends a RST here (tcp_rst_sent > 0) -- that RST is what discards the
        peer's unread data on real HW. This is the core regression assertion.
  R3 retransmit delivers: under a bounded txdrop, the dropped data segment is
        retransmitted and the echo still round-trips (the feature works).

Run BROKEN-code-first to confirm the repro (R2 FAILs: rst_sent>0), then after the
fix (R2 PASSes: rst_sent==0). Per qemu-test-hygiene: private disk copies, a
test-unique serial socket + host ports, tears down only its own QEMU.

  AIOS_KERNEL=build-netd/images/...  (default; the Pi runs AIOS_NETD=ON)
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
    "AIOS_KERNEL",
    os.path.join(REPO, "build-netd/images/aios_root-image-arm-qemu-arm-virt"))
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)

SOCK = "/tmp/aios-tcploss-%d.sock" % os.getpid()
PORT_BASE = 36500 + (os.getpid() % 1200)
P_ECHO = PORT_BASE + 0
P_NETCON = PORT_BASE + 4
GUEST_HOST = "10.0.2.2"
BIND = "127.0.0.1"


def tcp_echo_server(stop):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((BIND, P_ECHO)); s.listen(8); s.settimeout(0.5)
    while not stop.is_set():
        try:
            c, _ = s.accept()
        except socket.timeout:
            continue
        threading.Thread(target=_echo_conn, args=(c,), daemon=True).start()
    s.close()


def _echo_conn(c):
    c.settimeout(6.0)
    try:
        while True:
            d = c.recv(1024)
            if not d:
                break
            c.sendall(d)
    except OSError:
        pass
    finally:
        c.close()


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

    def run(self, cmd, timeout=25):
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


def stat(nc, key):
    """Read one /proc/netstat counter."""
    out = nc.run("cat /proc/netstat", 15)
    m = re.search(r"^%s:\s*(\d+)" % re.escape(key), out, re.M)
    return int(m.group(1)) if m else None


def main():
    if not os.path.exists(KERNEL):
        print("FAIL: kernel not found: %s" % KERNEL); return 2
    if not os.path.exists(DISK):
        print("FAIL: disk not found: %s (run build_apps.py first)" % DISK); return 2

    tmp = tempfile.mkdtemp(prefix="aios-tcploss-")
    disk = os.path.join(tmp, "disk.img"); shutil.copy(DISK, disk)
    logdisk = os.path.join(tmp, "log.img")
    if os.path.exists(LOGDISK):
        shutil.copy(LOGDISK, logdisk)
    else:
        logdisk = None

    stop = threading.Event()
    threading.Thread(target=tcp_echo_server, args=(stop,), daemon=True).start()
    time.sleep(0.3)
    if os.path.exists(SOCK):
        os.unlink(SOCK)

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

        # R1: no-loss baseline -- connectivity + zero give-ups/RSTs.
        out = ""
        for _ in range(4):
            out = nc.run("nettest connect %s %d" % (GUEST_HOST, P_ECHO), 20)
            if "NETTEST connect PASS" in out:
                break
            time.sleep(1.0)
        net_up = "NETTEST connect PASS" in out
        check("R1a connect (baseline)", net_up,
              "" if net_up else repr(out.replace("aios# ", "").strip()[:120]))
        if not net_up:
            for n in ("R1b no give-ups", "R2 give-up sends NO RST", "R3 retransmit delivers"):
                check(n, False, "(skipped: no connectivity)")
            return 1

        rst0 = stat(nc, "tcp_rst_sent")
        give0 = stat(nc, "tcp_giveups")
        check("R1b no give-ups/RST at baseline", rst0 == 0 and give0 == 0,
              "(rst_sent=%s giveups=%s)" % (rst0, give0))

        # R2 (the core regression assertion): force a give-up by dropping our FIN
        # so the close can never complete. Data (PING) still flows, so the guest
        # does not hang in read(); only the FIN is lost -> the RTO retransmits it,
        # then gives up. Broken code: the give-up sends a TCP_RST (rst_sent climbs
        # -- the RST that discards the client's unread data). Fixed code: the
        # give-up frees the slot SILENTLY (rst_sent stays 0).
        nc.run("cat /proc/netstat.findrop.30", 10)  # drop our next 30 FINs (echo conn only)
        # closelinger does connect+echo+close then sleeps ~14s FOREGROUND, so the
        # server-side socket LINGERS (no process-exit reap) and the RTO give-up
        # fires DURING the sleep (rtx_count hits TCP_RTX_MAX(8) at ~8s). The
        # netconsole shell is blocked for the duration, then returns.
        print("=== running closelinger (~14s; give-up fires mid-sleep) ===", flush=True)
        nc.run("nettest closelinger %s %d" % (GUEST_HOST, P_ECHO), 30)
        nc.run("cat /proc/netstat.findrop.0", 10)   # restore
        give1 = stat(nc, "tcp_giveups")
        rst1 = stat(nc, "tcp_rst_sent")
        reproduced_giveup = give1 is not None and give1 > (give0 or 0)
        check("R2a give-up path was exercised (FIN drop)", reproduced_giveup,
              "(giveups %s -> %s)" % (give0, give1))
        # THE fix assertion: the give-up must NOT send a RST.
        no_rst = rst1 == 0
        check("R2b give-up sends NO RST (fixed) ", no_rst,
              "(rst_sent=%s -- >0 means the broken RST-on-give-up is present)" % rst1)

        # R3: drop the next outbound DATA segment (the PING). The guest read()
        # blocks ~1 RTO until the retransmit redelivers it; the echo then still
        # round-trips (proves the retransmit feature actually delivers lost data).
        rtx0 = stat(nc, "tcp_rtx_segs")
        nc.run("cat /proc/netstat.txdrop.1", 10)     # drop the next 1 outbound DATA seg
        out = nc.run("nettest connect %s %d" % (GUEST_HOST, P_ECHO), 25)
        nc.run("cat /proc/netstat.txdrop.0", 10)
        rtx1 = stat(nc, "tcp_rtx_segs")
        delivered = "NETTEST connect PASS" in out
        retransmitted = rtx1 is not None and rtx0 is not None and rtx1 > rtx0
        check("R3 retransmit delivers under loss", delivered and retransmitted,
              "(echo %s, rtx_segs %s -> %s)" %
              ("PASS" if delivered else "FAIL", rtx0, rtx1))

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
    print("\n=== tcp loss suite: %d/%d passed ===" % (npass, ntot))
    return 0 if npass == ntot and ntot > 0 else 1


def _drain(sk, stop):
    sk.settimeout(0.5)
    while not stop.is_set():
        try:
            if not sk.recv(4096):
                return
        except (socket.timeout, OSError):
            continue


if __name__ == "__main__":
    sys.exit(main())
