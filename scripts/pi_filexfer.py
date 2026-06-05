#!/usr/bin/env python3
"""Pull files off an AIOS Pi over netconsole (no crypto, LAN/dev only).

netconsole (port 2323) runs one `dash -c "<line>"` per socket line. This pulls
a file binary-safe by LENGTH FRAMING: ask the Pi for the size (`wc -c`) and a
`sha256sum`, then `cat` the file and read exactly that many bytes off the socket
(so binary content cannot collide with the `aios# ` prompt). The sha256 is
re-checked on the Mac. Uses only tools the Pi already has -- no reflash.

LIMITATION: practical for small/medium files (configs, logs, /proc, source).
The big ~300KB static ELFs in /bin are slow to read on AIOS (fs/block layer +
non-cacheable pipe SHM) and exceed netconsole's per-command timeout (~30s).
Push (Mac -> Pi) and large/fast transfer are follow-ups (see NEXT_20260604c.md).

Usage:  python3 scripts/pi_filexfer.py pull <remote-path> <local-path> [host]
"""
import hashlib, socket, sys, time

DEFAULT_HOST, PORT = "192.168.0.8", 2323

class NC:
    def __init__(self, host):
        self.s = socket.create_connection((host, PORT), timeout=10)
        self.s.settimeout(2.0); self.buf = b""
    def _fill(self, dl):
        if time.time() > dl:
            raise TimeoutError("netconsole timeout (%d bytes buffered)" % len(self.buf))
        try:
            self.buf += self.s.recv(65536)
        except socket.timeout:
            pass
    def expect(self, pat, to=60):
        dl = time.time() + to
        while True:
            i = self.buf.find(pat)
            if i != -1:
                r = self.buf[:i + len(pat)]; self.buf = self.buf[i + len(pat):]
                return r
            self._fill(dl)
    def cmd(self, c, to=60):
        self.s.sendall((c + "\n").encode())
        return self.expect(b"aios# ", to).split(b"aios# ")[0]
    def read_n(self, n, to=120):
        dl = time.time() + to
        while len(self.buf) < n:
            self._fill(dl)
        r = self.buf[:n]; self.buf = self.buf[n:]; return r
    def close(self):
        try:
            self.s.sendall(b"exit\n"); self.s.close()
        except OSError:
            pass

def pull(remote, local, host=DEFAULT_HOST):
    nc = NC(host); nc.expect(b"aios# ")
    # sha256sum for integrity (fast -- tiny output; the bottleneck was never the
    # file read, it was cat's small-chunk pipe writes, which __get avoids).
    sout = nc.cmd("sha256sum %s" % remote).split()
    sha = sout[0].decode() if sout and len(sout[0]) == 64 else None
    t0 = time.time()
    # __get: netconsole reads the file directly and streams it length-framed --
    # "__get ok <len>\n" + <len> bytes, or "__get err <reason>\n".
    nc.s.sendall(("__get %s\n" % remote).encode())
    hdr = nc.expect(b"\n").decode("utf-8", "replace").strip()
    if not hdr.startswith("__get ok"):
        nc.close(); print("pull failed: %r" % hdr); return False
    n = int(hdr.split()[2])
    try:
        data = nc.read_n(n, to=max(30, n // 2000 + 30))
    except TimeoutError as e:
        nc.close(); print("pull failed: %s" % e); return False
    nc.expect(b"aios# "); nc.close()
    dt = time.time() - t0
    mac = hashlib.sha256(data).hexdigest()
    ok = (len(data) == n) and (sha is None or mac == sha)
    with open(local, "wb") as f:
        f.write(data)
    print("pulled %d bytes -> %s in %.1fs (%.1f KB/s)   INTEGRITY: %s" %
          (len(data), local, dt, len(data) / max(dt, 0.01) / 1024,
           "OK" if ok else ("MISMATCH (%s)" % mac if sha else "unverified")))
    return ok

def push(local, remote, host=DEFAULT_HOST):
    with open(local, "rb") as f:
        data = f.read()
    sha = hashlib.sha256(data).hexdigest()
    nc = NC(host); nc.expect(b"aios# ")
    nc.s.settimeout(60)   # large pushes drain over seconds; 2s was the v1 bug
    t0 = time.time()
    # control line "__put <remote> <len>" then exactly <len> raw bytes
    nc.s.sendall(("__put %s %d\n" % (remote, len(data))).encode())
    nc.s.sendall(data)
    reply = nc.expect(b"aios# ", to=max(30, len(data) // 1500 + 30))
    line = reply.split(b"aios# ")[0].decode("utf-8", "replace").strip()
    if "__put ok" not in line:
        nc.close(); print("push failed: %r" % line); return False
    pisha = nc.cmd("sha256sum %s" % remote).split()[0].decode()
    nc.close()
    dt = time.time() - t0
    ok = (pisha == sha)
    print("pushed %d bytes -> %s in %.1fs   INTEGRITY: %s" %
          (len(data), remote, dt, "OK" if ok else "MISMATCH (pi %s)" % pisha))
    return ok

if __name__ == "__main__":
    a = sys.argv
    if len(a) >= 4 and a[1] == "pull":
        sys.exit(0 if pull(a[2], a[3], a[4] if len(a) > 4 else DEFAULT_HOST) else 1)
    if len(a) >= 4 and a[1] == "push":
        sys.exit(0 if push(a[2], a[3], a[4] if len(a) > 4 else DEFAULT_HOST) else 1)
    print("usage: pi_filexfer.py pull <remote> <local> [host]")
    print("       pi_filexfer.py push <local> <remote> [host]")
    sys.exit(2)
