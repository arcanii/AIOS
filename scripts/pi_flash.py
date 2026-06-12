#!/usr/bin/env python3
"""pi_flash.py -- one-command flash-over-network kernel deploy for the RPi4.

Replaces the physical card dance (mount, cp, sha, eject, reinsert, power)
with: push kernel8.img over netconsole -> fatswap rewrites it on the FAT32
boot partition (crash-ordered, see src/fat32.c) -> sha verify -> reboot ->
wait for the board -> confirm the boot banner via /proc/version.

  python3 scripts/pi_flash.py                  # deploy disk/kernel8.img
  python3 scripts/pi_flash.py --build          # ninja -C build-rpi4 + mkkernel8 first
  python3 scripts/pi_flash.py --no-reboot      # stage + swap + verify only

The expected banner is extracted from the kernel8.img BINARY (the baked-in
AIOS_VERSION_FULL string), so the build-number-minus-one and stale-getty-
banner gotchas cannot bite (memory: feedback_kernel8_deploy_verify; the
kernel truth on the Pi is /proc/version, never the getty banner).

Netconsole discipline (memory: project_netconsole): ONE connection for the
whole push+swap+reboot sequence, one fresh connection for the banner check,
generous timeouts (the v0.4.221 kernel can stall 32s+ per Source-B quantum
when a keyboard is attached).
"""
import argparse
import hashlib
import os
import re
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import pi_filexfer as fx   # NC netconsole client (prompt framing, __put)

DEFAULT_KERNEL = os.path.join(REPO, "disk", "kernel8.img")
REMOTE_PATH = "/tmp/kernel8.img"
# Addresses the box has held across DHCP leases (checked after reboot),
# plus the /etc/network.conf static fallback used when boot DHCP fails.
DEFAULT_FALLBACK_HOSTS = ["192.168.0.8", "192.168.0.127", "192.168.0.250",
                          "192.168.0.197"]


def step(msg):
    print("[pi_flash] %s" % msg, flush=True)


def fail(msg):
    print("[pi_flash] FAIL: %s" % msg, flush=True)
    sys.exit(1)


def banner_from_binary(path):
    """The AIOS_VERSION_FULL string baked into the kernel image."""
    with open(path, "rb") as f:
        blob = f.read()
    m = re.search(rb"AIOS v\d+\.\d+\.\d+ \(build \d+\)", blob)
    if not m:
        fail("no AIOS version string inside %s -- wrong file?" % path)
    return m.group(0).decode()


def build_kernel():
    step("building: ninja -C build-rpi4")
    r = subprocess.run(["ninja", "-C", os.path.join(REPO, "build-rpi4")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:] + r.stderr[-2000:])
        fail("ninja -C build-rpi4 failed")
    step("packing: scripts/mkkernel8.py")
    r = subprocess.run([sys.executable,
                        os.path.join(REPO, "scripts", "mkkernel8.py")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:] + r.stderr[-2000:])
        fail("mkkernel8.py failed")


def ping_ok(host, timeout_s=3):
    r = subprocess.run(["ping", "-c", "1", "-t", str(timeout_s), host],
                       capture_output=True)
    return r.returncode == 0


def wait_for_board(host, timeout=300):
    """Reboot recovery: wait for ICMP, then give services time to start.
    Checks the DHCP-fallback candidates too -- the router does not always
    re-issue the same lease across reboots (2026-06-12: .8 became .127).
    Returns the address that answered, or None."""
    cands = [host] + [h for h in (DEFAULT_FALLBACK_HOSTS) if h != host]
    step("waiting for ping on %s (up to %ds)..." % ("/".join(cands), timeout))
    deadline = time.time() + timeout
    while time.time() < deadline:
        for h in cands:
            if ping_ok(h):
                step("ping OK at %s" % h)
                return h
        time.sleep(3)
    return None


def scan_netconsole(prefix, port=2323):
    """Find every host with netconsole (:port) open on the /24. The Pi is the
    only box that listens there, so this beats ping-based discovery -- DHCP
    churn (.8/.127/.197/.250 this session) and decoy hosts that answer ICMP
    but not :2323 (e.g. .197) repeatedly mis-routed the deploy. Returns a list
    of live IPs (usually exactly one: the Pi)."""
    import concurrent.futures as cf

    def probe(ip):
        try:
            s = socket.socket()
            s.settimeout(1.0)
            r = s.connect_ex((ip, port))
            s.close()
            return ip if r == 0 else None
        except OSError:
            return None
    ips = ["%s%d" % (prefix, i) for i in range(2, 255)]
    with cf.ThreadPoolExecutor(max_workers=128) as ex:
        return [r for r in ex.map(probe, ips) if r]


