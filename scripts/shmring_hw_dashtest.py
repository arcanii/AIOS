#!/usr/bin/env python3
# Decisive: does running the pipeline under a RING-AWARE dash engage the direct
# userspace ring (map_ok>0)? Pushes /tmp/dash, then runs seq|wc under it.
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pi_filexfer as fx

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.8"
SBASE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build-04", "sbase")

print("=== push /tmp/dash ===", flush=True)
if not fx.push(os.path.join(SBASE, "dash"), "/tmp/dash", HOST):
    print("push dash FAILED"); sys.exit(1)
time.sleep(5)

nc = None
for a in range(6):
    try:
        nc = fx.NC(HOST); nc.expect(b"aios# ", to=15); break
    except Exception as e:
        print("connect retry %d: %s" % (a, e), flush=True); time.sleep(8)
if nc is None:
    print("FAIL: unreachable"); sys.exit(2)

def run(c, to=90):
    try: out = nc.cmd(c, to).decode("utf-8","replace").strip()
    except Exception as e: out = "<ERR %s>" % e
    print("$ %s\n  -> %r" % (c, out), flush=True); return out

run("chmod +x /tmp/dash /tmp/seq /tmp/wc")
run("cat /proc/shmring.1")     # arm
m0 = run("cat /proc/shmring")  # baseline counters
# run the pipeline under the NEW (ring-aware) dash
r1 = run('/tmp/dash -c "/tmp/seq 1 100000 | /tmp/wc -l"', to=120)
m1 = run("cat /proc/shmring")  # did map_ok move?
# cross-core
run("cat /proc/coresched.1")
xc = []
for i in range(3):
    xc.append(run('/tmp/dash -c "/tmp/seq 1 100000 | /tmp/wc -l"', to=120))
xcc = run('/tmp/dash -c "/tmp/seq 1 100000 | /tmp/wc -c"', to=120)
m2 = run("cat /proc/shmring")
run("cat /proc/coresched.0"); run("cat /proc/shmring.0")
run("cat /proc/version")
nc.close()

def mapok(s):
    for tok in s.split():
        if tok.startswith("map_ok="): return int(tok.split("=")[1])
    return -1
print("\n=== VERDICT ===", flush=True)
print("map_ok: before=%d after-1pipe=%d after-xcore=%d" % (mapok(m0), mapok(m1), mapok(m2)))
print("data: single='%s'(want 100000)  xcore=%r(want 100000 x3)  xcore -c='%s'(want 588895)" %
      (r1, xc, xcc))
direct = mapok(m2) > 0
data_ok = (r1 == "100000") and all(x == "100000" for x in xc) and (xcc == "588895")
print("DIRECT RING ENGAGED: %s   DATA EXACT: %s" % (direct, data_ok))
sys.exit(0 if (direct and data_ok) else 1)
