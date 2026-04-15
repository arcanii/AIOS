#!/usr/bin/env python3
"""
AIOS Linux build environment setup.

Installs system packages, creates Python venv, clones seL4 deps,
applies patches, clones sbase/dash, generates dash headers, and
performs a full build.

Usage: python3 scripts/setup-linux.py [--skip-apt] [--skip-deps] [--skip-build]

Must be run from the AIOS repository root.
"""
import argparse
import os
import subprocess
import sys

AIOS_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd, cwd=None, check=True):
    """Run a command, print it, and return the result."""
    if isinstance(cmd, str):
        print(f"  $ {cmd}")
        return subprocess.run(cmd, shell=True, cwd=cwd, check=check)
    else:
        print(f"  $ {' '.join(cmd)}")
        return subprocess.run(cmd, cwd=cwd, check=check)


def step(msg):
    print(f"\n{'='*60}")
    print(f"  {msg}")
    print(f"{'='*60}")


def check_apt():
    """Check if required system packages are installed."""
    required = {
        "aarch64-linux-gnu-gcc": "gcc-aarch64-linux-gnu",
        "aarch64-linux-gnu-g++": "g++-aarch64-linux-gnu",
        "cmake": "cmake",
        "ninja": "ninja-build",
        "qemu-system-aarch64": "qemu-system-arm",
        "xmllint": "libxml2-utils",
        "dtc": "device-tree-compiler",
    }
    missing = []
    for binary, pkg in required.items():
        r = subprocess.run(["which", binary], capture_output=True)
        if r.returncode != 0:
            missing.append(pkg)
    return missing


def setup_apt():
    step("Step 1: System packages")
    missing = check_apt()
    if not missing:
        print("  All required packages installed.")
        return

    pkgs = " ".join(missing)
    # Also ensure python3-venv is available
    pkgs += " python3-venv"
    print(f"  Installing: {pkgs}")
    run(f"sudo apt install -y {pkgs}")


def setup_venv():
    step("Step 2: Python virtual environment")
    venv_path = os.path.join(AIOS_ROOT, ".venv")
    if os.path.isdir(venv_path):
        print(f"  venv exists at {venv_path}")
    else:
        run([sys.executable, "-m", "venv", venv_path])

    pip = os.path.join(venv_path, "bin", "pip")
    packages = "pyfdt pyyaml jinja2 jsonschema lxml ply pyelftools libarchive-c"
    run(f"{pip} install {packages}")


def setup_deps():
    step("Step 3: seL4 dependencies")
    deps_dir = os.path.join(AIOS_ROOT, "deps")
    os.makedirs(deps_dir, exist_ok=True)

    repos = {
        "seL4-kernel": "https://github.com/seL4/seL4.git",
        "musllibc": "https://github.com/seL4/musllibc.git",
        "seL4_libs": "https://github.com/seL4/seL4_libs.git",
        "seL4_tools": "https://github.com/seL4/seL4_tools.git",
        "sel4runtime": "https://github.com/seL4/sel4runtime.git",
        "util_libs": "https://github.com/seL4/util_libs.git",
    }

    for name, url in repos.items():
        dest = os.path.join(deps_dir, name)
        if os.path.isdir(dest):
            print(f"  {name}: exists")
        else:
            run(["git", "clone", url, dest])

    # kernel symlink
    kernel_link = os.path.join(deps_dir, "kernel")
    if not os.path.islink(kernel_link):
        os.symlink("seL4-kernel", kernel_link)
        print("  Created deps/kernel -> seL4-kernel")

    # cmake-tool symlink
    tools_dir = os.path.join(AIOS_ROOT, "tools", "seL4")
    os.makedirs(tools_dir, exist_ok=True)
    cmake_link = os.path.join(tools_dir, "cmake-tool")
    target = "../../deps/seL4_tools/cmake-tool"
    if os.path.islink(cmake_link):
        current = os.readlink(cmake_link)
        if current != target:
            os.remove(cmake_link)
            os.symlink(target, cmake_link)
            print(f"  Fixed cmake-tool symlink: {target}")
        else:
            print("  cmake-tool symlink: OK")
    else:
        if os.path.exists(cmake_link):
            os.remove(cmake_link)
        os.symlink(target, cmake_link)
        print(f"  Created cmake-tool symlink: {target}")

    # Verify
    all_cmake = os.path.join(tools_dir, "cmake-tool", "all.cmake")
    if os.path.isfile(all_cmake):
        print("  Verified: tools/seL4/cmake-tool/all.cmake exists")
    else:
        print("  ERROR: tools/seL4/cmake-tool/all.cmake NOT FOUND")
        sys.exit(1)


