#!/usr/bin/env python3
"""DVFS governor cooling measurement + a resilient Pi driver (wedge recovery).

The Pi is PASSIVELY cooled: slow thermal response (minutes), likely a small Delta.
That actually helps the observer effect -- a brief read-connection (a few seconds
at the boosted clock) barely moves the temperature, so we can sample the cooling
curve periodically and watch it plateau instead of guessing a soak time.

Experiment: compare two steady states over long disconnected soaks --
  COOL = governor ON, idle -> it downclocks to 300 (the new behavior)
  HOT  = governor OFF + clock pinned 600 (the old "holds 600 at idle" behavior)
The Delta = HOT - COOL is the governor's cooling benefit.

WEDGE RECOVERY (the netconsole/TLBI-stall strategy). The wedge is the kernel
~32s TLBI stall (project_stall_hunt): a whole-system freeze that self-clears, NOT
a stuck process -- so the fix is to ride it out, not restart anything. Tiered:
  1. fresh connection per command (a wedged relay shell is sidestepped; getty
     forks a new one)
  2. on timeout, back off PAST the ~32s stall (gaps escalate to 40-60s) and retry
  3. last resort: reboot (BCM2711 watchdog) restarts every wedged process cleanly
A blind pkill is deliberately avoided -- it could kill the netconsole listener and
strand the board (no network path back).
"""
import sys, time, re
sys.path.insert(0, "scripts")
import pi_filexfer as fx

HOST = "192.168.0.8"

class Pi:
    def __init__(self, host=HOST):
        self.host = host
    def _once(self, cmd, to):
        nc = None
        try:
            nc = fx.NC(self.host)
            nc.expect(b"aios# ", to=to)
            out = nc.cmd(cmd, to=to).decode("utf-8", "replace")
            nc.close()
            return out
        finally:
            try:
                if nc: nc.close()
            except Exception:
                pass
    # escalating backoff rides out the ~32s kernel TLBI stall; a fresh connection
    # each try sidesteps a wedged relay shell.
    GAPS = [4, 10, 40, 45, 60]
    def cmd(self, cmd, to=12, tries=5, label=None):
        for i in range(tries):
            try:
                return self._once(cmd, to)
            except Exception as e:
                if i == tries - 1:
                    print("    [%s WEDGED, gave up after %d tries: %s]"
                          % (label or cmd, tries, e), flush=True)
                    return None
                gap = self.GAPS[min(i, len(self.GAPS) - 1)]
                print("    [%s wedged (%s); ride-out %ds, retry %d/%d]"
                      % (label or cmd, e, gap, i + 2, tries), flush=True)
                time.sleep(gap)
        return None
    def alive(self):
        return self.cmd("echo PING", to=10, tries=4, label="alive") is not None

TEMP_RE = re.compile(r"soc_temp_c:\s*([0-9.]+)")
SET_RE  = re.compile(r"\bset=(\d+)")
SETS_RE = re.compile(r"\bsets=(\d+)")
BMIN_RE = re.compile(r"\bbmin=(\d+)")
CUR_RE  = re.compile(r"arm_cur_mhz:\s*(\d+)")

def sample(pi, t0, tag):
    """One brief read: temp + governor state. Returns (temp, cur_mhz, set, sets, bmin)."""
    tp = pi.cmd("cat /proc/temp", to=12, label="temp")
    cf = pi.cmd("cat /proc/cpufreq", to=12, label="cpufreq")
    temp = TEMP_RE.search(tp).group(1) if tp and TEMP_RE.search(tp) else "??"
    cur  = CUR_RE.search(cf).group(1) if cf and CUR_RE.search(cf) else "??"
    sset = SET_RE.search(cf).group(1) if cf and SET_RE.search(cf) else "?"
    sets = SETS_RE.search(cf).group(1) if cf and SETS_RE.search(cf) else "?"
    bmin = BMIN_RE.search(cf).group(1) if cf and BMIN_RE.search(cf) else "?"
    el = int(time.time() - t0)
    print("  [%4ds] %-4s temp=%5s C  cur=%3s set=%3s sets=%-4s bmin=%s"
          % (el, tag, temp, cur, sset, sets, bmin), flush=True)
    return temp

def soak(pi, t0, tag, secs, every):
    """Sample every `every`s for `secs`s (disconnected between samples)."""
    end = secs
    waited = 0
    sample(pi, t0, tag)
    while waited < end:
        nap = min(every, end - waited)
        time.sleep(nap); waited += nap
        sample(pi, t0, tag)

def main():
    hot_s  = int(sys.argv[1]) if len(sys.argv) > 1 else 240
    cool_s = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    every  = int(sys.argv[3]) if len(sys.argv) > 3 else 70
    t0 = time.time()

    print("=== alive check ===", flush=True)
    if not Pi().alive():
        print("Pi unreachable after retries -- may be mid-stall or wedged; "
              "manual reboot may be needed.", flush=True)
        return 1
    pi = Pi()

    # COOL first: the Pi has been idle (governor on) so it is already near the cool
    # plateau; confirm governor on + sample the plateau.
    print("\n=== COOL phase: governor ON (idle -> 300) ===", flush=True)
    pi.cmd("cat /proc/cpufreq.gov.1", to=12, label="gov.on")
    soak(pi, t0, "COOL", cool_s, every)

    # HOT: pin 600 with the governor off (the old hold-600 behavior), watch it heat.
    print("\n=== HOT phase: governor OFF + clock pinned 600 ===", flush=True)
    pi.cmd("cat /proc/cpufreq.gov.0", to=12, label="gov.off")
    pi.cmd("cat /proc/cpufreq.set.600", to=12, label="set600")
    soak(pi, t0, "HOT", hot_s, every)

    # restore: governor back on so the board downclocks + cools after the run.
    print("\n=== restore: governor ON ===", flush=True)
    pi.cmd("cat /proc/cpufreq.gov.1", to=12, label="gov.on")
    sample(pi, t0, "DONE")
    print("\nDone. Compare the COOL plateau vs the HOT plateau (passive cooling: "
          "the last samples of each phase are the settled values).", flush=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())
