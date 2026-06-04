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
    # NOTE: do NOT add `2>&1` -- netconsole already dup2's the child stderr to
    # the stdout pipe, and a redundant 2>&1 re-dups the pipe fd and breaks the
    # writer/EOF tracking, hanging the command. stderr already comes through.
    size_out = nc.cmd("wc -c %s" % remote).decode("utf-8", "replace").split()
    if not size_out or not size_out[0].isdigit():
        nc.close()
        print("pull failed: could not size %s (got %r)." % (remote, " ".join(size_out)[:80]))
        return False
    n = int(size_out[0])
    sha = nc.cmd("sha256sum %s" % remote).split()[0].decode()
    print("pi: %s = %d bytes, sha256 %s" % (remote, n, sha))
    t0 = time.time()
    nc.s.sendall(("cat %s\n" % remote).encode())
    try:
        data = nc.read_n(n, to=max(30, n // 2000 + 30))
    except TimeoutError as e:
        nc.close(); print("pull failed: %s (file too large/slow for netconsole)" % e); return False
    nc.expect(b"aios# "); nc.close()
    dt = time.time() - t0
    mac = hashlib.sha256(data).hexdigest()
    ok = (mac == sha and len(data) == n)
    with open(local, "wb") as f:
        f.write(data)
    print("pulled %d bytes -> %s in %.1fs   INTEGRITY: %s" %
          (len(data), local, dt, "OK" if ok else "MISMATCH (%s)" % mac))
    return ok

if __name__ == "__main__":
    a = sys.argv
    if len(a) >= 4 and a[1] == "pull":
        host = a[4] if len(a) > 4 else DEFAULT_HOST
        sys.exit(0 if pull(a[2], a[3], host) else 1)
    print(__doc__)
    sys.exit(2)
