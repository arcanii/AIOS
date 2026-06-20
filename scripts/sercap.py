#!/usr/bin/env python3
"""sercap.py -- timestamped read-only serial capture for the stall hunt.

Opens the Pi serial console (8N1, default 115200), prefixes every line with
wall-clock time, and appends to a log file. Survives port drops (the USB
adapter can hiccup across Pi power cycles) by reopening with retries, and
keeps running across warm reboots so boot banners land in the same timeline.

The [tlbi]/[pipe]/[reap] stall probes print to the SERIAL console only (not
/proc/log), so this capture is the primary Source-B quantum detector:
  - "[tlbi] SLOW round=N worst_unmap=NNNNms"  = a stalled unmap, directly
  - "[tlbi] alive rounds=N" beats every ~30s   = gaps >45s mean a freeze
    even when the SLOW print itself was lost

Usage:  python3 scripts/sercap.py /tmp/sercap.log [--dev /dev/cu.usbserial-0001]
Stop with SIGTERM/SIGINT. One reader only -- quit other serial users first.
"""
import argparse
import os
import sys
import termios
import time


def open_serial(dev, baud):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)   # O_RDWR: macOS FTDI rejects tcsetattr on a read-only fd (EINVAL)
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


def stamp():
    t = time.time()
    return "%s.%03d %.3f" % (time.strftime("%H:%M:%S", time.localtime(t)),
                             (t % 1) * 1000, t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logfile")
    ap.add_argument("--dev", default="/dev/cu.usbserial-0001")
    ap.add_argument("--baud", type=int, default=115200)
    a = ap.parse_args()

    log = open(a.logfile, "a", buffering=1)
    log.write("[sercap] %s capture start dev=%s\n" % (stamp(), a.dev))
    print("[sercap] OK capturing %s -> %s" % (a.dev, a.logfile), flush=True)

    fd = None
    buf = b""
    last_data = time.time()
    while True:
        if fd is None:
            try:
                fd = open_serial(a.dev, a.baud)
                log.write("[sercap] %s port open\n" % stamp())
            except OSError as e:
                log.write("[sercap] %s port open failed: %s (retry 5s)\n"
                          % (stamp(), e))
                time.sleep(5)
                continue
        try:
            chunk = os.read(fd, 4096)
        except (OSError, BlockingIOError) as e:
            if isinstance(e, BlockingIOError) or e.errno == 35:
                time.sleep(0.05)
                continue
            log.write("[sercap] %s port error: %s (reopen)\n" % (stamp(), e))
            try:
                os.close(fd)
            except OSError:
                pass
            fd = None
            buf = b""
            time.sleep(2)
            continue
        if not chunk:
            time.sleep(0.05)
            continue
        last_data = time.time()
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            log.write("%s %s\n" % (stamp(),
                                   line.decode("utf-8", "replace").rstrip("\r")))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
