#!/usr/bin/env python3
"""QEMU socket-API regression suite for netd Stage 0 (DESIGN_NETD s9 item 6).

Boots AIOS under QEMU with user-mode (SLIRP) networking and drives the
guest-side `nettest` tool through the client socket paths that had no automated
coverage before (the existing net scripts are all listen/accept-side):

  - outbound TCP connect + echo round-trip
  - ECONNREFUSED on a RST (connect to a closed host port)
  - UDP send + echo recv
  - close() the same fd twice (robustness)
  - parked recv + peer RST  -> ECONNRESET   (the RST reply-slot poisoning fix:
        without it the parked recv never wakes and the guest hangs here)
  - op-98 process-exit cleanup: a child opens every free socket slot and exits
        WITHOUT closing; the slots must be reclaimed (count recovers)

Driving: commands run over the auto-started netconsole (a root shell on TCP
2323, reached via a SLIRP hostfwd) rather than the serial console -- this avoids
the boot-log/getty interleave that splits the serial password prompt, and it
additionally exercises fork/exec + the op-98 cleanup-proxy path on every
command. The guest reaches the host echo/RST/UDP servers via the SLIRP alias
10.0.2.2.

Per qemu-test-hygiene: boots PRIVATE copies of the disk images in a tempdir,
uses a test-unique serial socket + host ports, and tears down only its own QEMU
process. Requires a disk built with `nettest` on it.
"""
import importlib.util
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "build-04/images/aios_root-image-arm-qemu-arm-virt")
DISK = os.path.join(REPO, "disk/disk_ext2.img")
LOGDISK = os.path.join(REPO, "disk/log_ext2.img")

