#!/usr/bin/env python3
"""sourceb_soak.py -- standardized per-variant load + soak for the Source-B hunt.

Drives a fixed workload at a running AIOS Pi and renders a CLEAN/DIRTY verdict
for the 10.8s-quantum whole-system freezes (project_stall_hunt, Source B).
Run scripts/sercap.py FIRST (separate process, same log file) -- the serial
capture is the primary detector; this script appends phase markers into the
same timeline and parses it for the verdict.

Phases (all timings recorded):
  pre   counters snapshot: /proc/version, /proc/netstat, /proc/cachestats
  L1    N echo round-trips over netconsole (classic spawn probe)
  L2    M pushes of a ~1.5MB payload to /tmp/soak.bin (the pi_flash workload
        that fires quanta ~1/3 of the time on the dirty baseline)
  L3    K chunks of 20 /bin/echo spawns in one dash loop (local TLBI storm,
        no GENET traffic during execution)
  I1    idle minutes -- no connection held, hammer beats on serial only
  post  counters snapshot + delta

Verdict evidence (any -> DIRTY):
  - capture log [tlbi]/[pipe]/[reap] SLOW with ms >= 5000
  - gap > 45s between [tlbi] alive beats while the system should be idle
  - any single netconsole op taking >= 10s wall (normal: echo <1s)
  - emmc_timeout_retries/fails delta > 0
Wedges (connection dies / no banner) are recorded as suspect events, not
proof -- the serial capture decides.

Usage:
  python3 scripts/sourceb_soak.py --variant V0 [--host 192.168.0.127]
      [--cap /tmp/sercap.log] [--rt 10] [--pushes 3] [--spawn-chunks 5]
      [--idle-min 20] [--payload disk/kernel8.img] [--quick]
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import pi_filexfer as fx

SETTLE = 4          # polite gap between connections (netconsole discipline)
WEDGE_SETTLE = 45   # recovery pause after a dropped/failed connection


class Soak:
    def __init__(self, a):
        self.a = a
        self.nc = None
        self.events = []      # (phase, kind, detail, seconds)
        self.optimes = []     # (phase, op, seconds)
        self.wedges = 0
        self.t0 = time.time()
        self.cap = open(a.cap, "a", buffering=1) if a.cap else None

    def mark(self, msg):
        line = "[soak] %s %.3f %s" % (time.strftime("%H:%M:%S"), time.time(), msg)
        print(line, flush=True)
        if self.cap:
            self.cap.write(line + "\n")

    def connect(self, tries=6):
        last = None
        for i in range(tries):
            try:
                nc = fx.NC(self.a.host)
                nc.expect(b"aios# ", to=200)
                self.nc = nc
                return True
            except (OSError, TimeoutError) as e:
                last = e
                self.mark("connect attempt %d failed: %s" % (i + 1, e))
                time.sleep(WEDGE_SETTLE)
        self.mark("CONNECT-FAILED after %d tries: %s" % (tries, last))
        return False

    def drop(self):
        if self.nc:
            try:
                self.nc.close()
            except OSError:
                pass
            self.nc = None

    def cmd(self, c, phase, to=300):
        t = time.time()
        try:
            out = self.nc.cmd(c, to=to).decode("utf-8", "replace")
            dt = time.time() - t
            self.optimes.append((phase, c[:40], dt))
            if dt >= 10:
                self.events.append((phase, "SLOW-OP", "%s took %.1fs" % (c[:40], dt), dt))
                self.mark("SLOW-OP %.1fs: %s" % (dt, c[:60]))
            return out
        except (OSError, TimeoutError) as e:
            dt = time.time() - t
            self.wedges += 1
            self.events.append((phase, "WEDGE", "%s after %.1fs: %s" % (c[:40], dt, e), dt))
            self.mark("WEDGE in %s after %.1fs: %s" % (phase, dt, e))
            self.drop()
            time.sleep(WEDGE_SETTLE)
            self.connect()
            return None

    def snapshot(self, name):
        self.mark("phase %s start" % name)
        snap = {}
        if not self.nc and not self.connect():
            return snap
        for f in ["version", "netstat", "cachestats"]:
            out = self.cmd("cat /proc/%s" % f, name, to=240)
            if out is not None:
                snap[f] = out.strip()
            time.sleep(1)
        self.mark("phase %s done" % name)
        return snap

    def l1_roundtrips(self, n):
        self.mark("phase L1 start (%d echo round-trips)" % n)
        for i in range(n):
            if not self.nc and not self.connect():
                return
            self.cmd("echo rt%d" % i, "L1", to=240)
            time.sleep(1)
        self.mark("phase L1 done")

    def l2_pushes(self, m, payload):
        with open(payload, "rb") as f:
            data = f.read()
        self.mark("phase L2 start (%d pushes of %d bytes)" % (m, len(data)))
        for i in range(m):
            if not self.nc and not self.connect():
                return
            t = time.time()
            try:
                self.nc.s.settimeout(180)
                self.nc.s.sendall(("__put /tmp/soak.bin %d\n" % len(data)).encode())
                self.nc.s.sendall(data)
                reply = self.nc.expect(b"aios# ", to=max(300, len(data) // 1500 + 120))
                dt = time.time() - t
                ok = b"__put ok" in reply
                self.optimes.append(("L2", "push%d" % i, dt))
                self.mark("push %d/%d %s in %.1fs" % (i + 1, m, "ok" if ok else "BAD", dt))
                if dt >= 60 + len(data) // 15000:
                    self.events.append(("L2", "SLOW-PUSH", "push %d took %.1fs" % (i, dt), dt))
                if not ok:
                    self.events.append(("L2", "PUSH-FAIL", reply[-120:].decode("utf-8", "replace"), dt))
            except (OSError, TimeoutError) as e:
                dt = time.time() - t
                self.wedges += 1
                self.events.append(("L2", "WEDGE", "push %d after %.1fs: %s" % (i, dt, e), dt))
                self.mark("WEDGE on push %d after %.1fs: %s" % (i, dt, e))
                self.drop()
                time.sleep(WEDGE_SETTLE)
                self.connect()
            time.sleep(SETTLE)
        if self.nc:
            self.cmd("rm -f /tmp/soak.bin", "L2", to=240)
        self.mark("phase L2 done")

    def l3_spawns(self, k):
        self.mark("phase L3 start (%d chunks x 20 spawns)" % k)
        loop = "i=0; while [ $i -lt 20 ]; do /bin/echo -n .; i=$((i+1)); done"
        for i in range(k):
            if not self.nc and not self.connect():
                return
            self.cmd(loop, "L3", to=280)
            time.sleep(2)
        self.mark("phase L3 done")

    def i1_idle(self, minutes):
        self.mark("phase I1 start (%d min idle, no connection)" % minutes)
        self.drop()
        time.sleep(minutes * 60)
        self.mark("phase I1 done")


def parse_capture(cap_path, t_start, t_end):
    """SLOW lines >= 5000ms and alive-beat gaps > 45s inside [t_start, t_end].

    sercap lines are "HH:MM:SS.mmm EPOCH.mmm payload" -- the epoch is field 2
    (a naive \\d+\\.\\d+ search would grab the SS.mmm fragment of field 1)."""
    findings = []
    if not cap_path or not os.path.exists(cap_path):
        return findings, "no capture file"
    slow_re = re.compile(r"\[(tlbi|pipe|reap)\] SLOW.*?(\d+)ms")
    destroy_re = re.compile(r"\[reap\] SLOW.*destroy=(\d+)ms")
    alive_re = re.compile(r"\[tlbi\] alive rounds=(\d+)")
    last_alive = None
    with open(cap_path, "r", errors="replace") as f:
        for line in f:
            if line.startswith("[soak]") or line.startswith("[sercap]"):
                continue
            parts = line.split(None, 2)
            if len(parts) < 3:
                continue
            try:
                ts = float(parts[1])
            except ValueError:
                continue
            payload = parts[2]
            m = destroy_re.search(payload) or slow_re.search(payload)
            if m:
                ms = int(m.group(m.lastindex))
                mod = m.group(1) if m.re is slow_re else "reap"
                if t_start <= ts <= t_end and ms >= 5000:
                    findings.append("SLOW %dms [%s] at +%.0fs" % (ms, mod, ts - t_start))
                continue
            m = alive_re.search(payload)
            if m:
                if last_alive is not None and t_start <= ts <= t_end:
                    gap = ts - last_alive
                    if gap > 45:
                        findings.append("ALIVE-GAP %.0fs ending at +%.0fs"
                                        % (gap, ts - t_start))
                last_alive = ts
    return findings, None


def counter_delta(pre, post, key):
    def get(txt):
        m = re.search(r"%s:\s*(\d+)" % key, txt or "")
        return int(m.group(1)) if m else None
    a, b = get(pre.get("cachestats", "")), get(post.get("cachestats", ""))
    if a is None or b is None:
        return None
    return b - a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", required=True)
    ap.add_argument("--host", default="192.168.0.127")
    ap.add_argument("--cap", default="/tmp/sercap.log")
    ap.add_argument("--rt", type=int, default=6)
    ap.add_argument("--pushes", type=int, default=2)
    ap.add_argument("--spawn-chunks", type=int, default=6)
    ap.add_argument("--idle-min", type=int, default=4)
    ap.add_argument("--payload", default=os.path.join(REPO, "disk", "kernel8.img"))
    ap.add_argument("--quick", action="store_true",
                    help="rt=4 pushes=1 chunks=2 idle=3 (harness smoke test)")
    ap.add_argument("--full", action="store_true",
                    help="rt=10 pushes=3 chunks=8 idle=20 (long confirmation run)")
    a = ap.parse_args()
    if a.quick:
        a.rt, a.pushes, a.spawn_chunks, a.idle_min = 4, 1, 2, 3
    if a.full:
        a.rt, a.pushes, a.spawn_chunks, a.idle_min = 10, 3, 8, 20

    s = Soak(a)
    s.mark("=== soak %s start (rt=%d pushes=%d chunks=%d idle=%dmin) ==="
           % (a.variant, a.rt, a.pushes, a.spawn_chunks, a.idle_min))
    t_start = time.time()

    pre = s.snapshot("pre")
    ver = (pre.get("version", "?").splitlines() or ["?"])[-1]
    s.mark("running: %s" % ver)
    time.sleep(SETTLE)
    s.l1_roundtrips(a.rt)
    time.sleep(SETTLE)
    s.l2_pushes(a.pushes, a.payload)
    time.sleep(SETTLE)
    s.l3_spawns(a.spawn_chunks)
    s.i1_idle(a.idle_min)
    post = s.snapshot("post")
    s.drop()
    t_end = time.time()

    cap_findings, cap_err = parse_capture(a.cap, t_start, t_end)
    emmc_r = counter_delta(pre, post, "emmc_timeout_retries")
    emmc_f = counter_delta(pre, post, "emmc_timeout_fails")

    dirty = []
    dirty += cap_findings
    if emmc_r:
        dirty.append("emmc_timeout_retries +%d" % emmc_r)
    if emmc_f:
        dirty.append("emmc_timeout_fails +%d" % emmc_f)
    dirty += ["%s %s %s" % (p, k, d) for (p, k, d, _) in s.events
              if k in ("SLOW-OP", "SLOW-PUSH")]
    suspects = ["%s %s %s" % (p, k, d) for (p, k, d, _) in s.events if k == "WEDGE"]

    summary = {
        "variant": a.variant,
        "version": ver,
        "duration_s": round(t_end - t_start, 1),
        "verdict": "DIRTY" if dirty else "CLEAN",
        "dirty_evidence": dirty,
        "suspect_events": suspects,
        "wedges": s.wedges,
        "emmc_timeout_retries_delta": emmc_r,
        "emmc_timeout_fails_delta": emmc_f,
        "capture_parse_error": cap_err,
        "op_times_over_3s": [(p, o, round(t, 1)) for (p, o, t) in s.optimes if t >= 3],
    }
    out_path = "/tmp/soak_%s_%d.json" % (a.variant, int(t_start))
    with open(out_path, "w") as f:
        json.dump(summary, f, indent=2)

    s.mark("=== soak %s VERDICT: %s ===" % (a.variant, summary["verdict"]))
    print(json.dumps(summary, indent=2))
    print("[soak] %s summary -> %s" % ("OK" if not cap_err else "WARN", out_path))


if __name__ == "__main__":
    main()
