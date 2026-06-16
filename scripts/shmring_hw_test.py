#!/usr/bin/env python3
# Cross-core SHM-ring coherency test on the real Pi (seq=writer, wc=reader).
# Uses ONE held netconsole connection (gentle). Drives: baseline OFF -> ring ON
# (single core) -> ring ON + coresched (cross-core), checking seq|wc -l == 100000
# and seq|wc -c == 588895 (the exact byte count of "1\n..100000\n"). A cross-core
# barrier/memory-type bug => wrong count, garbage, or a hang (the all-NUL class).
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pi_filexfer as fx

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.8"
N = 100000
EXP_L = "100000"
EXP_C = "588895"   # bytes in seq 1 100000: 488895 digits + 100000 newlines

settle = int(sys.argv[2]) if len(sys.argv) > 2 else 15
print("settling %ds (let netconsole recover from the push wedge)..." % settle, flush=True)
time.sleep(settle)

nc = None
for attempt in range(6):
    try:
        nc = fx.NC(HOST); nc.expect(b"aios# ", to=15); break
    except Exception as e:
        print("connect attempt %d failed: %s; wait 10s" % (attempt + 1, e), flush=True)
        time.sleep(10)
if nc is None:
    print("FAIL: netconsole unreachable"); sys.exit(2)

def run(c, to=90):
    try:
        out = nc.cmd(c, to).decode("utf-8", "replace").strip()
    except Exception as e:
        out = "<TIMEOUT/ERR: %s>" % e
    print("$ %s\n  -> %r" % (c, out), flush=True)
    return out

results = []
def check(tag, got, want):
    ok = (got.strip() == want)
    results.append((tag, ok, got))
    print("  [%s] %s  got=%r want=%r" % ("PASS" if ok else "FAIL", tag, got, want), flush=True)
    return ok

run("cat /proc/version")
run("chmod +x /tmp/seq /tmp/wc")

print("\n--- baseline: ring OFF (legacy path) ---", flush=True)
run("cat /proc/shmring")   # expect enabled=0
check("OFF seq|wc -l", run("/tmp/seq 1 %d | /tmp/wc -l" % N), EXP_L)
check("OFF seq|wc -c", run("/tmp/seq 1 %d | /tmp/wc -c" % N), EXP_C)

print("\n--- ring ON, single core (server still core-0 pinned) ---", flush=True)
run("cat /proc/shmring.1")  # arm
check("ON seq|wc -l", run("/tmp/seq 1 %d | /tmp/wc -l" % N), EXP_L)
check("ON seq|wc -c", run("/tmp/seq 1 %d | /tmp/wc -c" % N), EXP_C)
run("cat /proc/shmring")   # counters: map_ok/wake>0 => ring engaged

print("\n--- ring ON + coresched: WRITER and READER on different cores (THE test) ---", flush=True)
run("cat /proc/coresched.1")  # distribute procs to cores 1-3
for i in range(4):   # repeat -- cross-core coherency bugs can be intermittent
    check("XCORE seq|wc -l #%d" % i, run("/tmp/seq 1 %d | /tmp/wc -l" % N), EXP_L)
check("XCORE seq|wc -c", run("/tmp/seq 1 %d | /tmp/wc -c" % N), EXP_C)
run("cat /proc/shmring")   # final counters

print("\n--- disarm ---", flush=True)
run("cat /proc/coresched.0")
run("cat /proc/shmring.0")
run("cat /proc/version")   # still alive?
nc.close()

npass = sum(1 for _, ok, _ in results if ok)
print("\n=== HW cross-core SHM-ring: %d/%d checks passed ===" % (npass, len(results)), flush=True)
for tag, ok, got in results:
    if not ok:
        print("   FAIL %s -> %r" % (tag, got))
sys.exit(0 if npass == len(results) else 1)
