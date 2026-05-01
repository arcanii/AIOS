#!/usr/bin/env python3
"""
AIOS TCC libc blob builder

Produces a single pre-linked relocatable object (libaios_tcc.o) that
contains the AIOS runtime + the libc subset reachable from
src/lib/aios_tcc_anchor.c. The on-AIOS tcc links this single .o
into compiled programs, bypassing TCC's archive parser (which
cannot handle libc.a -- 1418 members with duplicate names).

Usage: python3 scripts/build_tcc_libc_blob.py

Inputs:
  src/lib/aios_tcc_anchor.c (anchor symbol references)
  build-04/projects/...     (AIOS + musl + sel4 archives)

Output:
  build-04/libaios_tcc.o    (~600 KB relocatable, single .o file)
"""
import os, sys, subprocess

AIOS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(AIOS, "build-04")
ANCHOR_SRC = os.path.join(AIOS, "src", "lib", "aios_tcc_anchor.c")
ANCHOR_OBJ = os.path.join(BUILD, "aios_tcc_anchor.o")
OUTPUT = os.path.join(BUILD, "libaios_tcc.o")

CC = "aarch64-linux-gnu-gcc"

# Same library order as scripts/aios-cc, with --start-group/--end-group
# resolving cycles. aios_stubs.o is intentionally omitted: the real
# uart_init / sel4platsupport_arch_copy_irq_cap from libplatsupport
# are pulled in instead, and the duplicate stubs would conflict.
LIBS = [
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


def check_prereqs():
    ok = True
    if not os.path.exists(ANCHOR_SRC):
        print(f"FAIL -- missing anchor source: {ANCHOR_SRC}")
        ok = False
    for rel in LIBS:
        full = os.path.join(BUILD, rel)
        if not os.path.exists(full):
            print(f"FAIL -- missing archive: {rel}")
            ok = False
    if not ok:
        sys.exit(1)


def compile_anchor():
    """Compile the anchor source with AIOS includes (musl headers)."""
    cmd = [
        CC,
        "-c", ANCHOR_SRC,
        "-o", ANCHOR_OBJ,
        "-nostdinc",
        "-static",
        "-fno-pic", "-fno-pie",
        "-fno-stack-protector",
        "-mstrict-align",
        "-mno-outline-atomics",
        "-ftls-model=local-exec",
        "-O0", "-fno-builtin",
        "-I", os.path.join(BUILD, "projects", "musllibc", "build-temp", "stage", "include"),
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL -- compile anchor:\n{r.stderr}")
        sys.exit(1)
    sz = os.path.getsize(ANCHOR_OBJ)
    print(f"  aios_tcc_anchor.o: {sz} bytes")


def link_blob():
    """Run ld -r over the AIOS runtime + libc archives."""
    cmd = [
        CC,
        "-nostdlib", "-static",
        "-Wl,-r",
        "-Wl,--no-eh-frame-hdr",
        # Note: --wrap=main is intentionally NOT set here. Setting it
        # during ld -r rewrites every "main" reference inside the blob
        # (notably __aios_entry -> main) to __wrap_main, which then
        # tail-calls a weak __real_main. User programs link without
        # --wrap, so their main stays as "main" and __real_main is
        # NULL at runtime, leading to a silent crash. The aios-cc CLI
        # workflow does want --wrap=main, but that link path bypasses
        # this blob entirely.
        # The pthread / pwd / grp wraps from aios-cc are similarly
        # only meaningful at the final user link, not during -r.
        # Force-undefined anchors that must be present even if no caller.
        "-Wl,-umuslcsys_init_muslc",
        "-Wl,--undefined=arm_gic_ptr",
        "-Wl,--undefined=tegra_ictlr_ptr",
        "-Wl,--undefined=arm_gicv3_ptr",
        "-Wl,--undefined=ti_omap3_ptr",
        "-Wl,-u", "-Wl,__vsyscall_ptr",
        # Drop the explicit fsl_avic_ptr undef -- the anchor now
        # provides a weak fallback so tcc does not flag it.
        ANCHOR_OBJ,
        "-Wl,--start-group",
        "-lgcc", "-lgcc_eh",
    ]
    cmd += [os.path.join(BUILD, rel) for rel in LIBS]
    cmd += [
        "-Wl,--end-group",
        "-o", OUTPUT,
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL -- ld -r:\n{r.stderr}")
        sys.exit(1)
    sz = os.path.getsize(OUTPUT)
    print(f"  libaios_tcc.o: {sz:,} bytes (pre-localize)")


def localize_crt_symbols():
    """sel4runtime crti.S provides _init / _fini, so does the system
    crti.o that tcc adds at link time. Localize the blobs versions to
    avoid duplicate-symbol errors when the user links against crti.o."""
    cmd = [
        "aarch64-linux-gnu-objcopy",
        # _init and _fini come from sel4runtime crti.S.obj inside the
        # blob and from the system /usr/lib/crti.o that tcc adds.
        "--localize-symbol=_init",
        "--localize-symbol=_fini",
        # __sel4_start_c is defined by both sel4runtime crt1.c (pulled
        # into the blob) and aios_crt1.S (the user-link CRT). Prefer
        # aios_crt1.S, which uses bl-only relocations tcc handles.
        "--localize-symbol=__sel4_start_c",
        OUTPUT,
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL -- objcopy localize:\n{r.stderr}")
        sys.exit(1)
    sz = os.path.getsize(OUTPUT)
    print(f"  libaios_tcc.o: {sz:,} bytes (post-localize)")


def report():
    """Show defined and remaining undefined symbols."""
    r = subprocess.run(["aarch64-linux-gnu-nm", OUTPUT],
                       capture_output=True, text=True)
    defined = []
    undef = []
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] == "U":
            undef.append(parts[-1])
        elif len(parts) >= 3 and parts[-2] in ("T", "t"):
            defined.append(parts[-1])

    # _start lives in aios_crt1.o (separate file, linked at user link time).
    must_have = [
        "__sel4_start_c", "__sel4runtime_load_env",
        "__aios_entry", "muslcsys_init_muslc",
        "printf", "malloc", "free", "exit", "strlen", "memcpy",
    ]
    missing = [s for s in must_have if s not in defined]
    print(f"  Defined symbols (T): {len(defined)}")
    print(f"  Undefined symbols (U): {len(undef)}")
    if undef:
        print(f"    Remaining undefined (will be resolved at user link time):")
        for s in undef:
            print(f"      {s}")
    if missing:
        print(f"  WARN -- missing critical symbols: {missing}")
    else:
        print(f"  OK -- all critical symbols present")


def main():
    check_prereqs()
    print(f"Building TCC libc blob -> {OUTPUT}")
    compile_anchor()
    link_blob()
    localize_crt_symbols()
    report()
    print("OK -- libaios_tcc.o ready")


if __name__ == "__main__":
    main()
