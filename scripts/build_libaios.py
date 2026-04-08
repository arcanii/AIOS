#!/usr/bin/env python3
"""
AIOS Augmented libc Builder (MRI merge)

Merges all AIOS runtime + musl into libaios_full.a via MRI script.
Handles duplicate archive members (musl has duplicate syscall.o).
Also compiles aios_crt1.S and aios_stubs.c.

Usage: python3 scripts/build_libaios.py
"""
import os, sys, subprocess, tempfile

AIOS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(AIOS, "build-04")
OUTPUT = os.path.join(BUILD, "libaios_full.a")
CRT1_SRC = os.path.join(AIOS, "src", "lib", "aios_crt1.S")
CRT1_OUT = os.path.join(BUILD, "aios_crt1.o")
STUBS_SRC = os.path.join(AIOS, "src", "lib", "aios_stubs.c")
STUBS_OUT = os.path.join(BUILD, "aios_stubs.o")

AR = "aarch64-linux-gnu-ar"
AS = "aarch64-linux-gnu-as"
CC = "aarch64-linux-gnu-gcc"

ARCHIVES = [
    "projects/sel4runtime/libsel4runtime.a",
    "libsel4/libsel4.a",
    "projects/seL4_libs/libsel4muslcsys/libsel4muslcsys.a",
    "projects/seL4_libs/libsel4platsupport/libsel4platsupport.a",
    "projects/seL4_libs/libsel4utils/libsel4utils.a",
    "projects/seL4_libs/libsel4simple/libsel4simple.a",
    "projects/seL4_libs/libsel4simple-default/libsel4simple-default.a",
    "projects/seL4_libs/libsel4vka/libsel4vka.a",
    "projects/seL4_libs/libsel4allocman/libsel4allocman.a",
    "projects/aios/libaios_posix.a",
    "projects/seL4_libs/libsel4vspace/libsel4vspace.a",
    "projects/seL4_libs/libsel4debug/libsel4debug.a",
    "projects/util_libs/libplatsupport/libplatsupport.a",
    "projects/util_libs/libfdt/libfdt.a",
    "projects/util_libs/libelf/libelf.a",
    "projects/util_libs/libcpio/libcpio.a",
    "projects/util_libs/libutils/libutils.a",
    "projects/musllibc/build-temp/stage/lib/libc.a",
]


def build_augmented_libc():
    """Merge all archives via MRI script (handles duplicate members)"""
    mri_lines = [f"create {OUTPUT}"]
    for relpath in ARCHIVES:
        full = os.path.join(BUILD, relpath)
        if not os.path.exists(full):
            print(f"FAIL -- missing {relpath}")
            sys.exit(1)
        mri_lines.append(f"addlib {full}")
    # Add stubs object
    if os.path.exists(STUBS_OUT):
        mri_lines.append(f"addmod {STUBS_OUT}")
    mri_lines.append("save")
    mri_lines.append("end")

    mri_script = "\n".join(mri_lines) + "\n"
    r = subprocess.run([AR, "-M"], input=mri_script,
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL -- ar -M: {r.stderr}")
        sys.exit(1)

    sz = os.path.getsize(OUTPUT)
    r2 = subprocess.run([AR, "t", OUTPUT], capture_output=True, text=True)
    count = len([m for m in r2.stdout.strip().split("\n") if m])
    print(f"  libaios_full.a: {sz:,} bytes ({count} objects)")


def build_stubs():
    """Compile aios_stubs.c for missing linker/platform symbols"""
    if not os.path.exists(STUBS_SRC):
        print(f"  No stubs file, skipping")
        return
    aios_cc = os.path.join(AIOS, "scripts", "aios-cc")
    cmd = [aios_cc, "-c", STUBS_SRC, "-o", STUBS_OUT]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL -- compile aios_stubs.c:\n{r.stderr}")
        sys.exit(1)
    sz = os.path.getsize(STUBS_OUT)
    print(f"  aios_stubs.o: {sz} bytes")


def build_aios_crt1():
    """Assemble aios_crt1.S"""
    if not os.path.exists(CRT1_SRC):
        print(f"FAIL -- missing {CRT1_SRC}")
        sys.exit(1)
    cmd = [AS, CRT1_SRC, "-o", CRT1_OUT]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL -- assemble aios_crt1.S:\n{r.stderr}")
        sys.exit(1)
    sz = os.path.getsize(CRT1_OUT)
    print(f"  aios_crt1.o: {sz} bytes")


def verify():
    """Check critical symbols exist in archive"""
    r = subprocess.run(["aarch64-linux-gnu-nm", OUTPUT],
                       capture_output=True, text=True)
    for sym in ["__syscall", "_Exit", "__aios_entry", "__sel4runtime_load_env"]:
        lines = [l for l in r.stdout.splitlines() if f" T {sym}" in l or f" t {sym}" in l]
        if lines:
            print(f"  OK {sym}")
        else:
            print(f"  WARN -- {sym} not defined (T) in archive")


def main():
    print("Building stubs...")
    build_stubs()
    print("Merging archives (MRI)...")
    build_augmented_libc()
    print("Assembling CRT...")
    build_aios_crt1()
    print("Verifying symbols...")
    verify()
    print(f"\nOK -- {OUTPUT}")


if __name__ == "__main__":
    main()
