#!/usr/bin/env python3
"""
AIOS dash builder -- cross-compiles dash shell for AIOS.

Usage: python3 scripts/build_dash.py [--dash PATH] [--build PATH]

Default dash path: ~/Source/github/dash/src  (Linux)
                   ~/Desktop/github_repos/dash/src  (macOS)
"""
import argparse
import os
import platform
import subprocess
import sys

AIOS_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SOURCES = [
    "main.c", "eval.c", "parser.c", "expand.c", "exec.c",
    "jobs.c", "trap.c", "redir.c", "input.c", "output.c",
    "var.c", "cd.c", "error.c", "options.c", "memalloc.c",
    "mystring.c", "syntax.c", "nodes.c", "builtins.c",
    "init.c", "show.c", "arith_yacc.c", "arith_yylex.c",
    "miscbltin.c", "system.c", "alias.c", "histedit.c",
    "mail.c", "signames.c",
    "bltin/test.c", "bltin/printf.c", "bltin/times.c",
]


def default_dash_path():
    if platform.system() == "Darwin":
        return os.path.expanduser("~/Desktop/github_repos/dash/src")
    return os.path.expanduser("~/Source/github/dash/src")


def check_generated_headers(dash_src):
    """Verify all generated headers exist."""
    required = ["syntax.c", "syntax.h", "nodes.c", "nodes.h",
                "builtins.c", "builtins.h", "init.c", "signames.c",
                "token.h", "config.h"]
    missing = []
    for f in required:
        if not os.path.isfile(os.path.join(dash_src, f)):
            missing.append(f)
    if missing:
        print(f"ERROR: missing generated files in {dash_src}:")
        for f in missing:
            print(f"  {f}")
        print("\nRun the header generators first (see docs/BUILDING_ENVIRO_LINUX.md)")
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="Build dash for AIOS")
    parser.add_argument("--dash", default=default_dash_path(),
                        help="Path to dash src directory")
    parser.add_argument("--build", default=os.path.join(AIOS_ROOT, "build-04"),
                        help="Build directory")
    args = parser.parse_args()

    dash_src = args.dash
    build = args.build
    output = os.path.join(build, "sbase", "dash")
    aioscc = os.path.join(AIOS_ROOT, "scripts", "aios-cc")

    if not os.path.isdir(dash_src):
        print(f"ERROR: dash source not found at {dash_src}")
        sys.exit(1)

    check_generated_headers(dash_src)

    os.makedirs(os.path.join(build, "sbase"), exist_ok=True)

    cmd = [aioscc]
    cmd += [os.path.join(dash_src, s) for s in SOURCES]
    cmd += ["-I", dash_src]
    cmd += ["-include", os.path.join(dash_src, "config.h")]
    cmd += ["-DSHELL", "-DSMALL", "-DGLOB_BROKEN", "-D_GNU_SOURCE"]
    cmd += ["-o", output]

    print(f"Building dash -> {output}")
    r = subprocess.run(cmd)
    if r.returncode == 0:
        sz = os.path.getsize(output)
        print(f"OK: dash ({sz} bytes)")
    else:
        print(f"FAIL: exit code {r.returncode}")
        sys.exit(1)


if __name__ == "__main__":
    main()
