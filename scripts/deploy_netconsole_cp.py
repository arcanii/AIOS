#!/usr/bin/env python3
"""deploy_netconsole_cp.py -- install a new /bin/netconsole over the network without
the rename(2)-over-existing EPERM that breaks pi_deploy.py on the AIOS fs.

Steps (all retried, stall-tolerant):
  1. __put the bytes to /tmp/netconsole.new   (non-forking -> not subject to the
     30s forked-command SIGKILL; a stall just means a retry).
  2. __get /tmp/netconsole.new and verify sha256 (integrity before we touch /bin).
  3. `cp /tmp/netconsole.new /bin/netconsole` (FORKED -- run when the board is calm,
     e.g. right after a reboot before any idle, so a stall cannot kill it mid-write).
  4. __get /bin/netconsole and verify sha256 == source (the live binary is correct).

Usage: python3 scripts/deploy_netconsole_cp.py [--host H] [local=build-04/sbase/netconsole]
"""
import sys, os, time, socket, hashlib, argparse

PORT = 2323
PROMPT = b"aios#"


def _connect(host, to=15):
    s = socket.create_connection((host, PORT), timeout=to)
    s.settimeout(2.0)
    buf = b""
    dl = time.time() + 50
    while PROMPT not in buf and time.time() < dl:
        try:
            buf += s.recv(4096)
        except socket.timeout:
            continue
    return s


def put(host, data, remote, tries=6, settle=6):
    for a in range(tries):
        try:
            s = _connect(host)
            s.settimeout(180)
            s.sendall(("__put %s %d\n" % (remote, len(data))).encode())
            s.sendall(data)
            buf = b""
            dl = time.time() + max(180, len(data) // 1000 + 60)
            while b"__put" not in buf and time.time() < dl:
                try:
                    buf += s.recv(4096)
                except socket.timeout:
                    continue
            s.close()
            if b"__put ok" in buf:
                return True
            print("  __put try %d: %r" % (a, buf.decode("utf-8", "replace").strip()[:60]))
        except OSError as e:
            print("  __put try %d err: %s" % (a, e))
        time.sleep(settle)
    return False


def get(host, remote, tries=6, settle=4):
    for a in range(tries):
        try:
            s = _connect(host)
            s.settimeout(2.0)
            s.sendall(("__get %s\n" % remote).encode())
            hdr = b""
            hdl = time.time() + 50
            while b"\n" not in hdr and len(hdr) < 64 and time.time() < hdl:
                try:
                    hdr += s.recv(1)
                except socket.timeout:
                    continue
            h = hdr.decode("utf-8", "replace").strip()
            if not h.startswith("__get ok"):
                s.close(); time.sleep(settle); continue
            n = int(h.split()[2])
            got = b""
            dl = time.time() + max(60, n // 1000 + 30)
            while len(got) < n and time.time() < dl:
                try:
                    chunk = s.recv(min(65536, n - len(got)))
                    if not chunk:
                        break
                    got += chunk
                except socket.timeout:
                    continue
            s.close()
            if len(got) == n:
                return got
        except (OSError, ValueError) as e:
            print("  __get try %d err: %s" % (a, e)); time.sleep(settle)
    return None


def runcmd(host, cmd, to=40, tries=3, settle=5):
    for a in range(tries):
        try:
            s = _connect(host)
            s.sendall((cmd + "\n").encode())
            s.settimeout(2.0)
            buf = b""
            dl = time.time() + to
            while PROMPT not in buf and time.time() < dl:
                try:
                    buf += s.recv(4096)
                except socket.timeout:
                    continue
            s.close()
            if PROMPT in buf:
                return buf.split(PROMPT)[0].decode("utf-8", "replace")
        except OSError:
            pass
        time.sleep(settle)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.0.8")
    ap.add_argument("local", nargs="?", default="build-04/sbase/netconsole")
    a = ap.parse_args()

    data = open(a.local, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    print("netconsole %s: %d bytes sha %s" % (a.local, len(data), sha[:16]))

    print("1. __put -> /tmp/netconsole.new")
    if not put(a.host, data, "/tmp/netconsole.new"):
        print("FAIL: __put"); return 1
    print("2. verify /tmp staging sha")
    g = get(a.host, "/tmp/netconsole.new")
    if not g or hashlib.sha256(g).hexdigest() != sha:
        print("FAIL: staging sha mismatch (got %s)" % (hashlib.sha256(g).hexdigest()[:16] if g else "none")); return 1
    print("   OK staging matches")
    print("3. cp /tmp/netconsole.new -> /bin/netconsole")
    out = runcmd(a.host, "cp /tmp/netconsole.new /bin/netconsole")
    print("   cp out: %r" % (out.strip()[:60] if out else None))
    print("4. verify live /bin/netconsole sha")
    g2 = get(a.host, "/bin/netconsole")
    if not g2 or hashlib.sha256(g2).hexdigest() != sha:
        print("FAIL: /bin/netconsole sha mismatch (got %s) -- live binary NOT updated"
              % (hashlib.sha256(g2).hexdigest()[:16] if g2 else "none")); return 1
    print("   OK: /bin/netconsole now matches (%s). Reboot to respawn netconsole." % sha[:16])
    return 0


if __name__ == "__main__":
    sys.exit(main())
