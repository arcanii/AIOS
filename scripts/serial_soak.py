#!/usr/bin/env python3
"""serial_soak.py -- serial-only teardown-load soak + stall detector.

For GENET-off variants (V4) where netconsole is gone: drive a process-teardown
spawn-storm over the serial console AND detect Source-B quanta in the same
stream (the [tlbi] alive 30s beat + [pipe]/[reap] SLOW prints). One process
owns the port -- stop scripts/sercap.py first.

Logs in (root/root) with quantum-tolerant stage timeouts, runs K chunks of a
20-spawn shell loop (each chunk = 20 process teardowns, the L3 workload), and
times each chunk. A chunk wall-time >> a few seconds, or a SLOW>=5000ms line,
or a >45s gap between [tlbi] alive beats = a stall quantum.

Usage: python3 scripts/serial_soak.py [--dev ...] [--chunks 8] [--log FILE]
"""
import argparse
import os
import re
import sys
import termios
import time

LOOP = "i=0; while [ $i -lt 20 ]; do /bin/echo -n .; i=$((i+1)); done; echo OK$i"


def open_serial(dev, baud=115200):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    speed = getattr(termios, "B%d" % baud)
    a = termios.tcgetattr(fd)
    a[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP
              | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
    a[1] &= ~termios.OPOST
    a[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)
    a[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    a[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG
              | termios.IEXTEN)
    a[4] = a[5] = speed
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 1
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


class Serial:
    def __init__(self, dev, log):
        self.fd = open_serial(dev)
        self.buf = b""
        self.log = log

    def read_into(self):
        try:
            d = os.read(self.fd, 4096)
        except (BlockingIOError, OSError):
            d = b""
        if d:
            self.buf += d
            if self.log:
                self.log.write(d)
        return d

    def expect(self, pats, to):
        dl = time.time() + to
        while time.time() < dl:
            for p in pats:
                if p.encode() in self.buf[-600:]:
                    return p
            if not self.read_into():
                time.sleep(0.05)
        return None

    def send(self, s):
        os.write(self.fd, (s + "\n").encode())


def stamp():
    return time.strftime("%H:%M:%S")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", default="/dev/cu.usbserial-0001")
    ap.add_argument("--chunks", type=int, default=8)
    ap.add_argument("--variant", default="V4")
    ap.add_argument("--log", default="/tmp/serial_soak.log")
    ap.add_argument("--stage-to", type=float, default=240,
                    help="per-stage timeout, must outlast a 4x quantum")
    a = ap.parse_args()

    log = open(a.log, "ab", buffering=0)
    log.write(("\n=== serial_soak %s %s ===\n" % (a.variant, stamp())).encode())
    s = Serial(a.dev, log)
    print("[ssoak] %s driving %s, %d chunks -> %s" %
          (stamp(), a.variant, a.chunks, a.log), flush=True)

    # ---- login (quantum-tolerant) ----
    s.send("")
    p = s.expect(["login:", "# "], a.stage_to)
    if p == "login:":
        s.send("root")
        if not s.expect(["word:"], a.stage_to):
            print("[ssoak] FAIL no password prompt"); sys.exit(1)
        s.send("root")
        if not s.expect(["# "], a.stage_to):
            print("[ssoak] FAIL no shell prompt"); sys.exit(1)
    elif p != "# ":
        print("[ssoak] FAIL no login prompt (board stalled at boot?)")
        sys.exit(1)
    print("[ssoak] %s logged in" % stamp(), flush=True)

    # ---- spawn-storm chunks ----
    chunk_times = []
    stalls = []
    for i in range(a.chunks):
        t0 = time.time()
        s.send(LOOP)
        ok = s.expect(["OK20"], a.stage_to)
        dt = time.time() - t0
        chunk_times.append(dt)
        flag = ""
        if not ok:
            flag = " TIMEOUT(stall)"
            stalls.append(("chunk%d" % i, dt))
        elif dt >= 5:
            flag = " SLOW"
            stalls.append(("chunk%d" % i, dt))
        print("[ssoak] %s chunk %d/%d: %.1fs%s" %
              (stamp(), i + 1, a.chunks, dt, flag), flush=True)
        # drain any trailing output between chunks
        time.sleep(1.5)
        s.read_into()

    # ---- parse the captured stream for the kernel stall probes ----
    text = s.buf.decode("utf-8", "replace")
    slow = re.findall(r"\[(tlbi|pipe|reap)\] SLOW[^\n]*?(\d+)ms", text)
    big = [(m, int(ms)) for m, ms in slow if int(ms) >= 5000]

    print("---", flush=True)
    print("[ssoak] chunk times: %s" %
          ", ".join("%.1f" % t for t in chunk_times), flush=True)
    print("[ssoak] chunk stalls (>=5s or timeout): %d" % len(stalls), flush=True)
    print("[ssoak] SLOW probe lines >=5s in stream: %d %s" %
          (len(big), big[:6]), flush=True)
    verdict = "DIRTY" if (stalls or big) else "CLEAN"
    print("[ssoak] %s VERDICT %s: %s" % (stamp(), a.variant, verdict), flush=True)


if __name__ == "__main__":
    main()
