#!/usr/bin/env python3
# Cross-core SHM-ring COHERENCY test on the real RPi4 (the #1 risk, QEMU-blind).
# The v0.4.258 kernel (build 2573, ring server code) is already on the Pi; the
# 9836f67 fix is libaios-only, so we just push the rebuilt seq/wc and run with
# /proc/shmring.1 + /proc/coresched.1 so the writer and reader land on DIFFERENT
# A72 cores and touch the SHARED ring frame directly. Exact data + map_ok>0 =>
# the cacheable-inner-shareable mapping + release/acquire/fence barriers hold
# across cores (no all-NUL / stale-index corruption). netconsole wedges under
# coresched+load, so we drive ONE command per fresh connection with retries.
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pi_filexfer as fx

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.8"
SBASE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build-04", "sbase")
N = 100000
EXP_L = "100000"
EXP_C = "588895"   # bytes in seq 1 100000

def push(name):
    print("=== push %s ===" % name, flush=True)
    for a in range(4):
        try:
            if fx.push(os.path.join(SBASE, name), "/tmp/%s" % name, HOST):
                time.sleep(8); return True
        except Exception as e:
            print("  push attempt %d wedged: %s" % (a, e), flush=True)
        time.sleep(12)   # let netconsole self-heal between attempts
    return False

def one(cmd, to=120, tries=6):
    """Run ONE command on a FRESH netconsole connection; retry on wedge."""
    for a in range(tries):
        try:
            nc = fx.NC(HOST); nc.expect(b"aios# ", to=15)
            out = nc.cmd(cmd, to).decode("utf-8", "replace").strip()
            nc.close()
            return out
        except Exception as e:
            last = e
            time.sleep(8)
    return "<UNREACHABLE: %s>" % last

results = []
def check(tag, got, want):
    ok = (got.strip() == want)
    results.append((tag, ok, got))
    print("  [%s] %s  got=%r want=%r" % ("PASS" if ok else "FAIL", tag, got, want), flush=True)

if not push("seq"): print("push seq FAILED"); sys.exit(1)
if not push("wc"):  print("push wc FAILED");  sys.exit(1)

print("--- setup ---", flush=True)
print("chmod:", one("chmod +x /tmp/seq /tmp/wc", to=30), flush=True)
print("ver  :", one("cat /proc/version", to=30), flush=True)
print("arm shmring:", one("cat /proc/shmring.1", to=30), flush=True)
print("arm coresched:", one("cat /proc/coresched.1", to=30), flush=True)

print("--- CROSS-CORE: writer + reader on different A72 cores, direct ring ---", flush=True)
for i in range(3):
    check("xcore seq|wc -l #%d" % i, one("/tmp/seq 1 %d | /tmp/wc -l" % N), EXP_L)
check("xcore seq|wc -c", one("/tmp/seq 1 %d | /tmp/wc -c" % N), EXP_C)

ctr = one("cat /proc/shmring", to=30)
print("counters:", ctr, flush=True)
mapok = 0
for tok in ctr.split():
    if tok.startswith("map_ok="):
        try: mapok = int(tok.split("=")[1])
        except ValueError: pass

print("--- disarm + health ---", flush=True)
print("coresched.0:", one("cat /proc/coresched.0", to=30), flush=True)
print("shmring.0  :", one("cat /proc/shmring.0", to=30), flush=True)
print("ver        :", one("cat /proc/version", to=30), flush=True)

npass = sum(1 for _, ok, _ in results if ok)
print("\n=== HW CROSS-CORE SHM-ring COHERENCY: %d/%d data checks  map_ok=%d ===" %
      (npass, len(results), mapok), flush=True)
if npass == len(results) and mapok > 0:
    print(">>> PASS: exact data across cores WITH the direct ring engaged (map_ok>0)")
    print(">>> The A72 cross-core coherency (#1 risk) HOLDS on real hardware.")
elif mapok == 0:
    print(">>> INCONCLUSIVE: map_ok=0 -- direct ring did NOT engage (server-mediated); coherency NOT exercised.")
else:
    print(">>> FAIL: data mismatch with the direct ring -- a cross-core coherency/barrier bug.")
    for tag, ok, got in results:
        if not ok: print("   FAIL %s -> %r" % (tag, got))
sys.exit(0 if (npass == len(results) and mapok > 0) else 1)
