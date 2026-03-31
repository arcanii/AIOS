#!/usr/bin/env python3
"""
AIOS Source Patcher — Phase 1: Init System + Shell Field
Runs all patch units in order.

Usage:
    python3 tools/patch_source.py              # apply all patches
    python3 tools/patch_source.py --dry-run    # preview only
    python3 tools/patch_source.py --no-backup  # skip .bak files
    python3 tools/patch_source.py 02           # run only unit 02
"""

import os, sys
from datetime import datetime

# Ensure we can import from tools/
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from patch.utils import configure
from patch import (
    utils  # need this to set globals
)

UNITS = [
    ("01", "Create disk/etc/ files",      "patch.01_create_etc"),
    ("02", "Patch auth_server.c",         "patch.02_auth_server"),
    ("03", "Patch orchestrator globals",   "patch.03_orchestrator"),
    ("04", "Patch login/respawn/prompt",   "patch.04_orch_input"),
    ("05", "Patch init() motd/hostname",   "patch.05_orch_main"),
    ("06", "Patch ext2_inject.py",         "patch.06_ext2_inject"),
    ("07", "Patch Makefile",               "patch.07_makefile"),
]

def main():
    dry_run = '--dry-run' in sys.argv
    backup = '--no-backup' not in sys.argv
    configure(dry_run=dry_run, backup=backup)

    # Check for single-unit run
    only_unit = None
    for arg in sys.argv[1:]:
        if arg.isdigit() or (len(arg) == 2 and arg[0] == '0'):
            only_unit = arg.zfill(2)

    print("=" * 60)
    print("  AIOS Phase 1: Init System + Shell Field Patcher")
    print(f"  {datetime.now().strftime('%Y-%m-%d %H:%M')}")
    if dry_run:
        print("  *** DRY RUN — no files will be modified ***")
    if only_unit:
        print(f"  Running only unit: {only_unit}")
    print("=" * 60)
    print()

    if not os.path.exists("src/auth_server.c"):
        print("ERROR: Run from AIOS project root directory")
        return 1

    results = []
    for uid, desc, modpath in UNITS:
        if only_unit and uid != only_unit:
            continue
        print(f"  [{uid}] {desc}")
        try:
            mod = __import__(modpath, fromlist=['run'])
            ok = mod.run()
            results.append((uid, desc, ok))
        except Exception as e:
            print(f"  ERROR in unit {uid}: {e}")
            import traceback
            traceback.print_exc()
            results.append((uid, desc, False))
        print()

    print("=" * 60)
    print("  Results:")
    for uid, desc, ok in results:
        status = "OK" if ok else "WARN"
        print(f"    [{uid}] {desc}: {status}")
    print()

    all_ok = all(ok for _, _, ok in results)
    if all_ok:
        print("  All patches applied successfully!")
    else:
        print("  Some patches had warnings — review output above")

    print()
    print("  Next steps:")
    print("    1. Review changes:  git diff")
    print("    2. Build:           make")
    print("    3. Build programs:  cd programs && make && cd ..")
    print("    4. Build disk:      make ext2-disk")
    print("    5. Run:             make run")
    print("    6. Test: login as root → should get osh (built-in) prompt")
    print("    7. Test: login as user → should auto-launch shell.bin")
    print("=" * 60)
    return 0 if all_ok else 1

if __name__ == '__main__':
    sys.exit(main())
