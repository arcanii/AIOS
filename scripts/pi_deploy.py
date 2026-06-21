#!/usr/bin/env python3
"""pi_deploy.py -- robust, ATOMIC deploy of a disk binary to the AIOS Pi.

Why this exists (lesson learned 2026-06-19, painfully): deploying a disk binary
with `cp /tmp/x /bin/x` over netconsole is STALL-VULNERABLE. The cp is a FORKED
command; the ~33s idle-teardown TLBI freeze hits it, netconsole's 30s command
timeout SIGKILLs it mid-write, and /bin/x is left PARTIAL. getty's respawn-
supervisor then respawns the partial service -> a corrupt binary that accepts
connections but whose forked-command path is broken. Recovering from that needs
serial or the non-forking __put/__get.

This tool avoids all of that:
  1. __put the bytes to <target>.tmp  -- netconsole's __put handler is NON-FORKING,
     so it is NOT subject to the 30s forked-command SIGKILL. Retried until "__put
     ok"; a stall that aborts a __put just means a retry (the .tmp, not the live
     service path, is what's partial -- harmless, getty never spawns .tmp).
  2. __get <target>.tmp back and byte-compare (sha256) to the local source --
     also non-forking. The deploy does not proceed unless this matches.
  3. `mv <target>.tmp <target>` -- the ONLY forked step, but rename(2) is ATOMIC,
     so <target> is always either the OLD intact binary or the NEW intact binary,
     NEVER partial. Retried + re-verified via __get until <target> == source.

So the live service path is never left partial, even mid-stall. After a verified
deploy, restart the service (kill -> getty respawns) or --reboot.

Usage:
  pi_deploy.py [--host H] <local-file> <remote-path>
  pi_deploy.py --host 192.168.0.8 build-04/sbase/netconsole /bin/netconsole
  pi_deploy.py ... --reboot      # reboot after a verified deploy (e.g. respawn a service)
"""
import argparse
import hashlib
import socket
import sys
import time

DEFAULT_HOST = "192.168.0.8"
PORT = 2323
PROMPT = b"aios# "


def _connect(host, to=15):
    s = socket.create_connection((host, PORT), timeout=to)
    s.settimeout(2.0)
    buf = b""
    # Wait out a possible ~33s freeze for the banner+prompt (continue, do NOT
    # break on the first recv timeout -- a frozen/slow box would otherwise leave
    # the prompt unconsumed and desync the next command).
    dl = time.time() + 50
    while PROMPT not in buf and time.time() < dl:
        try:
            buf += s.recv(4096)
        except socket.timeout:
            continue
    return s


def put(host, data, remote, tries=5, settle=6):
    """Non-forking __put of `data` -> remote. Retries until '__put ok'."""
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


def get(host, remote, tries=5, settle=4):
    """Non-forking __get of remote -> bytes (or None). For a real file (not a
    0-size procfs entry)."""
    for a in range(tries):
        try:
            s = _connect(host)
            s.settimeout(2.0)
            s.sendall(("__get %s\n" % remote).encode())
            hdr = b""
            # Header "__get ok <len>\n" may sit behind a ~33s freeze; wait it out
            # (continue, not break) up to a freeze-spanning deadline.
            hdl = time.time() + 50
            while b"\n" not in hdr and len(hdr) < 64 and time.time() < hdl:
                try:
                    hdr += s.recv(1)
                except socket.timeout:
                    continue
            h = hdr.decode("utf-8", "replace").strip()
            if not h.startswith("__get ok"):
                s.close()
                time.sleep(settle)
                continue
            n = int(h.split()[2])
            got = b""
            dl = time.time() + max(60, n // 1000 + 30)
            # Read EXACTLY n bytes -- never over-read past the declared length.
            # After the n raw bytes, the netconsole serve loop sends the next 6-byte
            # prompt "aios# "; a plain recv(65536) would swallow it into `got`,
            # making len(got) == n+6 so the `len(got) == n` check below spuriously
            # fails (a correct transfer reported as a sha/length mismatch).
            while len(got) < n and time.time() < dl:
                try:
                    chunk = s.recv(min(65536, n - len(got)))
                    if not chunk:
                        break                       # peer closed mid-stream
                    got += chunk
                except socket.timeout:
                    continue
            s.close()
            if len(got) == n:
                return got
        except (OSError, ValueError) as e:
            print("  __get try %d err: %s" % (a, e))
            time.sleep(settle)
    return None


def runcmd(host, cmd, to=40, tries=3, settle=5):
    """A FORKED command (mv/reboot). Atomic ops only -- retried; output or None."""
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


def deploy(host, local, remote, reboot=False):
    data = open(local, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    tmp = remote + ".tmp"
    print("deploy %s (%d bytes, sha %s) -> %s" % (local, len(data), sha[:16], remote))

    # 1. non-forking __put to the .tmp staging path
    if not put(host, data, tmp):
        print("FAIL: __put -> %s" % tmp)
        return False
    print("  staged %s via __put" % tmp)

    # 2. verify the staged copy byte-exact (non-forking __get)
    back = get(host, tmp)
    if back is None or hashlib.sha256(back).hexdigest() != sha:
        print("FAIL: verify %s (no read / sha mismatch)" % tmp)
        return False
    print("  verified %s byte-exact" % tmp)

    # 3. atomic rename .tmp -> target (forked mv, but rename(2) is atomic, so the
    #    live path is never partial); retry + re-verify the TARGET until it == src
    for a in range(5):
        runcmd(host, "mv %s %s" % (tmp, remote))
        fin = get(host, remote)
        if fin is not None and hashlib.sha256(fin).hexdigest() == sha:
            print("  %s == source, byte-exact VERIFIED" % remote)
            if reboot:
                print("  rebooting (to respawn the new binary)...")
                runcmd(host, "reboot", to=8)
            print("DEPLOY OK")
            return True
        print("  rename retry %d (target not yet new)" % a)
        time.sleep(4)
    print("FAIL: rename/verify %s" % remote)
    return False


def main():
    ap = argparse.ArgumentParser(description="robust atomic disk-binary deploy over netconsole")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("local", help="local file to deploy")
    ap.add_argument("remote", help="remote path, e.g. /bin/netconsole")
    ap.add_argument("--reboot", action="store_true",
                    help="reboot after a verified deploy (e.g. to respawn a service)")
    a = ap.parse_args()
    sys.exit(0 if deploy(a.host, a.local, a.remote, a.reboot) else 1)


if __name__ == "__main__":
    main()