def connect_netconsole(host, attempts=30, gap=15):
    """One careful connection (netconsole serves one client at a time)."""
    last = None
    for i in range(attempts):
        try:
            nc = fx.NC(host)
            nc.expect(b"aios# ", to=150)   # banner can sit behind a quantum
            return nc
        except (OSError, TimeoutError) as e:
            last = e
            time.sleep(gap)
    fail("netconsole did not come back: %s" % last)


def find_pi(host, timeout=420):
    """Locate the Pi's netconsole after a reboot, scan-based (not ping-based,
    which latches onto ICMP-only decoys). Returns the IP, or None. The /24 is
    taken from `host`; falls back to the known lease list inside it."""
    prefix = host.rsplit(".", 1)[0] + "."
    step("scanning %s0/24 for netconsole :2323 (up to %ds)..." % (prefix, timeout))
    deadline = time.time() + timeout
    while time.time() < deadline:
        hits = scan_netconsole(prefix)
        if hits:
            step("netconsole found at %s" % ", ".join(hits))
            return hits[0]
        time.sleep(5)
    return None


def main():
    ap = argparse.ArgumentParser(description="AIOS flash-over-network deploy")
    ap.add_argument("--host", default=fx.DEFAULT_HOST)
    ap.add_argument("--kernel", default=DEFAULT_KERNEL)
    ap.add_argument("--build", action="store_true",
                    help="ninja -C build-rpi4 + mkkernel8.py first")
    ap.add_argument("--no-reboot", action="store_true",
                    help="push + swap + verify, skip reboot/banner check")
    ap.add_argument("--keep-remote", action="store_true",
                    help="leave %s on the Pi" % REMOTE_PATH)
    a = ap.parse_args()

    if a.build:
        build_kernel()
    if not os.path.exists(a.kernel):
        fail("kernel not found: %s (build with --build)" % a.kernel)

    with open(a.kernel, "rb") as f:
        data = f.read()
    local_sha = hashlib.sha256(data).hexdigest()
    expect_banner = banner_from_binary(a.kernel)
    step("kernel: %s (%d bytes)" % (a.kernel, len(data)))
    step("sha256: %s" % local_sha)
    step("banner: %s" % expect_banner)

    # ---- one connection: push + pull-back verify + fatswap (+ reboot) ----
    # Try the given host directly first (no scan -> no wasted connection-1,
    # which matters: netconsole serves one session per boot and a bare scan
    # connect+close can corrupt the next real connection). Only scan if the
    # Pi has moved off a.host.
    host = a.host
    step("connecting to netconsole %s:2323" % host)
    nc = None
    try:
        nc = fx.NC(host)
        nc.expect(b"aios# ", to=150)
    except (OSError, TimeoutError) as e:
        step("%s did not answer (%s); scanning for the Pi..." % (host, e))
        try:
            nc.close()
        except (OSError, AttributeError):
            pass
        host = find_pi(a.host, timeout=60)
        if not host:
            fail("no netconsole on the /24 -- is the Pi up?")
        nc = connect_netconsole(host, attempts=3, gap=5)

    # Verify by PULLING the file back (__get: server-direct, ~1MB/s, no
    # exec) and byte-comparing locally. Faster and safer than an on-Pi
    # sha256sum (minutes of fs IPC + the 30s netconsole command timeout).
    # Retried because the ext2 write path can corrupt a partial-block RMW
    # when a stall quantum hits mid-push (2026-06-12: one 124-byte run --
    # exactly 1024-900 -- clobbered at an ext2 block boundary; the repush
    # in a clean stretch was byte-perfect).
    PUSH_TRIES = 3
    verified = False
    for attempt in range(1, PUSH_TRIES + 1):
        try:
            step("pushing %d bytes -> %s (attempt %d/%d)..."
                 % (len(data), REMOTE_PATH, attempt, PUSH_TRIES))
            # a 129s stall quantum can freeze the receiver while the TCP
            # window is full -- the send timeout must outlast a quantum
            nc.s.settimeout(180)
            t0 = time.time()
            nc.s.sendall(("__put %s %d\n" % (REMOTE_PATH, len(data))).encode())
            nc.s.sendall(data)
            reply = nc.expect(b"aios# ", to=max(300, len(data) // 1500 + 120))
            line = reply.split(b"aios# ")[0].decode("utf-8", "replace").strip()
            if "__put ok" in line:
                step("push done in %.1fs; pulling back to verify..."
                     % (time.time() - t0))
                nc.s.sendall(("__get %s\n" % REMOTE_PATH).encode())
                hdr = nc.expect(b"\n", to=180).decode("utf-8", "replace").strip()
                if not hdr.startswith("__get ok"):
                    raise RuntimeError("pull-back failed: %r" % hdr)
                n = int(hdr.split()[2])
                echoed = nc.read_n(n, to=max(300, n // 100000 + 60))
                nc.expect(b"aios# ", to=120)
                if echoed == data:
                    step("pull-back verified: %d bytes identical" % n)
                    verified = True
                    break
                ndiff = sum(1 for x, y in zip(echoed, data) if x != y) \
                    + abs(len(echoed) - len(data))
                step("pull-back MISMATCH (%d bytes differ) -- stall quantum "
                     "likely hit the fs write path" % ndiff)
            else:
                step("push not acknowledged: %r" % line)
        except (OSError, TimeoutError, RuntimeError, ValueError) as e:
            step("push attempt %d failed mid-stream: %s" % (attempt, e))

        if attempt < PUSH_TRIES:
            # A stalled __put makes netconsole DROP the connection (so the
            # unframed tail cannot run as commands) -- every retry needs a
            # fresh connection, after a polite recovery pause.
            step("reconnecting for retry...")
            try:
                nc.close()
            except OSError:
                pass
            time.sleep(45)
            nc = connect_netconsole(a.host)
    if not verified:
        fail("push never verified after %d attempts" % PUSH_TRIES)

    step("running fatswap (root task rewrites the FAT32 boot partition)...")
    t0 = time.time()
    out = nc.cmd("fatswap %s" % REMOTE_PATH, to=900).decode("utf-8", "replace")
    print(out.rstrip(), flush=True)
    if "fatswap: OK" not in out:
        fail("fatswap did not report OK -- old kernel is still committed "
             "(crash-safe ordering); fix and retry")
    m_src = re.search(r"sha256 src\s*=\s*([0-9a-f]{64})", out)
    m_disk = re.search(r"sha256 disk\s*=\s*([0-9a-f]{64})", out)
    if not m_src or not m_disk:
        fail("could not parse fatswap sha report")
    if m_src.group(1) != local_sha or m_disk.group(1) != local_sha:
        fail("on-card sha mismatch: local=%s src=%s disk=%s"
             % (local_sha, m_src.group(1), m_disk.group(1)))
    step("on-card sha verified (src == disk == local) in %.1fs"
         % (time.time() - t0))

    if not a.keep_remote:
        nc.cmd("rm -f %s" % REMOTE_PATH, to=300)

    if a.no_reboot:
        nc.close()
        step("PASS (staged; --no-reboot: new kernel boots on next reset)")
        return

    step("rebooting the board...")
    try:
        nc.s.sendall(b"reboot\n")
        time.sleep(3)
        nc.s.close()
    except OSError:
        pass

    time.sleep(10)   # let the board actually drop off before scanning
    # Scan for the netconsole port rather than ping: a fresh lease can land
    # anywhere and ICMP-only decoys (.197 this session) mis-route a ping wait.
    back_host = find_pi(a.host, timeout=420)
    if not back_host:
        fail("netconsole did not reappear after reboot -- check serial console")

    step("connecting to netconsole at %s for banner check..." % back_host)
    nc = connect_netconsole(back_host)
    out = nc.cmd("cat /proc/version", to=600).decode("utf-8", "replace")
    nc.close()
    got = out.strip().splitlines()
    got = got[-1].strip() if got else ""
    step("/proc/version: %s" % got)
    if expect_banner in out:
        step("PASS: board is running %s" % expect_banner)
    else:
        fail("banner mismatch: expected %r in /proc/version -- the old "
             "kernel may still be running" % expect_banner)


if __name__ == "__main__":
    main()
