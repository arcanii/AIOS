#!/usr/bin/env python3
"""serwatch.py -- prime-and-watch the lossy RPi4 mini-UART for stall detection.

The FTDI link delivers nothing until primed with a TX, and the mini-UART is
lossy, so a purely-passive sercap can miss the [tlbi] alive beats. This sends a
CR on open + every PRIME_S seconds (harmless to the getty login prompt) to keep
the link flowing, timestamps every received line, and reports gaps between
[tlbi] alive beats (a gap >> 30s == a whole-system stall, the detector).

Usage: python3 scripts/serwatch.py [seconds] [--dev /dev/cu.usbserial-0001] [--log FILE]
"""
import os, sys, time, select
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aios_console as ac

DEV = "/dev/cu.usbserial-0001"
PRIME_S = 8.0

def main():
    dur = 70.0
    log = None
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--dev": globals()["DEV"] = args[i+1]; i += 2
        elif args[i] == "--log": log = args[i+1]; i += 2
        else: dur = float(args[i]); i += 1
    lf = open(log, "a") if log else None
    fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY)
    ac.configure_serial(fd, 115200)
    t0 = time.time()
    os.write(fd, b"\r")
    last_prime = t0
    buf = b""
    last_beat = None
    maxgap = 0.0
    beats = 0
    print("serwatch: %s for %.0fs (priming CR every %.0fs)" % (DEV, dur, PRIME_S))
    while time.time() - t0 < dur:
        now = time.time()
        if now - last_prime >= PRIME_S:
            try: os.write(fd, b"\r")
            except OSError: pass
            last_prime = now
        r, _, _ = select.select([fd], [], [], 0.5)
        if not r: continue
        try: data = os.read(fd, 4096)
        except OSError: break
        if not data: continue
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            now = time.time()
            s = line.decode("utf-8", "replace").rstrip("\r")
            if not s.strip(): continue
            ts = now - t0
            out = "%7.2f  %s" % (ts, s)
            print(out, flush=True)
            if lf: lf.write(out + "\n"); lf.flush()
            if "tlbi" in s and "alive" in s:
                beats += 1
                if last_beat is not None:
                    g = now - last_beat
                    if g > maxgap: maxgap = g
                    print("         <beat gap %.1fs>" % g, flush=True)
                last_beat = now
    os.close(fd)
    if lf: lf.close()
    print("=== done: %d alive-beats seen, max beat gap=%.1fs ===" % (beats, maxgap))

if __name__ == "__main__":
    main()
