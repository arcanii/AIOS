#!/usr/bin/env python3
"""Drive the V3D Phase 0 probes over netconsole (port 2323) on the real Pi.

One held connection per invocation (netconsole runs one dash -c per line). Gentle
by design -- run a small batch of probes, then let the caller settle before the
next connection. If a command pokes hardware (cat /proc/v3d.power) and the box
SErrors or wedges, the connection drops and this reports it rather than hanging.

Usage: v3d_probe.py HOST CMD [CMD ...]
  e.g. v3d_probe.py 192.168.0.8 "cat /proc/v3d"
"""
import socket, sys, time

PORT = 2323


class NC:
    def __init__(self, host):
        self.s = socket.create_connection((host, PORT), timeout=10)
        self.s.settimeout(2.0)
        self.buf = b""

    def _fill(self, dl):
        if time.time() > dl:
            raise TimeoutError("netconsole timeout (%d bytes buffered)" % len(self.buf))
        try:
            self.buf += self.s.recv(65536)
        except socket.timeout:
            pass

    def expect(self, pat, to=30):
        dl = time.time() + to
        while True:
            i = self.buf.find(pat)
            if i != -1:
                r = self.buf[:i + len(pat)]
                self.buf = self.buf[i + len(pat):]
                return r
            self._fill(dl)

    def cmd(self, c, to=30):
        self.s.sendall((c + "\n").encode())
        return self.expect(b"aios# ", to).split(b"aios# ")[0]

    def close(self):
        try:
            self.s.sendall(b"exit\n")
            self.s.close()
        except OSError:
            pass


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    host, cmds = sys.argv[1], sys.argv[2:]
    nc = NC(host)
    rc = 0
    try:
        nc.expect(b"aios# ")   # consume banner + first prompt
        for c in cmds:
            print("=== %s ===" % c, flush=True)
            try:
                out = nc.cmd(c).decode("utf-8", "replace")
            except (TimeoutError, EOFError, OSError) as e:
                print("!! NO REPLY (%s: %s)" % (type(e).__name__, e))
                print("!! the box may have SError'd or wedged on this command")
                rc = 2
                break
            print(out.strip("\n"), flush=True)
            print(flush=True)
    finally:
        nc.close()
    return rc


sys.exit(main())
