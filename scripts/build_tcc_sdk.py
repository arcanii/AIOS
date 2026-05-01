#!/usr/bin/env python3
"""
AIOS tcc SDK Builder

Stages musl headers, libc.a, CRT objects, tcc built-in headers,
and libtcc1.a into build-04/tcc-sdk/ for disk image installation.

Usage: python3 scripts/build_tcc_sdk.py [--rebuild-libtcc1]

Requires: build-04/ from a successful ninja build, tcc repo at ../tcc
"""
import os, sys, shutil, subprocess, glob

AIOS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(AIOS, "build-04")
SDK = os.path.join(BUILD, "tcc-sdk")
TCC_REPO = os.path.join(os.path.dirname(AIOS), "tcc")

MUSL_STAGE = os.path.join(BUILD, "projects", "musllibc", "build-temp", "stage")
MUSL_INCLUDE = os.path.join(MUSL_STAGE, "include")
MUSL_LIB = os.path.join(MUSL_STAGE, "lib")
AIOS_CRT = os.path.join(BUILD, "lib")

def check_prereqs():
    ok = True
    for p, desc in [
        (MUSL_INCLUDE, "musl headers"),
        (MUSL_LIB, "musl libraries"),
        (AIOS_CRT, "AIOS CRT objects"),
        (TCC_REPO, "tcc repository"),
    ]:
        if not os.path.isdir(p):
            print(f"FAIL -- missing {desc}: {p}")
            ok = False
    if not ok:
        sys.exit(1)

def stage_musl_headers():
    dst = os.path.join(SDK, "usr", "include")
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(MUSL_INCLUDE, dst)
    count = sum(len(files) for _, _, files in os.walk(dst))
    print(f"  /usr/include: {count} headers")

