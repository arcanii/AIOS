#!/usr/bin/env python3
"""
build_zsh.py -- Cross-compile ZSH with ZLE for AIOS
v0.4.99: Phase 2 (interactive mode)
"""
import subprocess, os, sys, time

AIOS = os.path.expanduser("~/Desktop/github_repos/AIOS")
ZSH  = os.path.expanduser("~/Desktop/github_repos/zsh/Src")
BUILD = os.path.join(AIOS, "build-04")
CC = "aarch64-linux-gnu-gcc"
MUSL_INC = os.path.join(BUILD, "projects/musllibc/build-temp/stage/include")

# GCC builtin include
gcc_inc = subprocess.check_output(
    [CC, "-print-file-name=include"], text=True).strip()

# Core zsh sources (33 files)
CORE = [
    "builtin", "compat", "cond", "context", "exec", "glob",
    "hashnameddir", "hashtable", "hist", "init", "input", "jobs",
    "lex", "linklist", "loop", "main", "math", "mem",
    "modentry", "module", "openssh_bsd_setres_id", "options",
    "params", "parse", "pattern", "prompt", "signals", "signames",
    "sort", "string", "subst", "text", "utils",
]

# Static modules (6 files)
# Modules/ directory
MODS = ["parameter", "datetime", "langinfo", "random", "random_real"]
# Builtins/ directory
BUILTINS = ["rlimits", "sched"]

# ZLE sources (15 files)
ZLE = [
    "zle_bindings", "zle_hist", "zle_keymap", "zle_main", "zle_misc",
    "zle_move", "zle_params", "zle_refresh", "zle_thingy", "zle_tricky",
    "zle_utils", "zle_vi", "zle_word", "termquery", "textobjects",
]

# Build source list with full paths
sources = []
for f in CORE:
    sources.append(os.path.join(ZSH, f + ".c"))
for f in MODS:
    sources.append(os.path.join(ZSH, "Modules", f + ".c"))
for f in BUILTINS:
    sources.append(os.path.join(ZSH, "Builtins", f + ".c"))
for f in ZLE:
    sources.append(os.path.join(ZSH, "Zle", f + ".c"))

print("Compiling %d source files (33 core + 4 modules + 2 builtins + 15 ZLE)..." % len(sources))

# Compile flags (matching aios-cc but with zsh-specific extras)
CFLAGS = [
    "-O2", "-static", "-nostdinc",
    "-isystem", gcc_inc,
    "-isystem", MUSL_INC,
    "-I", os.path.join(AIOS, "include"),
    "-I", os.path.join(AIOS, "src/lib"),
    "-I", ZSH,
    "-I", os.path.join(ZSH, "Zle"),
    "-include", os.path.join(ZSH, "..", "config.h"),
    "-fvisibility=default",
    "-march=armv8-a",
    "-D__KERNEL_64__",
    "-std=gnu11",
    "-fno-pic", "-fno-pie",
    "-fno-stack-protector",
    "-fno-asynchronous-unwind-tables",
    "-ftls-model=local-exec",
    "-mstrict-align",
    "-mno-outline-atomics",
    "-DHAVE_CONFIG_H",
    "-D_GNU_SOURCE",
    '-DMODULE_DIR="/usr/lib/zsh"',
    "-DLINKED_XMOD_zshQszle",
    "-Wno-implicit-function-declaration",
    "-Wno-int-conversion",
    "-Wno-incompatible-pointer-types",
]

# Compile each .c to .o
tmpdir = "/tmp/aios_zsh_build"
os.makedirs(tmpdir, exist_ok=True)

objects = []
errors = 0
t0 = time.time()

for i, src in enumerate(sources):
    basename = os.path.basename(src).replace(".c", ".o")
    # Prefix with directory to avoid collisions (e.g. random.c in both Modules/ and Zle/)
    parent = os.path.basename(os.path.dirname(src))
    objname = "%s_%s" % (parent, basename)
    obj = os.path.join(tmpdir, objname)
    objects.append(obj)

    cmd = [CC] + CFLAGS + ["-c", src, "-o", obj]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("FAIL: %s" % os.path.basename(src))
        # Show first 5 error lines
        for line in result.stderr.strip().split("\n")[:5]:
            print("  %s" % line)
        errors += 1
    else:
        sys.stdout.write("\r  [%d/%d] %s" % (i+1, len(sources), os.path.basename(src)))
        sys.stdout.write(" " * 30)
        sys.stdout.flush()

print("\r  Compiled %d/%d files (%.1fs)" % (len(sources)-errors, len(sources), time.time()-t0))

if errors > 0:
    print("FAIL: %d files failed to compile" % errors)
    sys.exit(1)

# Link using aios-cc (pass .o files + libtermcap.a)
print("Linking...")
output = os.path.join(BUILD, "sbase", "zsh")
link_cmd = [os.path.join(AIOS, "scripts", "aios-cc")]
link_cmd += objects
link_cmd += [os.path.join(BUILD, "libtermcap.a")]
link_cmd += ["-o", output]

result = subprocess.run(link_cmd, capture_output=True, text=True)
if result.returncode != 0:
    print("FAIL: link failed")
    for line in result.stderr.strip().split("\n")[:15]:
        print("  %s" % line)
    sys.exit(1)

size = os.path.getsize(output)
print("OK: %s (%d bytes, %.1f MB)" % (output, size, size/1048576))
