#!/usr/bin/env python3
"""netstall.py -- host-timed teardown-after-idle stall probe (reconnect-robust).

The RPi4 TLBI/DVM stall freezes the WHOLE system for ~10.8s x N and fires on a
process-exit page-unmap AFTER the cores idled. Minimal trigger: `sleep N; echo M`
(the sleep idles cores 1-3, echo's teardown-after-idle is the stall candidate).
We time each trial host-side over netconsole; a clean trial returns ~N+0.3s, a
stalled one ~N + 11..70s. A SEVERE stall (>~60s) can kill the held connection, so
this RECONNECTS (counting the death as a stall) and aggregates over all trials.

The warmer A/B knob is driven on SEPARATE short connections (--warm arms
/proc/corewarm.1 first, disarms after) so the armed state persists across probe
reconnects -- the only variable between an OFF and an ON run is cores 1-3 busy.

Usage:
  python3 -u scripts/netstall.py [--host H] [--trials K] [--idle N] [--warm] [--label TAG]
"""
import socket, time, sys, argparse

PROMPT = b"aios#"
STALL_S = 8.0
RECONNECT_SETTLE = 22.0      # netconsole discipline after a dropped/severe-stall conn

def recv_until(s, marker, timeout):
    t0 = time.time(); buf = b""; s.settimeout(1.0)
    while time.time() - t0 < timeout:
        try: x = s.recv(4096)
        except socket.timeout: continue
        if not x: break
        buf += x
        if marker in buf: return time.time() - t0, buf, True
    return time.time() - t0, buf, False

def connect(host, port):
    s = socket.create_connection((host, port), timeout=15)
    recv_until(s, PROMPT, 6)         # banner + first prompt
    return s

def one_shot(host, port, cmd, timeout=15):
    """Open, run one command, return its output, close. For the warmer knob."""
    try:
        s = connect(host, port)
        s.sendall((cmd + "\n").encode())
        _, buf, _ = recv_until(s, PROMPT, timeout)
        s.close()
        return buf.decode("utf-8", "replace")
    except (OSError, socket.timeout) as e:
        return "ERR:%s" % e

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.0.8")
    ap.add_argument("--port", type=int, default=2323)
    ap.add_argument("--trials", type=int, default=16)
    ap.add_argument("--idle", type=int, default=8)
    ap.add_argument("--warm", action="store_true")
    ap.add_argument("--label", default="")
    a = ap.parse_args()

    print("=== netstall %s host=%s trials=%d idle=%ds warm=%s ==="
          % (a.label, a.host, a.trials, a.idle, a.warm), flush=True)

    if a.warm:
        r = one_shot(a.host, a.port, "cat /proc/corewarm.1")
        print("  ARM: %s" % r.replace("\n", " ").strip()[:80], flush=True)
        time.sleep(2)
        r = one_shot(a.host, a.port, "cat /proc/corewarm")
        print("  armed-check: %s" % r.replace("\n", " ").strip()[:90], flush=True)
        time.sleep(a.warm and 3 or 0)

    s = None
    residuals = []; stalls = 0; deaths = 0
    for k in range(a.trials):
        if s is None:
            try:
                s = connect(a.host, a.port)
            except (OSError, socket.timeout) as e:
                print("  trial %2d: reconnect failed (%s); settling" % (k, e), flush=True)
                time.sleep(RECONNECT_SETTLE); continue
        try:
            s.sendall(("sleep %d; echo T%d" % (a.idle, k)).encode() + b"\n")
            dt, buf, ok = recv_until(s, PROMPT, a.idle + 75)
        except OSError as e:
            ok = False; dt = a.idle + 75; buf = b""
            print("  trial %2d: send/recv broke (%s)" % (k, e), flush=True)
        if ok:
            resid = dt - a.idle
            residuals.append(resid)
            flag = "  <<< STALL" if resid >= STALL_S else ""
            print("  trial %2d: total=%5.1fs residual=%5.1fs%s" % (k, dt, resid, flag), flush=True)
            if resid >= STALL_S: stalls += 1
        else:
            deaths += 1; stalls += 1
            print("  trial %2d: NO-PROMPT in %.0fs -> severe stall / conn death (settling %.0fs)"
                  % (k, dt, RECONNECT_SETTLE), flush=True)
            try: s.close()
            except OSError: pass
            s = None
            time.sleep(RECONNECT_SETTLE)
    if s is not None:
        try: s.close()
        except OSError: pass

    if a.warm:
        r = one_shot(a.host, a.port, "cat /proc/corewarm")
        print("  final-passes: %s" % r.replace("\n", " ").strip()[:90], flush=True)
        one_shot(a.host, a.port, "cat /proc/corewarm.0")
        print("  DISARMED", flush=True)

    worst = max(residuals) if residuals else 0
    clean = len(residuals) - sum(1 for r in residuals if r >= STALL_S)
    print("=== %s: %d/%d trials STALLED (%d conn-deaths), %d clean, worst residual=%.1fs -> %s ==="
          % (a.label or "result", stalls, a.trials, deaths, clean, worst,
             "DIRTY" if stalls else "CLEAN"), flush=True)
    return 0 if stalls == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