def apply_patches():
    step("Step 4: Apply patches")

    # musllibc visibility patch
    vis_h = os.path.join(AIOS_ROOT, "deps", "musllibc", "src", "internal", "vis.h")
    if os.path.isfile(vis_h):
        content = open(vis_h).read()
        if 'visibility("protected")' in content:
            content = content.replace('visibility("protected")', 'visibility("default")')
            open(vis_h, "w").write(content)
            print("  Patched musllibc vis.h: protected -> default")
        else:
            print("  musllibc vis.h: already patched")

    stdio_h = os.path.join(AIOS_ROOT, "deps", "musllibc", "src", "internal", "stdio_impl.h")
    if os.path.isfile(stdio_h):
        content = open(stdio_h).read()
        if 'visibility("protected")' in content:
            content = content.replace('visibility("protected")', 'visibility("default")')
            open(stdio_h, "w").write(content)
            print("  Patched musllibc stdio_impl.h: protected -> default")
        else:
            print("  musllibc stdio_impl.h: already patched")


def setup_sbase():
    step("Step 5a: sbase")
    sbase_dir = os.path.expanduser("~/Source/github/sbase")
    if os.path.isdir(sbase_dir):
        print(f"  sbase exists at {sbase_dir}")
    else:
        parent = os.path.dirname(sbase_dir)
        os.makedirs(parent, exist_ok=True)
        run(["git", "clone", "https://git.suckless.org/sbase", sbase_dir])


def setup_dash():
    step("Step 5b: dash")
    dash_dir = os.path.expanduser("~/Source/github/dash")
    dash_src = os.path.join(dash_dir, "src")

    if os.path.isdir(dash_dir):
        print(f"  dash exists at {dash_dir}")
    else:
        parent = os.path.dirname(dash_dir)
        os.makedirs(parent, exist_ok=True)
        run(["git", "clone", "https://github.com/tklauser/dash.git", dash_dir])

    # Generate headers
    print("  Generating dash headers...")

    # token.h (must be first)
    if not os.path.isfile(os.path.join(dash_src, "token.h")):
        run(["sh", "mktokens"], cwd=dash_src)

    # syntax.c/h
    if not os.path.isfile(os.path.join(dash_src, "syntax.c")):
        run(["cc", "-o", "mksyntax", "mksyntax.c"], cwd=dash_src)
        run(["./mksyntax"], cwd=dash_src)

    # nodes.c/h
    if not os.path.isfile(os.path.join(dash_src, "nodes.h")):
        run(["cc", "-o", "mknodes", "mknodes.c"], cwd=dash_src)
        run(["./mknodes", "nodetypes", "nodes.c.pat"], cwd=dash_src)

    # init.c
    # mkinit needs to rescan -- always regenerate
    run(["cc", "-o", "mkinit", "mkinit.c"], cwd=dash_src, check=False)
    run(["./mkinit"] + [f for f in os.listdir(dash_src) if f.endswith(".c") and not f.startswith("mk")],
        cwd=dash_src, check=False)

    # signames.c
    if not os.path.isfile(os.path.join(dash_src, "signames.c")):
        run(["cc", "-o", "mksignames", "mksignames.c"], cwd=dash_src)
        run(["./mksignames"], cwd=dash_src)

    # builtins.def
    gen_script = os.path.join(AIOS_ROOT, "scripts", "gen-builtins-def.py")
    if os.path.isfile(gen_script):
        run([sys.executable, gen_script, "--dash", dash_src])
        run(["sh", "mkbuiltins", "builtins.def"], cwd=dash_src)

    # config.h
    config_h = os.path.join(dash_src, "config.h")
    if not os.path.isfile(config_h):
        print("  Creating dash config.h...")
        with open(config_h, "w") as f:
            f.write("""\
/* config.h -- AIOS/musl/AArch64 configuration for dash */
#ifndef DASH_CONFIG_H
#define DASH_CONFIG_H

#define JOBS 0
#define SMALL 1
#define GLOB_BROKEN 1
#define _GNU_SOURCE 1
#define BSD 1

#define HAVE_STRTOD 1
#define HAVE_ISALPHA 1
#define HAVE_MEMPCPY 1
#define HAVE_PATHS_H 1
#define HAVE_STRSIGNAL 1
#define HAVE_KILLPG 1
#define HAVE_SYSCONF 1
#define HAVE_WAITPID 1

/* musl AArch64 uses stat, not stat64 */
#define stat64 stat
#define fstat64 fstat
#define lstat64 lstat
#define open64 open
#define lseek64 lseek
#define ftruncate64 ftruncate

#define SHELLPATH "/bin/dash"
#define bsd_signal signal

#endif
""")
    else:
        print("  dash config.h: exists")

    # jobs.c wait3 -> waitpid patch
    jobs_c = os.path.join(dash_src, "jobs.c")
    if os.path.isfile(jobs_c):
        content = open(jobs_c).read()
        if "wait3(status, flags, NULL)" in content:
            content = content.replace("wait3(status, flags, NULL)",
                                      "waitpid(-1, status, flags)")
            open(jobs_c, "w").write(content)
            print("  Patched jobs.c: wait3 -> waitpid")
        else:
            print("  jobs.c: already patched")


