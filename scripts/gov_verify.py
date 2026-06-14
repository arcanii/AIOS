#!/usr/bin/env python3
"""DVFS governor HW verification over netconsole -- resilient edition.

The governor bug was holding 600 MHz at idle; success = it downclocks to 300 and
the SoC cools during a DISCONNECTED idle.  Observer effect: a connected relay is
real work, so a connected read boosts the clock.  Cumulative signals that survive
the observer effect and prove repeated downclocking:
  - gov_dbg sets   -- count of clock actuations (grows ~2 per idle/reconnect cycle:
                      300 while idle, then 600 when the relay reconnects)
  - gov_dbg bmin   -- min busy permille ever (near 0 => the metric sees idle)
  - gov_dbg set    -- the clock last actuated (may already read 600 by read time as
                      the reconnect spawn re-boosts; best-effort)
  - /proc/temp     -- thermal lag reflects the just-ended disconnected idle

netconsole is flaky (transient 32s TLBI stalls under the reconnect spawn), so every
read is a fresh connection with retries, one command per connection, printed as it
arrives.  Usage: gov_verify.py <settle_s> <short_idle_s> <cycles> <long_idle_s>
"""
import sys, time
sys.path.insert(0, "scripts")
import pi_filexfer as fx

HOST = "192.168.0.8"

def read1(tag, cmd, keys, retries=4, to=14, gap=8):
    """Fresh connection, one command, print matching lines.  Retry on wedge."""
    for a in range(retries):
        nc = None
        try:
            nc = fx.NC(HOST)
            nc.expect(b"aios# ", to=to)
            out = nc.cmd(cmd, to=to).decode("utf-8", "replace")
            nc.close()
            got = [ln.strip() for ln in out.splitlines()
                   if any(k in ln for k in keys)]
            for ln in got:
                print("  [%s] %s" % (tag, ln), flush=True)
            if got:
                return True
            print("  [%s] (no match; raw=%r)" % (tag, out[:80]), flush=True)
            return False
        except Exception as e:
            print("  [%s] wedged attempt %d/%d: %s" % (tag, a + 1, retries, e), flush=True)
            try:
                if nc: nc.close()
            except Exception:
                pass
            time.sleep(gap)
    return False

CPUFREQ_KEYS = ("arm_cur_mhz", "governor:", "gov_dbg")
TEMP_KEYS    = ("soc_temp_c",)

def snap(label, temp=True):
    print("\n==== %s ====" % label, flush=True)
    read1("freq", "cat /proc/cpufreq", CPUFREQ_KEYS)
    if temp:
        time.sleep(6)                      # gentle gap between connections
        read1("temp", "cat /proc/temp", TEMP_KEYS)

def main():
    settle = int(sys.argv[1]) if len(sys.argv) > 1 else 15
    sidle  = int(sys.argv[2]) if len(sys.argv) > 2 else 18
    cycles = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    lidle  = int(sys.argv[4]) if len(sys.argv) > 4 else 0

    print("[settle %ds]" % settle, flush=True)
    time.sleep(settle)
    snap("INITIAL")
    for c in range(1, cycles + 1):
        print("\n[idle %ds disconnected, cycle %d/%d]" % (sidle, c, cycles), flush=True)
        time.sleep(sidle)
        snap("AFTER %ds IDLE (cycle %d)" % (sidle, c))
    if lidle:
        print("\n[long idle %ds disconnected -- thermal soak]" % lidle, flush=True)
        time.sleep(lidle)
        snap("AFTER %ds LONG IDLE" % lidle)

if __name__ == "__main__":
    main()
