#!/usr/bin/env python3
# Recover the Pi: disarm coresched + shmring (left armed when netconsole wedged),
# confirm health. Retries while netconsole self-heals (getty respawn).
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pi_filexfer as fx
HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.8"

print("waiting 20s for netconsole to self-heal...", flush=True)
time.sleep(20)
nc = None
for a in range(10):
    try:
        nc = fx.NC(HOST); nc.expect(b"aios# ", to=12)
        print("connected (attempt %d)" % (a + 1), flush=True); break
    except Exception as e:
        print("connect retry %d: %s" % (a + 1, e), flush=True); time.sleep(12)
if nc is None:
    print("FAIL: netconsole still unreachable -- the Pi may need a power-cycle or serial reboot")
    sys.exit(2)

def run(c, to=40):
    try: out = nc.cmd(c, to).decode("utf-8","replace").strip()
    except Exception as e: out = "<ERR %s>" % e
    print("$ %s\n  -> %r" % (c, out), flush=True); return out

run("cat /proc/coresched.0")   # pin procs back to core 0 (undo distribution)
run("cat /proc/shmring.0")     # disarm the ring
run("cat /proc/coresched")
run("cat /proc/shmring")
v = run("cat /proc/version")
h = run("echo alive:$(/tmp/seq 1 3 | /tmp/wc -l)")
nc.close()
print("\n=== Pi recovered: %s ; smoke=%r ===" % ("OK" if "v0.4.258" in v else "??", h))