spec = importlib.util.spec_from_file_location(
    "aios_console", os.path.join(REPO, "scripts/aios_console.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)

SOCK = "/tmp/aios-netsock-%d.sock" % os.getpid()
PORT_BASE = 35000 + (os.getpid() % 1500)
P_ECHO = PORT_BASE + 0       # host TCP echo server
P_UDP = PORT_BASE + 1        # host UDP echo server
P_RST = PORT_BASE + 2        # host TCP server that RSTs a parked reader
P_REFUSED = PORT_BASE + 3    # deliberately unbound -> connect is refused
P_NETCON = PORT_BASE + 4     # host-side forwarded port -> guest netconsole 2323
P_BULK = PORT_BASE + 5       # host TCP server that streams a large blob (burst RX)
BULK_BYTES = 200000          # ~140 frames; exceeds the 32KB rx window + HW ring

GUEST_HOST = "10.0.2.2"      # SLIRP alias for the host, as seen by the guest
BIND = "127.0.0.1"


# ---- host-side helper servers (daemon threads) ----------------------------

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
    c.settimeout(5.0)
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


def tcp_rst_server(stop):
    """Accept, let the guest park in read(), then send a RST (linger 0 close)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((BIND, P_RST)); s.listen(8); s.settimeout(0.5)
    while not stop.is_set():
        try:
            c, _ = s.accept()
        except socket.timeout:
            continue
        # Let the guest reliably issue read() and block in the server before we
        # RST. Generous (the guest can stall a few seconds between connect and
        # read under a slow boot); if the RST wins the race the guest's read
        # hits a reset socket (-1/EPERM) instead of exercising the wake path.
        time.sleep(4.0)
        c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        c.close()
    s.close()


def tcp_bulk_server(stop):
    """Accept, stream BULK_BYTES, then FIN -- a burst RX stress for the guest."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((BIND, P_BULK)); s.listen(8); s.settimeout(0.5)
    blob = bytes((i * 7) & 0xFF for i in range(4096))
    while not stop.is_set():
        try:
            c, _ = s.accept()
        except socket.timeout:
            continue
        try:
            sent = 0
            while sent < BULK_BYTES:
                chunk = min(len(blob), BULK_BYTES - sent)
                c.sendall(blob[:chunk])
                sent += chunk
        except OSError:
            pass
        finally:
            c.close()   # FIN -> guest read() sees EOF
    s.close()


def udp_echo_server(stop):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((BIND, P_UDP)); s.settimeout(0.5)
    while not stop.is_set():
        try:
            d, addr = s.recvfrom(2048)
        except socket.timeout:
            continue
        try:
            s.sendto(d, addr)
        except OSError:
            pass
    s.close()


# ---- netconsole TCP client (persistent "aios# " shell) --------------------

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
        """Send a command, return the output up to the next prompt."""
        self.s.sendall((cmd + "\n").encode())
        out = self.expect("aios# ", timeout)
        return out

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
    # hostfwd P_NETCON -> guest 2323 (netconsole, host->guest). Outbound guest
    # connections reach the host echo/RST/UDP servers via 10.0.2.2 (no fwd).
    cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:2323" % P_NETCON,
            "-device", "virtio-net-device,netdev=n0"]
    return cmd


def grab(out, pat):
    m = re.search(pat, out)
    return int(m.group(1)) if m else None


def main():
    if not os.path.exists(KERNEL):
        print("FAIL: kernel not found: %s" % KERNEL); return 2
    if not os.path.exists(DISK):
        print("FAIL: disk not found: %s (run build_apps.py first)" % DISK); return 2

    tmp = tempfile.mkdtemp(prefix="aios-netsock-")
    disk = os.path.join(tmp, "disk.img"); shutil.copy(DISK, disk)
    logdisk = os.path.join(tmp, "log.img")
    if os.path.exists(LOGDISK):
        shutil.copy(LOGDISK, logdisk)
    else:
        logdisk = None

    stop = threading.Event()
    for fn in (tcp_echo_server, tcp_rst_server, udp_echo_server, tcp_bulk_server):
        threading.Thread(target=fn, args=(stop,), daemon=True).start()
    time.sleep(0.3)

    if os.path.exists(SOCK):
        os.unlink(SOCK)

    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)

    proc = None
    serial = None
    nc = None
    try:
        proc = subprocess.Popen(qemu_cmd(disk, logdisk))
        # Keep the serial socket drained so the guest's TTY never blocks on a
        # full pipe; we drive everything over netconsole.
        serial = ac.connect_qemu_socket(SOCK, timeout=20)
        threading.Thread(target=_drain, args=(serial, stop), daemon=True).start()

        # netconsole auto-starts at boot (getty forks it). Poll the forwarded
        # port until it answers -- this also waits out the (slow) boot.
        print("=== waiting for netconsole (boot can take ~90s) ===", flush=True)
        deadline = time.time() + 240   # boot is slow under VKA/morecore pressure
        while time.time() < deadline:
            try:
                nc = Netcon("127.0.0.1", P_NETCON)
                nc.expect("aios# ", timeout=8)   # banner + first prompt
                break
            except (OSError, TimeoutError, EOFError):
                nc = None
                time.sleep(2.0)
        check("netconsole reachable (boot ok)", nc is not None)
        if nc is None:
            return 1

        # 1. outbound TCP connect + echo (also the net-up connectivity gate).
        out = ""
        for _ in range(4):
            out = nc.run("nettest connect %s %d" % (GUEST_HOST, P_ECHO), 20)
            if "NETTEST connect PASS" in out:
                break
            time.sleep(1.0)
        net_up = "NETTEST connect PASS" in out
        check("connect", net_up, "" if net_up else repr(out.replace("aios# ", "").strip()[:120]))

        if not net_up:
            for n in ("refused", "udp", "recvrst", "dblclose", "bulk_burst",
                      "op98_cleanup"):
                check(n, False, "(skipped: no outbound connectivity)")
        else:
            out = nc.run("nettest refused %s %d" % (GUEST_HOST, P_REFUSED), 20)
            check("refused", "NETTEST refused PASS" in out,
                  "" if "PASS" in out else repr(out.replace("aios# ", "").strip()[:120]))

            out = nc.run("nettest udp %s %d" % (GUEST_HOST, P_UDP), 20)
            check("udp", "NETTEST udp PASS" in out,
                  "" if "PASS" in out else repr(out.replace("aios# ", "").strip()[:120]))

            # parked recv + peer RST -> ECONNRESET (the Stage-0 fix). A missing
            # fix hangs read() forever; the 25s timeout then surfaces as a FAIL.
            out = nc.run("nettest recvrst %s %d" % (GUEST_HOST, P_RST), 25)
            check("recvrst", "NETTEST recvrst PASS" in out,
                  "" if "PASS" in out else repr(out.replace("aios# ", "").strip()[:120]))

            out = nc.run("nettest dblclose %s %d" % (GUEST_HOST, P_ECHO), 20)
            check("dblclose", "NETTEST dblclose PASS" in out,
                  "" if "PASS" in out else repr(out.replace("aios# ", "").strip()[:120]))

            # burst RX (v0.4.230 Stage 1): receive a large stream. TCP must
            # deliver every byte (peer retransmit covers any HW-ring drop); this
            # stresses the merged plat_net_drain() NAPI re-check under sustained
            # RX where the ring fills faster than one read() drains the socket.
            out = nc.run("nettest bulk %s %d" % (GUEST_HOST, P_BULK), 45)
            m = re.search(r"NETTEST bulk recv=(\d+)", out)
            rb = int(m.group(1)) if m else -1
            check("bulk_burst", rb == BULK_BYTES, "(recv=%d/%d)" % (rb, BULK_BYTES))

            # op-98 cleanup: baseline free slots, leak them all in a child that
            # exits, confirm the count recovers (proxy-driven NET_CLEANUP_PID).
            c0 = grab(nc.run("nettest count", 15), r"free=(\d+)")
            h = grab(nc.run("nettest hog", 15), r"opened=(\d+)")
            c1 = None
            for _ in range(5):
                c1 = grab(nc.run("nettest count", 15), r"free=(\d+)")
                if c1 is not None and c0 is not None and c1 >= c0:
                    break
                time.sleep(0.6)
            ok = (c0 and h is not None and c1 is not None and c0 >= 1
                  and h == c0 and c1 == c0)
            check("op98_cleanup", ok, "(free0=%s hog=%s free1=%s)" % (c0, h, c1))

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
    print("\n=== net socket suite: %d/%d passed ===" % (npass, ntot))
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
