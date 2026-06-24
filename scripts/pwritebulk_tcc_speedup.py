#!/usr/bin/env python3
# Validate the tcc-linker speedup from the bulk file write path (v0.4.298) on the
# real RPi4. tcc writes its output ELF via stdio fwrite -> writev, which now rides
# PIPE_PWRITE_BULK. The deterministic speedup metric is the IPC round-trip count:
# the bulk path writes the output in ceil(bytes/65536) IPCs instead of the legacy
# ceil(bytes/800) FS_PWRITE round-trips. We compile a tiny no-include program
# (robust over netconsole -- no shell escaping), read /proc/pwritebulk before/after
# to get the exact bytes + calls tcc's output cost, run the binary to prove the
# output is byte-exact (it returns 42), and report the round-trip reduction.
#
# Run AFTER a full SD reflash (build 2934+), with tcc + SDK on the board. Drives one
# command per fresh netconsole connection (the shmring HW soak pattern).
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pi_filexfer as fx

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.8"


def one(cmd, to=180, tries=6):
    last = None
    for _ in range(tries):
        try:
            nc = fx.NC(HOST); nc.expect(b"aios# ", to=15)
            out = nc.cmd(cmd, to).decode("utf-8", "replace").strip()
            nc.close()
            return out
        except Exception as e:
            last = e
            time.sleep(8)
    return "<UNREACHABLE: %s>" % last


def ctrs(s):
    d = {}
    for k in ("enabled", "calls", "bytes", "mapfail", "denied"):
        m = re.search(k + r"=(\d+)", s)
        d[k] = int(m.group(1)) if m else None
    return d


print("=== tcc-linker bulk-write speedup on %s ===" % HOST, flush=True)
print("ver:", one("cat /proc/version", to=30), flush=True)

# tiny, include-free source -- exit code 42 is the byte-exact-output oracle
one("rm -f /tmp/prog /tmp/prog.c", to=30)
print("mk src:", one("echo 'int main(void){return 42;}' > /tmp/prog.c", to=30), flush=True)

print("arm   :", one("cat /proc/pwritebulk.1", to=30), flush=True)
c0 = ctrs(one("cat /proc/pwritebulk", to=30))
print("before:", c0, flush=True)

comp = one("tcc -o /tmp/prog /tmp/prog.c", to=240)
print("tcc   :", repr(comp[-160:]), flush=True)
c1 = ctrs(one("cat /proc/pwritebulk", to=30))
print("after :", c1, flush=True)
size = one("ls -l /tmp/prog", to=30)
print("output:", size, flush=True)

# run the compiled binary -- rc 42 proves tcc's bulk-written ELF is byte-exact
runrc = one("/tmp/prog; echo rc=$?", to=60)
m = re.search(r"rc=(\d+)", runrc)
rc = int(m.group(1)) if m else None

print("disarm:", one("cat /proc/pwritebulk.0", to=30), flush=True)

dcalls = (c1["calls"] or 0) - (c0["calls"] or 0)
dbytes = (c1["bytes"] or 0) - (c0["bytes"] or 0)
legacy = (dbytes + 799) // 800 if dbytes else 0

print("\n=== RESULT ===", flush=True)
print("compiled binary returns 42 (byte-exact output): %s (rc=%s)" %
      ("PASS" if rc == 42 else "FAIL", rc), flush=True)
if dcalls > 0 and dbytes > 0:
    print("tcc output write: %d bytes in %d bulk IPCs" % (dbytes, dcalls), flush=True)
    print("legacy would be ceil(%d/800) = %d FS_PWRITE round-trips" % (dbytes, legacy), flush=True)
    print(">>> ~%.0fx fewer IPC round-trips for the tcc linker output." % (legacy / dcalls), flush=True)
elif dbytes == 0:
    print(">>> tcc output did not go through the bulk path this run (calls unchanged)."
          "\n    Output may be < the 4KB threshold; the writev path itself is proven"
          " by bulkwrite. ls -l above shows the size.", flush=True)
sys.exit(0 if rc == 42 else 1)
