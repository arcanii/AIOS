#!/usr/bin/env python3
"""s14_prewarm_ab.py -- serial-independent A/B driver for the PREWARM stall-cure confirmation.

The wedge is teardown-after-idle. One DRIVE CYCLE = idle T seconds with no connection (so the
BCM2711 SCB fabric parks), then one netconsole connect that reads /proc/laststall and disconnects;
the disconnect's shell teardown-after-idle is the wedge trigger, whose outcome shows up in the next
read. /proc/laststall total = g_wd_stalls = the serial-independent core-0 wedge count (counts ANY
~32s core-0 freeze, so total stays 0 only if the wedge was PREVENTED).

  s14_prewarm_ab.py preflight
  s14_prewarm_ab.py arm N            # arm /proc/confine.N (verify ticks climb)
  s14_prewarm_ab.py disarm
  s14_prewarm_ab.py readstall
  s14_prewarm_ab.py drive --cycles N --idle T [--label TAG]

No apostrophes in comments by house style. Prints OK/FAIL summaries.
"""
import argparse
import re
import socket
import sys
import time

HOST = "192.168.0.8"
PORT = 2323
PROMPT = b"aios# "


def nc_connect(overall_to=90):
    """Retry TCP connect + wait for the shell prompt until the board answers. Returns a socket."""
    t0 = time.time()
    while time.time() - t0 < overall_to:
        try:
            s = socket.create_connection((HOST, PORT), timeout=8)
        except OSError:
            time.sleep(1.5)
            continue
        s.settimeout(2.0)
        buf = b""
        t1 = time.time()
        while time.time() - t1 < 20:        # banner can sit behind a 32s quantum
            try:
                d = s.recv(4096)
            except socket.timeout:
                continue
            if not d:
                break
            buf += d
            if PROMPT in buf:
                return s
        try:
            s.close()
        except OSError:
            pass
        time.sleep(1.0)
    return None


def nc_cmd(s, line, to=40):
    """Send one command, collect output until the next prompt."""
    s.sendall(line.encode() + b"\n")
    buf = b""
    t0 = time.time()
    while time.time() - t0 < to:
        try:
            d = s.recv(4096)
        except socket.timeout:
            continue
        if not d:
            break
        buf += d
        if buf.count(PROMPT) >= 1 and buf.rstrip().endswith(b"aios#"):
            break
        if PROMPT in buf and line.encode() in buf:
            # output present and a trailing prompt seen
            if buf.rstrip().endswith(b"aios#"):
                break
    return buf.decode(errors="replace")


def parse_total(text):
    """Pull the laststall total out of /proc/laststall output. none-detected => 0."""
    if "none detected" in text:
        return 0
    m = re.search(r"total=(\d+)", text)
    if m:
        return int(m.group(1))
    return None


def read_one(cmd="cat /proc/laststall", connect_to=90):
    """One connection: run cmd, return its text. The disconnect is itself a teardown trigger."""
    s = nc_connect(connect_to)
    if not s:
        return None
    try:
        out = nc_cmd(s, cmd)
    finally:
        try:
            s.close()
        except OSError:
            pass
    return out


def preflight():
    s = nc_connect(120)
    if not s:
        print("FAIL preflight: board did not answer netconsole :2323")
        return 2
    try:
        for c in ["cat /proc/version", "cat /proc/watchdog", "cat /proc/laststall",
                  "cat /proc/confine"]:
            out = nc_cmd(s, c)
            # strip the echoed command + trailing prompt for readability
            print("--- %s ---" % c)
            print(out.strip())
            print()
    finally:
        s.close()
    print("OK preflight")
    return 0


def arm(core):
    out = read_one("cat /proc/confine.%d" % core)
    if out is None:
        print("FAIL arm: no connection")
        return 2
    print(out.strip())
    time.sleep(3)
    out2 = read_one("cat /proc/confine")
    print(out2.strip())
    m1 = re.search(r"ticks=(\d+)", out or "")
    m2 = re.search(r"ticks=(\d+)", out2 or "")
    if m1 and m2 and int(m2.group(1)) > int(m1.group(1)):
        print("OK arm: worker ticks climbing (%s -> %s)" % (m1.group(1), m2.group(1)))
        return 0
    print("FAIL arm: ticks not climbing -- worker not scheduled")
    return 1


def disarm():
    out = read_one("cat /proc/confine.r")
    print((out or "").strip())
    print("OK disarm")
    return 0


def readstall():
    out = read_one("cat /proc/laststall")
    if out is None:
        print("FAIL readstall: no connection")
        return 2
    print(out.strip())
    print("total=%s" % parse_total(out))
    return 0


def drive(cycles, idle, label):
    print("=== DRIVE label=%s cycles=%d idle=%ds ===" % (label, cycles, idle))
    # initial read (also trigger 0): records the running total before this drive
    out0 = read_one("cat /proc/laststall", connect_to=150)
    if out0 is None:
        print("FAIL drive: board unreachable at start")
        return 2
    base = parse_total(out0)
    print("[%s] base total=%s  (%s)" % (time.strftime("%H:%M:%S"), base, out0.strip().splitlines()[-2] if out0.strip().splitlines() else out0.strip()))
    if base is None:
        print("FAIL drive: cannot parse base total")
        return 2
    last = base
    for i in range(1, cycles + 1):
        time.sleep(idle)                          # idle window: fabric parks
        out = read_one("cat /proc/laststall", connect_to=150)
        if out is None:
            print("[%s] cycle %d/%d: UNREACHABLE (possible total wedge) -- waiting hwdog ~63s"
                  % (time.strftime("%H:%M:%S"), i, cycles))
            time.sleep(70)
            out = read_one("cat /proc/laststall", connect_to=180)
            if out is None:
                print("FAIL drive: board un-driveable after retry (total wedge?) at cycle %d" % i)
                return 3
        t = parse_total(out)
        delta = (t - last) if (t is not None and last is not None) else "?"
        flag = "  <-- WEDGE(S)" if (isinstance(delta, int) and delta > 0) else ""
        # pull last_dur if present
        m = re.search(r"last_dur=(\d+)ms", out)
        dur = (" last_dur=%sms" % m.group(1)) if m else ""
        print("[%s] cycle %d/%d: total=%s (+%s)%s%s"
              % (time.strftime("%H:%M:%S"), i, cycles, t, delta, dur, flag))
        last = t
    observed = (last - base) if (last is not None and base is not None) else None
    print("=== RESULT label=%s: base=%s final=%s WEDGES_OBSERVED=%s over %d cycles ==="
          % (label, base, last, observed, cycles))
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("preflight")
    pa = sub.add_parser("arm"); pa.add_argument("core", type=int)
    sub.add_parser("disarm")
    sub.add_parser("readstall")
    pd = sub.add_parser("drive")
    pd.add_argument("--cycles", type=int, default=20)
    pd.add_argument("--idle", type=int, default=35)
    pd.add_argument("--label", default="run")
    a = ap.parse_args()
    if a.cmd == "preflight":
        return preflight()
    if a.cmd == "arm":
        return arm(a.core)
    if a.cmd == "disarm":
        return disarm()
    if a.cmd == "readstall":
        return readstall()
    if a.cmd == "drive":
        return drive(a.cycles, a.idle, a.label)
    return 1


if __name__ == "__main__":
    sys.exit(main())
