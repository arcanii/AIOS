#!/usr/bin/env python3
"""Host golden-CL gate for the V3D emitters (Phase 2 clear + Phase 3 triangle).

Compiles src/gpu/v3d_cl.c + tools/v3d_host_clgen.c on the host, runs it to emit our
control lists + GL Shader State records, and byte-diffs them against the captured
golden fixtures (tests/fixtures/v3d_{clear,triangle}_*.golden). Since QEMU has zero
V3D model, this is the only correctness check that exists before the Pi -- so a
CL-touching change must pass it before any kernel flash. The triangle set also gates
the indirect-buffer sub-objects (shader record, attribute records, generic tile list)
which no CL stream contains -- a bug there would otherwise reach HW unverified.
OK/FAIL, nonzero exit on FAIL.

Usage: v3d_clcheck.py        (no args)
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CL_SRC = os.path.join(ROOT, "src", "gpu", "v3d_cl.c")
CLGEN = os.path.join(ROOT, "tools", "v3d_host_clgen.c")
INC = os.path.join(ROOT, "src", "gpu")
FIX = os.path.join(ROOT, "tests", "fixtures")
# emitted-file basename (written by clgen into the outdir) -> golden fixture
GOLDEN = {
    "clear_bin.bin":     os.path.join(FIX, "v3d_clear_bin.golden"),
    "clear_render.bin":  os.path.join(FIX, "v3d_clear_render.golden"),
    "tri_bin.bin":       os.path.join(FIX, "v3d_triangle_bin.golden"),
    "tri_render.bin":    os.path.join(FIX, "v3d_triangle_render.golden"),
    "tri_record.bin":    os.path.join(FIX, "v3d_triangle_record.golden"),
    "tri_attr0.bin":     os.path.join(FIX, "v3d_triangle_attr0.golden"),
    "tri_attr1.bin":     os.path.join(FIX, "v3d_triangle_attr1.golden"),
    "tri_tilelist.bin":  os.path.join(FIX, "v3d_triangle_tilelist.golden"),
}

fails = 0


def check(name, ok, detail=""):
    global fails
    if not ok:
        fails += 1
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  -- " + detail) if detail else ""))


def hexdump_diff(ours, gold):
    # first differing offset plus a short context window, both sides
    n = max(len(ours), len(gold))
    for i in range(n):
        a = ours[i] if i < len(ours) else None
        b = gold[i] if i < len(gold) else None
        if a != b:
            lo = max(0, i - 4)
            hi = min(n, i + 8)
            ah = " ".join("%02x" % ours[j] for j in range(lo, min(hi, len(ours))))
            bh = " ".join("%02x" % gold[j] for j in range(lo, min(hi, len(gold))))
            return "first diff at byte 0x%x (len ours=%d gold=%d)\n      ours: %s\n      gold: %s" % (
                i, len(ours), len(gold), ah, bh)
    return "len ours=%d gold=%d" % (len(ours), len(gold))


def main():
    print("=== v3d_clcheck: host golden-CL gate ===")
    cc = os.environ.get("CC", "cc")
    td = tempfile.mkdtemp(prefix="v3d_clcheck_")
    exe = os.path.join(td, "clgen")

    build = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O0", "-g",
             "-I", INC, CLGEN, CL_SRC, "-o", exe]
    r = subprocess.run(build, capture_output=True, text=True)
    check("host build of v3d_cl.c + clgen", r.returncode == 0, r.stderr.strip()[-400:])
    if r.returncode != 0:
        print("=== v3d_clcheck: FAIL (build) ===")
        return 1

    r = subprocess.run([exe, td], capture_output=True, text=True)
    check("clgen run", r.returncode == 0, r.stderr.strip()[-200:])
    if r.returncode != 0:
        print("=== v3d_clcheck: FAIL (run) ===")
        return 1

    for name, golden in GOLDEN.items():
        with open(os.path.join(td, name), "rb") as f:
            ours = f.read()
        with open(golden, "rb") as f:
            gold = f.read()
        ok = ours == gold
        check("%s byte-exact vs golden" % name, ok,
              "" if ok else hexdump_diff(ours, gold))

    print("=== v3d_clcheck: %s ===" % ("PASS" if fails == 0 else "FAIL"))
    return 1 if fails else 0


sys.exit(main())
