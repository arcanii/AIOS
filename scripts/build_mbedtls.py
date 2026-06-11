#!/usr/bin/env python3
"""
build_mbedtls.py -- cross-compile mbedTLS crypto into build-04/libmbedcrypto.a

This recovers the (gitignored, easily-lost) crypto archive that the AIOS SSH
server links against. The archive itself is NOT committed -- this script is, so
it can never be "lost" again. Run it after a fresh checkout / re-clone of
mbedTLS, or after a clean rm -rf build-04 (once ninja has rebuilt the toolchain).

What it does:
  1. Idempotently ensure the AIOS custom config in mbedTLS mbedtls_config.h:
       DISABLE  MBEDTLS_FS_IO NET_C TIMING_C AESNI_C PADLOCK_C AESCE_C
                PSA_ITS_FILE_C PSA_CRYPTO_STORAGE_C
       ENABLE   MBEDTLS_NO_PLATFORM_ENTROPY MBEDTLS_ENTROPY_HARDWARE_ALT
     (verify only; applies + prints a diff only if something is off.)
  2. Read the src_crypto file list from mbedTLS library/CMakeLists.txt, so this
     tracks the upstream version (v3.6.3 = 81 files) instead of a hard-coded list.
  3. Compile each via scripts/aios-cc -c (so the flags never drift from the
     toolchain wrapper), plus the AIOS /dev/urandom entropy shim
     (src/ssh/aios_entropy.c, which provides mbedtls_hardware_poll).
  4. Archive everything with the cross ar into build-04/libmbedcrypto.a.

The arm_neon.h gotcha: aios-cc compiles with -nostdinc, which hides GCC's
arm_neon.h that mbedTLS common.h pulls in. We add just that one dir back with
  -isystem $(aarch64-linux-gnu-gcc -print-file-name=include)

Expected result: ~82 objects (81 crypto + entropy shim), libmbedcrypto.a ~1.18 MB.

Usage:
  python3 scripts/build_mbedtls.py            # verify config, build
  python3 scripts/build_mbedtls.py --apply    # apply config edits if needed
"""
import os, sys, re, subprocess, time
from concurrent.futures import ThreadPoolExecutor

AIOS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(AIOS, "build-04")
MBEDTLS = os.path.join(os.environ.get("AIOS_DEPS_ROOT", os.path.dirname(AIOS)), "mbedtls")
CONFIG = os.path.join(MBEDTLS, "include", "mbedtls", "mbedtls_config.h")
CMAKELISTS = os.path.join(MBEDTLS, "library", "CMakeLists.txt")
AIOS_CC = os.path.join(AIOS, "scripts", "aios-cc")
ENTROPY_SHIM = os.path.join(AIOS, "src", "ssh", "aios_entropy.c")
ARCHIVE = os.path.join(BUILD, "libmbedcrypto.a")
CROSS_AR = "aarch64-linux-gnu-ar"
CC = "aarch64-linux-gnu-gcc"

DISABLE = ["MBEDTLS_FS_IO", "MBEDTLS_NET_C", "MBEDTLS_TIMING_C",
           "MBEDTLS_AESNI_C", "MBEDTLS_PADLOCK_C", "MBEDTLS_AESCE_C",
           "MBEDTLS_PSA_ITS_FILE_C", "MBEDTLS_PSA_CRYPTO_STORAGE_C"]
ENABLE = ["MBEDTLS_NO_PLATFORM_ENTROPY", "MBEDTLS_ENTROPY_HARDWARE_ALT"]


def active_define(text, sym):
    """True if SYM has an uncommented #define line."""
    return re.search(r"(?m)^[ \t]*#define[ \t]+" + re.escape(sym) + r"\b", text) is not None


def ensure_config(apply_edits):
    if not os.path.isfile(CONFIG):
        sys.exit("FAIL -- mbedtls config not found: " + CONFIG)
    text = open(CONFIG).read()
    orig = text
    problems = []
    for sym in DISABLE:
        if active_define(text, sym):
            problems.append("should be DISABLED: " + sym)
            # comment out the active #define
            text = re.sub(r"(?m)^([ \t]*)#define([ \t]+" + re.escape(sym) + r"\b)",
                          r"\1//#define\2", text)
    for sym in ENABLE:
        if not active_define(text, sym):
            problems.append("should be ENABLED: " + sym)
            # uncomment an existing //#define, else there is nothing to flip
            new = re.sub(r"(?m)^([ \t]*)//[ \t]*#define([ \t]+" + re.escape(sym) + r"\b)",
                         r"\1#define\2", text)
            if new == text:
                sys.exit("FAIL -- no //#define line for " + sym +
                         " in config; cannot enable automatically.")
            text = new

    if not problems:
        print("  config: OK (all 8 disables commented, both enables active)")
        return
    print("  config NEEDS fixes:")
    for p in problems:
        print("    - " + p)
    if not apply_edits:
        sys.exit("FAIL -- config not in AIOS state. Re-run with --apply to fix, "
                 "or edit " + CONFIG + " manually.")
    if text != orig:
        open(CONFIG, "w").write(text)
        print("  config: applied AIOS edits to mbedtls_config.h")
    # re-verify
    text = open(CONFIG).read()
    for sym in DISABLE:
        assert not active_define(text, sym), sym + " still active after apply"
    for sym in ENABLE:
        assert active_define(text, sym), sym + " still inactive after apply"
    print("  config: verified after apply")