def stage_musl_libs():
    dst = os.path.join(SDK, "usr", "lib")
    os.makedirs(dst, exist_ok=True)
    # libc.a -- single-member archive wrapping libaios_tcc.o
    # The on-AIOS tcc cannot parse the multi-member augmented libc.a
    # (1418 entries with duplicate names trip TCCs archive parser).
    # Wrapping the pre-linked relocatable blob in a one-member ar
    # gives tcc a libc.a that resolves the same symbols via -lc.
    blob = os.path.join(BUILD, "libaios_tcc.o")
    libc_dst = os.path.join(dst, "libc.a")
    if os.path.exists(blob):
        # Remove any stale archive first so ar starts fresh
        if os.path.exists(libc_dst):
            os.unlink(libc_dst)
        r = subprocess.run(
            ["aarch64-linux-gnu-ar", "rcs", libc_dst, blob],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            print(f"FAIL -- ar libc.a:\n{r.stderr}")
            sys.exit(1)
        sz = os.path.getsize(libc_dst)
        print(f"  /usr/lib/libc.a (blob-wrapped, {sz} bytes)")
        # Keep the multi-member augmented archive available for callers
        # that can parse it (eg gcc/host tooling). Not used by on-AIOS tcc.
        full = os.path.join(BUILD, "libaios_full.a")
        if os.path.exists(full):
            shutil.copy2(full, os.path.join(dst, "libc_full.a"))
            print(f"  /usr/lib/libc_full.a (multi-member, {os.path.getsize(full)} bytes)")
    else:
        libaios = os.path.join(BUILD, "libaios_full.a")
        if os.path.exists(libaios):
            shutil.copy2(libaios, libc_dst)
            print(f"  /usr/lib/libc.a (multi-member fallback, {os.path.getsize(libc_dst)} bytes)")
            print(f"  WARN -- on-AIOS tcc will not parse this; build libaios_tcc.o first")
        else:
            src = os.path.join(MUSL_LIB, "libc.a")
            if os.path.exists(src):
                shutil.copy2(src, dst)
                print(f"  /usr/lib/libc.a (musl-only fallback, {os.path.getsize(libc_dst)} bytes)")
    # crt1.o -- custom CRT with __aios_entry init
    # Built by scripts/build_libaios.py
    aios_crt1 = os.path.join(BUILD, "aios_crt1.o")
    if os.path.exists(aios_crt1):
        shutil.copy2(aios_crt1, os.path.join(dst, "crt1.o"))
        print(f"  /usr/lib/crt1.o (aios_crt1, AIOS init)")
    else:
        crt0 = os.path.join(AIOS_CRT, "crt0.o")
        if os.path.exists(crt0):
            shutil.copy2(crt0, os.path.join(dst, "crt1.o"))
            print(f"  /usr/lib/crt1.o (crt0.o fallback, no AIOS init)")
    for crt in ["crti.o", "crtn.o"]:
        src = os.path.join(AIOS_CRT, crt)
        if os.path.exists(src):
            shutil.copy2(src, dst)
            print(f"  /usr/lib/{crt}")
    # libaios_tcc.o -- pre-linked relocatable blob with libc + AIOS
    # runtime symbols. Built by scripts/build_tcc_libc_blob.py.
    # Lets the on-AIOS tcc link programs without parsing libc.a.
    blob = os.path.join(BUILD, "libaios_tcc.o")
    if os.path.exists(blob):
        shutil.copy2(blob, os.path.join(dst, "libaios_tcc.o"))
        sz = os.path.getsize(blob)
        print(f"  /usr/lib/libaios_tcc.o ({sz} bytes, pre-linked libc blob)")
    else:
        print(f"  WARN -- libaios_tcc.o not found; run build_tcc_libc_blob.py")

def stage_tcc_headers():
    dst = os.path.join(SDK, "usr", "lib", "tcc", "include")
    os.makedirs(dst, exist_ok=True)
    tcc_inc = os.path.join(TCC_REPO, "include")
    count = 0
    for h in glob.glob(os.path.join(tcc_inc, "*.h")):
        shutil.copy2(h, dst)
        count += 1
    print(f"  /usr/lib/tcc/include: {count} tcc headers")

def build_libtcc1():
    dst = os.path.join(SDK, "usr", "lib")
    os.makedirs(dst, exist_ok=True)
    libtcc1_a = os.path.join(dst, "libtcc1.a")
    tcc_inc = os.path.join(TCC_REPO, "include")
    CC = "aarch64-linux-gnu-gcc"
    AR = "aarch64-linux-gnu-ar"
    tmpdir = "/tmp/aios_libtcc1_build"
    os.makedirs(tmpdir, exist_ok=True)
    objs = []
    # libtcc1.c -- generic runtime helpers
    src1 = os.path.join(TCC_REPO, "lib", "libtcc1.c")
    obj1 = os.path.join(tmpdir, "libtcc1.o")
    cmd1 = [CC, "-c", src1, "-I", tcc_inc,
            "-O2", "-Wall", "-o", obj1]
    r = subprocess.run(cmd1, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL -- compile libtcc1.c:\n{r.stderr}")
        sys.exit(1)
    objs.append(obj1)
    # lib-arm64.c -- AArch64 128-bit float runtime
    src2 = os.path.join(TCC_REPO, "lib", "lib-arm64.c")
    obj2 = os.path.join(tmpdir, "lib-arm64.o")
    cmd2 = [CC, "-c", src2, "-I", tcc_inc,
            "-O2", "-Wno-implicit-function-declaration", "-o", obj2]
    r = subprocess.run(cmd2, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL -- compile lib-arm64.c:\n{r.stderr}")
        sys.exit(1)
    objs.append(obj2)
    # Archive
    cmd3 = [AR, "rcs", libtcc1_a] + objs
    r = subprocess.run(cmd3, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL -- ar libtcc1.a:\n{r.stderr}")
        sys.exit(1)
    sz = os.path.getsize(libtcc1_a)
    print(f"  /usr/lib/libtcc1.a ({sz} bytes)")
    # Also install in tcc dir for {B} path resolution
    tcc_dst = os.path.join(SDK, "usr", "lib", "tcc")
    os.makedirs(tcc_dst, exist_ok=True)
    shutil.copy2(libtcc1_a, tcc_dst)
    print(f"  /usr/lib/tcc/libtcc1.a (copy)")
    # Cleanup
    shutil.rmtree(tmpdir, ignore_errors=True)

def main():
    check_prereqs()
    print(f"Staging tcc SDK in {SDK}")
    os.makedirs(SDK, exist_ok=True)
    stage_musl_headers()
    stage_musl_libs()
    stage_tcc_headers()
    build_libtcc1()
    total = sum(os.path.getsize(os.path.join(dp, f))
                for dp, _, fns in os.walk(SDK) for f in fns)
    print(f"SDK total: {total // 1024} KB")
    print("OK -- tcc SDK staged")

if __name__ == "__main__":
    main()
