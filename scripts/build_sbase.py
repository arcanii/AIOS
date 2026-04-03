#!/usr/bin/env python3
"""
AIOS sbase builder — cross-compiles sbase tools with progress display.

Usage: python3 scripts/build_sbase.py [--sbase PATH] [--jobs N]
"""
import os, sys, subprocess, time, argparse, glob
from concurrent.futures import ThreadPoolExecutor, as_completed

AIOS_ROOT = os.path.join(os.path.dirname(__file__), "..")
AIOS_ROOT = os.path.abspath(AIOS_ROOT)
AIOS_CC = os.path.join(AIOS_ROOT, "scripts", "aios-cc")
BUILD = os.path.join(AIOS_ROOT, "build-04", "sbase")

BARE = ["true", "false"]
LIBUTIL = ["echo", "yes", "basename", "dirname", "hostname", "logname", "whoami"]
FULL = [
    "cat", "head", "wc", "sort", "tee", "sleep", "link", "unlink", "tty",
    "printenv", "pwd", "env", "uname", "date", "cal", "cksum", "comm",
    "cmp", "cut", "expand", "fold", "nl", "paste", "rev", "seq", "strings",
    "tail", "tr", "ls", "grep", "chmod", "chown", "dd", "du", "find",
    "join", "kill", "ln", "mkdir", "mkfifo", "mktemp", "mv", "nice",
    "nohup", "od", "printf", "readlink", "rmdir", "sed", "split", "sync",
    "touch", "tsort", "unexpand", "uniq", "which", "xargs", "test", "expr",
    "chgrp", "chroot", "dc", "logger", "md5sum", "sha256sum", "sha512sum",
    "sponge", "pathchk",
]
SPECIAL = ["cp", "rm"]

def build_tool(name, srcs, flags):
    """Build one tool. Returns (name, success, error)."""
    args = [AIOS_CC] + srcs + (flags or []) + ["-o", os.path.join(BUILD, name)]
    r = subprocess.run(args, capture_output=True, text=True)
    return name, r.returncode == 0, r.stderr

def main():
    parser = argparse.ArgumentParser(description="Build sbase tools for AIOS")
    parser.add_argument("--sbase", default=os.path.expanduser("~/Desktop/github_repos/sbase"),
                        help="Path to sbase source")
    parser.add_argument("--clean", action="store_true",
                        help="Remove build dir before building")
    parser.add_argument("--jobs", "-j", type=int, default=os.cpu_count(),
                        help="Parallel jobs (default: all cores)")
    args = parser.parse_args()
    sbase = args.sbase
    jobs = args.jobs

    if not os.path.isdir(sbase):
        print(f"ERROR: sbase not found at {sbase}")
        sys.exit(1)

    if args.clean and os.path.isdir(BUILD):
        import shutil
        shutil.rmtree(BUILD)
        print(f"Cleaned {BUILD}")
    os.makedirs(BUILD, exist_ok=True)

    libutil = sorted(glob.glob(os.path.join(sbase, "libutil", "*.c")))
    libutf = sorted(glob.glob(os.path.join(sbase, "libutf", "*.c")))
    inc = ["-I", sbase]

    tasks = []
    for cmd in BARE:
        tasks.append((cmd, [os.path.join(sbase, f"{cmd}.c")], None))
    for cmd in LIBUTIL:
        tasks.append((cmd, [os.path.join(sbase, f"{cmd}.c")] + libutil, inc))
    for cmd in FULL:
        tasks.append((cmd, [os.path.join(sbase, f"{cmd}.c")] + libutil + libutf, inc))
    for cmd in SPECIAL:
        tasks.append((cmd, [os.path.join(sbase, f"{cmd}.c")] + libutil + libutf, inc))

    total = len(tasks)
    ok_count = 0
    fail_count = 0
    failed = []
    done = 0
    t0 = time.time()

    print(f"Building {total} sbase tools ({jobs} parallel jobs)...")
    print()

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(build_tool, n, s, f): n for n, s, f in tasks}
        for future in as_completed(futures):
            name, success, err = future.result()
            done += 1
            if success:
                ok_count += 1
            else:
                fail_count += 1
                failed.append((name, err))
            pct = int(done * 100 / total)
            bar = "\u2588" * (pct // 5) + "\u2591" * (20 - pct // 5)
            sys.stdout.write(f"\r  [{bar}] {pct:3d}%  ({done}/{total})  {name:<16}")
            sys.stdout.flush()

    elapsed = time.time() - t0
    print(f"\r  [{chr(0x2588) * 20}] 100%  ({total}/{total})  done{'':16}")
    print()
    print(f"  {ok_count} OK, {fail_count} failed, {elapsed:.1f}s")

    if failed:
        print()
        print("  Failed tools:")
        for name, err in failed:
            first_err = ""
            for line in err.split("\n"):
                if "error:" in line:
                    first_err = line.strip()
                    break
            print(f"    {name}: {first_err or 'unknown error'}")

    actual = len([f for f in os.listdir(BUILD) if os.path.isfile(os.path.join(BUILD, f))])
    print(f"\n  {actual} tools in {BUILD}")

if __name__ == "__main__":
    main()