def crypto_sources():
    """Parse the src_crypto( ... ) list from mbedTLS library/CMakeLists.txt."""
    text = open(CMAKELISTS).read()
    m = re.search(r"set\(src_crypto(.*?)\)", text, re.S)
    if not m:
        sys.exit("FAIL -- could not find set(src_crypto ...) in " + CMAKELISTS)
    files = re.findall(r"([A-Za-z0-9_]+\.c)", m.group(1))
    missing = [f for f in files if not os.path.isfile(os.path.join(MBEDTLS, "library", f))]
    if missing:
        sys.exit("FAIL -- src_crypto files missing: " + ", ".join(missing))
    return files


def main():
    apply_edits = "--apply" in sys.argv
    t0 = time.time()
    print("=== build_mbedtls.py ===")

    # sanity
    for p in [MBEDTLS, AIOS_CC, ENTROPY_SHIM]:
        if not os.path.exists(p):
            sys.exit("FAIL -- missing prerequisite: " + p)
    if not os.path.isdir(BUILD):
        sys.exit("FAIL -- build-04 not configured; run ninja first.")

    ver = {}
    bi = open(os.path.join(MBEDTLS, "include", "mbedtls", "build_info.h")).read()
    for k in ("MAJOR", "MINOR", "PATCH"):
        mm = re.search(r"MBEDTLS_VERSION_" + k + r"\s+(\d+)", bi)
        ver[k] = mm.group(1) if mm else "?"
    print("  mbedTLS version: %s.%s.%s" % (ver["MAJOR"], ver["MINOR"], ver["PATCH"]))

    ensure_config(apply_edits)

    srcs = crypto_sources()
    print("  src_crypto: %d files" % len(srcs))

    gcc_inc = subprocess.check_output([CC, "-print-file-name=include"]).decode().strip()
    tmp = "/tmp/aios_mbedtls_build"
    os.makedirs(tmp, exist_ok=True)

    # full compile command for one source -> object via aios-cc -c
    extra = ["-I", os.path.join(MBEDTLS, "include"),
             "-isystem", gcc_inc, "-DMBEDTLS_ALLOW_PRIVATE_ACCESS"]

    jobs = []  # (src_path, obj_path)
    for c in srcs:
        jobs.append((os.path.join(MBEDTLS, "library", c),
                     os.path.join(tmp, "mbed_" + c[:-2] + ".o")))
    jobs.append((ENTROPY_SHIM, os.path.join(tmp, "aios_entropy.o")))

    def compile_one(job):
        src, obj = job
        cmd = [AIOS_CC, "-c", src, "-o", obj] + extra
        r = subprocess.run(cmd, capture_output=True, text=True)
        return (src, obj, r.returncode, r.stderr)

    print("  compiling %d objects ..." % len(jobs))
    fails = []
    with ThreadPoolExecutor(max_workers=min(16, os.cpu_count() or 4)) as ex:
        for src, obj, rc, err in ex.map(compile_one, jobs):
            if rc != 0:
                fails.append((src, err))
    if fails:
        for src, err in fails:
            print("\nFAIL compiling " + src)
            for l in err.strip().splitlines()[-8:]:
                print("  " + l)
        sys.exit(1)

    # archive
    objs = [obj for _, obj in jobs]
    if os.path.exists(ARCHIVE):
        os.remove(ARCHIVE)
    r = subprocess.run([CROSS_AR, "rcs", ARCHIVE] + objs,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("FAIL -- ar:\n" + r.stderr)

    size = os.path.getsize(ARCHIVE)
    nsyms = len(subprocess.check_output(
        [CROSS_AR, "t", ARCHIVE]).decode().splitlines())
    dt = time.time() - t0
    print("\n  %s" % ARCHIVE)
    print("  %d objects, %d bytes (%d KB)" % (nsyms, size, size // 1024))
    print("=== OK (%.1fs) ===" % dt)


if __name__ == "__main__":
    main()
