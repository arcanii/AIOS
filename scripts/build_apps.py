#!/usr/bin/env python3
"""
AIOS Application Builder -- single command to rebuild everything

Usage: python3 scripts/build_apps.py [--no-tcc] [--no-sbase] [--no-dash]

Steps:
  1. ninja (incremental kernel + root task + libaios_posix)
  2. sbase (99 Unix tools)
  3. dash (login shell)
  4. tcc (compiler)
  5. libaios + SDK (augmented libc for tcc)
  6. disk image
"""
import os, sys, subprocess, time

AIOS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(AIOS, "build-04")
DASH_SRC = os.path.expanduser("~/Desktop/github_repos/dash/src")
TCC_SRC = os.path.expanduser("~/Desktop/github_repos/tcc")

def run(cmd, label, cwd=None, tail=3):
    print(f"\n--- {label} ---")
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True,
                       cwd=cwd, shell=isinstance(cmd, str))
    dt = time.time() - t0
    if r.returncode != 0:
        out = r.stdout + r.stderr
        lines = out.strip().splitlines()
        for l in lines[-10:]:
            print(f"  {l}")
        print(f"FAIL -- {label} ({dt:.1f}s)")
        return False
    lines = r.stdout.strip().splitlines()
    for l in lines[-tail:]:
        print(f"  {l}")
    print(f"  OK ({dt:.1f}s)")
    return True


def main():
    skip_tcc = "--no-tcc" in sys.argv
    skip_sbase = "--no-sbase" in sys.argv
    skip_dash = "--no-dash" in sys.argv
    t_start = time.time()

    # 1. ninja
    if not run(["ninja"], "ninja (incremental)", cwd=BUILD):
        sys.exit(1)

    # 2. sbase
    if not skip_sbase:
        run([sys.executable, os.path.join(AIOS, "scripts", "build_sbase.py")],
            "sbase (99 tools)")

    # 3. dash
    if not skip_dash and os.path.isdir(DASH_SRC):
        dash_srcs = [
            "main.c", "eval.c", "parser.c", "expand.c", "exec.c",
            "jobs.c", "trap.c", "redir.c", "input.c", "output.c",
            "var.c", "cd.c", "error.c", "options.c", "memalloc.c",
            "mystring.c", "syntax.c", "nodes.c", "builtins.c",
            "init.c", "show.c", "arith_yacc.c", "arith_yylex.c",
            "miscbltin.c", "system.c", "alias.c", "histedit.c",
            "mail.c", "signames.c",
            "bltin/test.c", "bltin/printf.c", "bltin/times.c",
        ]
        cmd = [os.path.join(AIOS, "scripts", "aios-cc")]
        cmd += [os.path.join(DASH_SRC, s) for s in dash_srcs]
        cmd += ["-I", DASH_SRC, "-include",
                os.path.join(DASH_SRC, "config.h"),
                "-DSHELL", "-DSMALL", "-DGLOB_BROKEN",
                "-o", os.path.join(BUILD, "sbase", "dash")]
        run(cmd, "dash (login shell)")

    # 4. tcc
    if not skip_tcc and os.path.isdir(TCC_SRC):
        cmd = [os.path.join(AIOS, "scripts", "aios-cc"),
               os.path.join(TCC_SRC, "tcc.c"),
               "-I", TCC_SRC, "-I", os.path.join(TCC_SRC, "include"),
               "-include", os.path.join(TCC_SRC, "config.h"),
               "-o", os.path.join(BUILD, "sbase", "tcc")]
        run(cmd, "tcc (compiler)")

    # 5. libaios + SDK
    run([sys.executable, os.path.join(AIOS, "scripts", "build_libaios.py")],
        "libaios + CRT", tail=5)
    run([sys.executable, os.path.join(AIOS, "scripts", "build_tcc_sdk.py")],
        "tcc SDK", tail=3)

    # 6. disk image
    cmd = [sys.executable, os.path.join(AIOS, "scripts", "mkdisk.py"),
           os.path.join(AIOS, "disk", "disk_ext2.img"),
           "--rootfs", os.path.join(AIOS, "disk", "rootfs"),
           "--install-elfs", os.path.join(BUILD, "sbase"),
           "--aios-elfs", os.path.join(BUILD, "projects", "aios"),
           "--install-sdk", os.path.join(BUILD, "tcc-sdk")]
    run(cmd, "disk image", tail=3)

    dt = time.time() - t_start
    print(f"\n=== Build complete ({dt:.0f}s) ===")


if __name__ == "__main__":
    main()
