#!/usr/bin/env python3
"""pcap_tcp_dump.py -- minimal stdlib pcap reader for the netrx B3 hunt.

Prints one line per TCP segment on the chosen port: relative time, direction
(H>G host/SLIRP to guest, G>H guest to SLIRP), seq/ack RELATIVE to each
direction's ISN, flags, advertised window, payload length. No scapy needed.

Usage: python3 scripts/pcap_tcp_dump.py /tmp/b3.pcap [--port 2323] [--max 200]
"""
import argparse
import struct
import sys


def flags_str(f):
    out = []
    for bit, ch in [(0x02, "S"), (0x10, "A"), (0x01, "F"), (0x04, "R"),
                    (0x08, "P"), (0x20, "U")]:
        if f & bit:
            out.append(ch)
    return "".join(out) or "-"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--port", type=int, default=2323)
    ap.add_argument("--max", type=int, default=200)
    ap.add_argument("--tail", type=int, default=0,
                    help="only print the last N segments")
    a = ap.parse_args()

    f = open(a.pcap, "rb")
    gh = f.read(24)
    if len(gh) < 24:
        print("FAIL truncated pcap"); sys.exit(1)
    magic = struct.unpack("<I", gh[:4])[0]
    if magic == 0xa1b2c3d4:
        endian = "<"
    elif struct.unpack(">I", gh[:4])[0] == 0xa1b2c3d4:
        endian = ">"
    else:
        print("FAIL bad magic %08x" % magic); sys.exit(1)

    rows = []
    t0 = None
    isn = {}
    while True:
        ph = f.read(16)
        if len(ph) < 16:
            break
        ts, tu, incl, _orig = struct.unpack(endian + "IIII", ph)
        data = f.read(incl)
        if len(data) < incl:
            break
        t = ts + tu / 1e6
        if t0 is None:
            t0 = t
        if len(data) < 14 + 20:
            continue
        if data[12:14] != b"\x08\x00":          # IPv4 only
            continue
        ip = data[14:]
        ihl = (ip[0] & 0xF) * 4
        if ip[9] != 6 or len(ip) < ihl + 20:     # TCP only
            continue
        tot = struct.unpack(">H", ip[2:4])[0]
        tcp = ip[ihl:tot]
        sport, dport = struct.unpack(">HH", tcp[0:4])
        if a.port not in (sport, dport):
            continue
        seq, ack = struct.unpack(">II", tcp[4:12])
        doff = (tcp[12] >> 4) * 4
        fl = tcp[13]
        win = struct.unpack(">H", tcp[14:16])[0]
        plen = max(0, tot - ihl - doff)
        d = "H>G" if dport == a.port else "G>H"
        if d not in isn and (fl & 0x02):
            isn[d] = seq
        rs = (seq - isn.get(d, seq)) & 0xFFFFFFFF
        od = "G>H" if d == "H>G" else "H>G"
        ra = (ack - isn.get(od, ack)) & 0xFFFFFFFF if (fl & 0x10) else 0
        rows.append((t - t0, d, rs, ra, flags_str(fl), win, plen))

    if a.tail:
        rows = rows[-a.tail:]
    else:
        rows = rows[:a.max]
    print("%8s %3s %10s %10s %5s %6s %5s" %
          ("t", "dir", "seq", "ack", "flags", "win", "len"))
    for t, d, s, k, fl, w, ln in rows:
        print("%8.3f %3s %10d %10d %5s %6d %5d" % (t, d, s, k, fl, w, ln))
    print("(%d segments total on port %d)" % (len(rows), a.port))


if __name__ == "__main__":
    main()
