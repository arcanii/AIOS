#!/usr/bin/env python3
# HW cache-coherency soak for the bulk file write path (PIPE_PWRITE_BULK, v0.4.298)
# on the real RPi4 -- the #1 risk QEMU is blind to ([[feedback_pipe_shm_cache]]).
#
# The bulk path shares a frame across vspaces: pipe_server (core 0) bounce-maps the
# client's source buffer CACHEABLE and reads it to vfs_pwrite. If the client wrote
# that buffer on a DIFFERENT A72 core and the cacheable mapping is not coherent, the
# server reads stale / zero bytes and the file is silently wrong -- exactly the
# v0.4.164 pipe-SHM class of bug. /proc/coresched.1 lands user procs on cores 1-3
# while pipe_server stays on core 0, so every bulkwrite run exercises the cross-core
# client-write -> server-read coherency.
#
# Run AFTER a full SD reflash (disk/sdcard-rpi4.img, build 2934+) -- /bin/bulkwrite,
# cp, sha256sum all carry the new libaios. No /tmp push needed.
#
# SIGNALS (all must hold for a PASS):
#   * bulkwrite PASS on every soak run        -- its internal memcmp is byte-exact
#   * cp + sha256sum: copy sha == source sha  -- an independent real-tool path
#   * /proc/pwritebulk calls>0, mapfail=0, denied=0 -- the bulk path actually ran
# netconsole wedges under coresched+load, so drive ONE command per fresh connection
# with retries (the shmring HW soak pattern).
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pi_filexfer as fx

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.8"
REPS = int(sys.argv[2]) if len(sys.argv) > 2 else 12     # cross-core soak iterations
SEQN = 100000                                            # seq 1 100000 -> 588895 bytes
SEQ_BYTES = "588895"


def one(cmd, to=120, tries=6):
    """Run ONE command on a FRESH netconsole connection; retry on wedge."""
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


results = []
def check(tag, ok, detail=""):
    results.append(bool(ok))
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", tag,
                           ("  -- " + detail) if detail else ""), flush=True)


print("=== bulk file write HW coherency soak on %s (reps=%d) ===" % (HOST, REPS), flush=True)
print("ver :", one("cat /proc/version", to=30), flush=True)

# --- baseline: feature OFF (default) ---
base = ctrs(one("cat /proc/pwritebulk", to=30))
check("default OFF (enabled=0, calls=0)", base["enabled"] == 0 and base["calls"] == 0,
      str(base))
off = one("bulkwrite", to=180)
check("bulkwrite OFF byte-exact (legacy)", "BULKWRITE: PASS" in off, off.splitlines()[-1:])

# --- arm bulk + cross-core placement ---
arm = one("cat /proc/pwritebulk.1", to=30)
check("arm /proc/pwritebulk.1", "enabled=1" in arm, arm)
print("coresched.1:", one("cat /proc/coresched.1", to=30), flush=True)

# --- COHERENCY SOAK: repeated cross-core bulkwrite, each internally byte-exact ---
npass = 0
for i in range(REPS):
    out = one("bulkwrite", to=180)
    ok = "BULKWRITE: PASS" in out
    npass += 1 if ok else 0
    if not ok:
        print("  run %d FAIL: %r" % (i, out[-160:]), flush=True)
check("cross-core bulkwrite soak %d/%d byte-exact" % (npass, REPS), npass == REPS)

# --- independent real-tool path: cp a large file, sha256 must match the source ---
one("rm -f /tmp/src /tmp/dst", to=30)
one("seq 1 %d > /tmp/src" % SEQN, to=120)
src_sha = one("sha256sum /tmp/src", to=120)
m = re.search(r"\b([0-9a-f]{64})\b", src_sha)
src_hash = m.group(1) if m else None
one("cp /tmp/src /tmp/dst", to=120)
dst_sha = one("sha256sum /tmp/dst", to=120)
m = re.search(r"\b([0-9a-f]{64})\b", dst_sha)
dst_hash = m.group(1) if m else None
check("cp+sha256: copy == source (byte-for-byte across cores)",
      src_hash is not None and src_hash == dst_hash,
      "src=%s dst=%s" % (src_hash, dst_hash))

# --- the bulk path actually engaged + no soft failures ---
fin = ctrs(one("cat /proc/pwritebulk", to=30))
print("counters:", fin, flush=True)
check("bulk engaged (calls>0, bytes>0)", bool(fin["calls"]) and fin["calls"] > 0
      and bool(fin["bytes"]) and fin["bytes"] > 0, str(fin))
check("no mapfail / no denied (coherent maps, authz clean)",
      fin["mapfail"] == 0 and fin["denied"] == 0, str(fin))

# --- disarm + health ---
print("pwritebulk.0:", one("cat /proc/pwritebulk.0", to=30), flush=True)
print("coresched.0 :", one("cat /proc/coresched.0", to=30), flush=True)

ok = results and all(results)
print("\n=== HW bulk-write coherency soak: %d/%d checks ===" %
      (sum(1 for r in results if r), len(results)), flush=True)
if ok:
    print(">>> PASS: byte-exact across cores WITH the bulk path engaged (calls>0).")
    print(">>> The A72 cross-vspace cacheable coherency (#1 risk) HOLDS on real hardware.")
else:
    print(">>> FAIL or INCONCLUSIVE -- inspect the checks above before flashing default-ON.")
sys.exit(0 if ok else 1)
