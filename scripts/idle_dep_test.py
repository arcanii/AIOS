#!/usr/bin/env python3
"""idle_dep_test.py -- is the Source-B stall gated by a preceding IDLE period?

Behavioral hypothesis (V4 serial soak): a process-teardown burst stalls only
when the core sat idle before it; back-to-back bursts are clean. This tests it
directly over serial (works on a GENET-off / network-stranded board):

  warm burst (baseline, no preceding idle)
  for each idle in [2,5,10,20,40]s:
      idle <n>s   (no commands -> core 0 idles)
      timed teardown burst (20 spawns); record wall time
  3 back-to-back bursts (control: no idle between)

A post-idle burst time that climbs with idle length (and >> the warm/back-to-back
baseline, hitting ~10.8s multiples) = idle-gated retention stall confirmed.

One process owns the serial port -- stop scripts/sercap.py first.
"""
import os
import re
import sys
import termios
import time

DEV = "/dev/cu.usbserial-0001"
BURST = "i=0; while [ $i -lt 20 ]; do /bin/echo -n .; i=$((i+1)); done; echo D$i"


def open_serial(dev, baud=115200):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    sp = getattr(termios, "B%d" % baud)
    a = termios.tcgetattr(fd)
    a[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP
              | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
    a[1] &= ~termios.OPOST
    a[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)
    a[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    a[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG
              | termios.IEXTEN)
    a[4] = a[5] = sp
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 1
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


class S:
    def __init__(self, fd):
        self.fd = fd
        self.buf = b""

    def rd(self):
        try:
            d = os.read(self.fd, 4096)
        except (BlockingIOError, OSError):
            d = b""
        if d:
            self.buf += d
        return d

    def exp(self, pat, to):
        dl = time.time() + to
        while time.time() < dl:
            if pat.encode() in self.buf[-400:]:
                return True
            if not self.rd():
                time.sleep(0.03)
        return False

    def send(self, s):
        self.buf = b""
        os.write(self.fd, (s + "\n").encode())


def burst(s, to=240):
    t0 = time.time()
    s.send(BURST)
    ok = s.exp("D20", to)
    return time.time() - t0, ok


def main():
    s = S(open_serial(DEV))
    s.send("")
    p = s.exp("# ", 60) or s.exp("login:", 5)
    if "login:" in s.buf.decode("utf-8", "replace")[-200:] or not p:
        s.send("root"); s.exp("word:", 60); s.send("root")
        if not s.exp("# ", 60):
            print("FAIL login"); sys.exit(1)
    print("[idle] logged in", flush=True)

    dt, ok = burst(s)
    print("[idle] warm burst (no preceding idle): %.1fs%s"
          % (dt, "" if ok else " TIMEOUT"), flush=True)

    for n in [2, 5, 10, 20, 40]:
        print("[idle] idling %ds (core 0 quiesces)..." % n, flush=True)
        time.sleep(n)
        s.rd()
        dt, ok = burst(s)
        flag = "" if ok else " TIMEOUT"
        if dt >= 5:
            flag += "  <-- STALL (%.1fx quantum)" % (dt / 10.8)
        print("[idle] post-%2ds-idle burst: %.1fs%s" % (n, dt, flag), flush=True)

    print("[idle] 3 back-to-back bursts (control, no idle between):", flush=True)
    for i in range(3):
        dt, ok = burst(s)
        print("[idle]   b2b %d: %.1fs%s" % (i + 1, dt, "" if ok else " TIMEOUT"),
              flush=True)

    text = s.buf.decode("utf-8", "replace")
    slow = [int(m) for _, m in re.findall(r"\[(tlbi|pipe|reap)\] SLOW[^\n]*?(\d+)ms", text)]
    print("[idle] done. SLOW probe lines this run: %s" % [m for m in slow if m >= 5000],
          flush=True)


if __name__ == "__main__":
    main()