def setup_build_number():
    """Create build_number.h if it does not exist."""
    bh = os.path.join(AIOS_ROOT, "include", "aios", "build_number.h")
    if not os.path.isfile(bh):
        with open(bh, "w") as f:
            f.write("#define AIOS_BUILD_NUMBER 1\n")
        print("  Created build_number.h")
    else:
        print("  build_number.h: exists")


def do_build():
    step("Step 6: Full build")
    venv_bin = os.path.join(AIOS_ROOT, ".venv", "bin")
    env = os.environ.copy()
    env["PATH"] = venv_bin + ":" + env.get("PATH", "")

    build_dir = os.path.join(AIOS_ROOT, "build-04")

    # cmake
    if os.path.isdir(build_dir):
        print(f"  build-04 exists, using incremental build")
    else:
        os.makedirs(build_dir)
        run(["cmake", "-G", "Ninja",
             f"-DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake",
             "-DCROSS_COMPILER_PREFIX=aarch64-linux-gnu-",
             ".."], cwd=build_dir)

    setup_build_number()

    # ninja
    run(["ninja"], cwd=build_dir)

    # sbase
    sbase_dir = os.path.expanduser("~/Source/github/sbase")
    run([sys.executable, os.path.join(AIOS_ROOT, "scripts", "build_sbase.py"),
         "--sbase", sbase_dir], cwd=AIOS_ROOT)

    # dash
    run([sys.executable, os.path.join(AIOS_ROOT, "scripts", "build_dash.py")],
        cwd=AIOS_ROOT)

    # disk
    run([sys.executable, os.path.join(AIOS_ROOT, "scripts", "mkdisk.py"),
         "disk/disk_ext2.img",
         "--rootfs", "disk/rootfs",
         "--install-elfs", "build-04/sbase",
         "--aios-elfs", "build-04/projects/aios/"],
        cwd=AIOS_ROOT)


def main():
    parser = argparse.ArgumentParser(description="AIOS Linux build environment setup")
    parser.add_argument("--skip-apt", action="store_true", help="Skip apt package install")
    parser.add_argument("--skip-deps", action="store_true", help="Skip seL4 dep cloning")
    parser.add_argument("--skip-build", action="store_true", help="Skip full build")
    args = parser.parse_args()

    print("AIOS Linux Build Environment Setup")
    print(f"AIOS_ROOT: {AIOS_ROOT}")

    if not os.path.isfile(os.path.join(AIOS_ROOT, "CMakeLists.txt")):
        print("ERROR: must be run from AIOS repository root")
        sys.exit(1)

    if not args.skip_apt:
        setup_apt()

    setup_venv()

    if not args.skip_deps:
        setup_deps()
        apply_patches()

    setup_sbase()
    setup_dash()

    if not args.skip_build:
        do_build()

    step("Setup complete!")
    print("  Boot with: python3 scripts/qemu-boot.py")
    print("  Login:     root / root")
    print()


if __name__ == "__main__":
    main()
