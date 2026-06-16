#!/usr/bin/env python3
# Push the rebuilt (v0.4.258, fixed ring client) test tools to the Pi's /tmp
# (NOT /bin -- no clobber, fully recoverable) + a minimal exec/pipe smoke.
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pi_filexfer as fx

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.8"
SBASE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build-04", "sbase")
TOOLS = ["seq", "wc", "cat", "sha256sum"]

for t in TOOLS:
    print("=== push %s ===" % t, flush=True)
    if not fx.push(os.path.join(SBASE, t), "/tmp/%s" % t, HOST):
        print("PUSH FAILED: %s" % t); sys.exit(1)
    time.sleep(3)   # settle between netconsole connections

nc = fx.NC(HOST); nc.expect(b"aios# ")
def run(c, to=60):
    out = nc.cmd(c, to).decode("utf-8", "replace").strip()
    print("$ %s\n  -> %r" % (c, out), flush=True)
    return out

run("chmod +x /tmp/seq /tmp/wc /tmp/cat /tmp/sha256sum")
run("/proc/version" if False else "cat /proc/version")
smoke = run("/tmp/seq 1 5 | /tmp/wc -l")
nc.close()
print("\n=== SMOKE: seq 1 5 | wc -l == %r (want '5') -> %s ===" %
      (smoke, "PASS" if smoke.strip() == "5" else "FAIL"))
sys.exit(0 if smoke.strip() == "5" else 1)
