#!/usr/bin/env python3
"""
Generate builtins.def from builtins.def.in for AIOS.

Preprocesses the dash builtins.def.in file, applying AIOS settings:
  JOBS=0    (no job control)
  SMALL=1   (no history)
  HAVE_GETRLIMIT=0  (not available in AIOS)

Usage: python3 scripts/gen-builtins-def.py [--dash PATH]

Default dash path: ~/Source/github/dash/src
"""
import argparse
import os
import sys


def generate(dash_src):
    src_path = os.path.join(dash_src, "builtins.def.in")
    dst_path = os.path.join(dash_src, "builtins.def")

    if not os.path.isfile(src_path):
        print(f"ERROR: {src_path} not found")
        sys.exit(1)

    defines = {"JOBS": 0, "SMALL": 1, "HAVE_GETRLIMIT": 0}

    lines = open(src_path).readlines()
    out = []
    skip_stack = []
    in_comment = False

    for line in lines:
        s = line.strip()

        # Skip C block comments
        if "/*" in s and "*/" not in s:
            in_comment = True
            continue
        if in_comment:
            if "*/" in s:
                in_comment = False
            continue
        if s.startswith("/*") and s.endswith("*/"):
            continue

        # Handle preprocessor directives
        if s.startswith("#"):
            if s.startswith("#ifndef "):
                name = s.split()[1]
                val = defines.get(name, 1)
                skip_stack.append(val == 0)
            elif s.startswith("#ifdef "):
                name = s.split()[1]
                val = defines.get(name, 0)
                skip_stack.append(val != 0)
            elif s.startswith("#if "):
                tok = s.split()[1]
                val = defines.get(tok, 0)
                skip_stack.append(val != 0)
            elif s.startswith("#define "):
                pass
            elif s == "#else":
                if skip_stack:
                    skip_stack[-1] = not skip_stack[-1]
            elif s == "#endif":
                if skip_stack:
                    skip_stack.pop()
            continue

        # Emit line if all conditions are met
        if all(skip_stack) and s:
            out.append(line)

    with open(dst_path, "w") as f:
        f.writelines(out)

    print(f"Generated {dst_path} ({len(out)} builtins)")


def main():
    parser = argparse.ArgumentParser(description="Generate dash builtins.def for AIOS")
    parser.add_argument("--dash", default=os.path.expanduser("~/Source/github/dash/src"),
                        help="Path to dash src directory")
    args = parser.parse_args()
    generate(args.dash)


if __name__ == "__main__":
    main()
