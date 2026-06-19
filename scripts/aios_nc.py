#!/usr/bin/env python3
"""aios_nc.py -- robust netconsole driver for the AIOS RPi4 (and QEMU hostfwd).

THE reliable way to drive the Pi over netconsole. The Pi's netconsole
(src/apps/netconsole.c serve_client) already runs MANY commands per connection
-- but every ad-hoc host tool (picmd, netstall one_shot, fatconfig nc_cmd) opens
a FRESH connection per command. That back-to-back-connection pattern is exactly
what wedges netconsole on real HW (a per-connection fork storm; see the
netconsole-status memory). This driver HOLDS ONE connection for a whole batch of
commands -- no connection storm, no wedge -- which is the documented "one held
conn" best practice.

Robustness:
  - one held connection for N commands (the wedge-free path)
  - per-command timeout (a stuck command cannot hang the driver forever)
  - prompt resync (netconsole can drop the first output line to prompt-sync)
  - reconnect-on-drop, BOUNDED + with a settle (never a tight retry loop, which
    would itself storm the listener); the in-flight command is retried once
  - clean, parseable output per command

CLI:
  aios_nc.py [--host H] [--port P] [--timeout T] CMD [CMD ...]   # run each CMD
  aios_nc.py [--host H] --batch FILE                             # one CMD per line
  echo "uname -a" | aios_nc.py [--host H] -                      # cmds from stdin

Importable:
  from aios_nc import NC
  nc = NC("192.168.0.8"); print(nc.run("uname -a")); nc.close()
"""
import argparse
import socket
import sys
import time

DEFAULT_HOST = "192.168.0.8"
DEFAULT_PORT = 2323
PROMPT = b"aios# "


class NCError(Exception):
    pass


class NC:
    """One held netconsole connection. run() sends a command and returns its
    output (text up to the next prompt). Auto-reconnects on a dropped connection,
    bounded, with a settle -- so a transient drop does not turn into a storm."""

    # Stall-resilient defaults. The RPi4 has a residual ~33s idle-teardown TLBI
    # freeze (~2-3% of teardown-after-idle; cure ruled out, see
    # project_stall_ubus_deadend) that freezes the WHOLE box -- including
    # netconsole -- for one ~33s quantum. A robust driver must RIDE THROUGH it,
    # not time out under it: the per-command timeout (run timeout=) comfortably
    # exceeds one quantum, and a dropped connection is reconnected patiently over
    # a window that spans the freeze (settle * reconnects), never a tight loop.
    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, connect_timeout=15,
                 settle=6.0, reconnects=8):
        self.host = host
        self.port = port
        self.connect_timeout = connect_timeout
        self.settle = settle
        self.reconnects = reconnects
        self.s = None
        self.buf = b""
        self._connect()

    def _connect(self):
        last = None
        for attempt in range(self.reconnects + 1):
            try:
                s = socket.create_connection((self.host, self.port),
                                             timeout=self.connect_timeout)
                s.settimeout(1.0)
                self.s = s
                self.buf = b""
                # Read the banner + first prompt so run() starts clean.
                self._expect(PROMPT, timeout=8)
                return
            except (OSError, NCError) as e:
                last = e
                try:
                    if self.s:
                        self.s.close()
                except OSError:
                    pass
                self.s = None
                if attempt < self.reconnects:
                    time.sleep(self.settle)
        raise NCError("connect to %s:%d failed after %d tries: %s"
                      % (self.host, self.port, self.reconnects + 1, last))

    def _expect(self, pat, timeout):
        dl = time.time() + timeout
        while True:
            i = self.buf.find(pat)
            if i != -1:
                out = self.buf[:i]
                self.buf = self.buf[i + len(pat):]
                return out
            if time.time() > dl:
                raise NCError("expect %r timed out; tail=%r" % (pat, self.buf[-160:]))
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            if not d:
                raise NCError("connection closed; tail=%r" % self.buf[-160:])
            self.buf += d

    def run(self, cmd, timeout=50):
        """Send one command, return its output (str). The 50s default rides
        through one ~33s idle-teardown freeze without dropping. On an actual
        dropped connection it reconnects (patiently, via _connect) + retries the
        command once."""
        for attempt in range(2):
            try:
                self.s.sendall((cmd + "\n").encode())
                out = self._expect(PROMPT, timeout=timeout)
                return out.decode("utf-8", "replace")
            except (OSError, NCError):
                # connection dropped (idle timeout / transient) -- reconnect ONCE
                # with a settle (never a tight loop) and retry the command.
                if attempt == 0:
                    time.sleep(self.settle)
                    self._connect()
                    continue
                raise

    def close(self):
        try:
            self.s.sendall(b"exit\n")
            self.s.close()
        except OSError:
            pass
        self.s = None


def main():
    ap = argparse.ArgumentParser(description="robust held-connection netconsole driver")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--timeout", type=float, default=50,
                    help="per-command timeout (s); default rides one ~33s stall")
    ap.add_argument("--batch", help="file with one command per line")
    ap.add_argument("--quiet", action="store_true", help="omit the '### cmd' headers")
    ap.add_argument("cmds", nargs="*", help="commands (or '-' to read stdin)")
    a = ap.parse_args()

    cmds = []
    if a.batch:
        with open(a.batch) as f:
            cmds = [ln.rstrip("\n") for ln in f if ln.strip()]
    elif a.cmds == ["-"]:
        cmds = [ln.rstrip("\n") for ln in sys.stdin if ln.strip()]
    else:
        cmds = a.cmds
    if not cmds:
        ap.error("no commands (give CMD..., --batch FILE, or '-' for stdin)")

    nc = NC(a.host, a.port)
    rc = 0
    try:
        for cmd in cmds:
            if not a.quiet:
                print("### " + cmd, flush=True)
            try:
                print(nc.run(cmd, timeout=a.timeout).strip(), flush=True)
            except NCError as e:
                print("[aios_nc: %s]" % e, flush=True)
                rc = 1
                break
            if not a.quiet:
                print(flush=True)
    finally:
        nc.close()
    sys.exit(rc)


if __name__ == "__main__":
    main()
