#!/usr/bin/env python3
"""Master runner: executes all crypto_server file-generation scripts in order.

Usage:
    python3 /tmp/00_crypto_all.py

Writes the following files to /tmp/:
    crypto_chacha20.h       ChaCha20 CSPRNG header
    crypto_chacha20.c       ChaCha20 CSPRNG implementation
    entropy_collect.h       Entropy collection header
    entropy_collect.c       Entropy collection (timer jitter + IPC timing)
    crypto_server.h         Server IPC protocol and entry point
    crypto_server.c         Server main loop
    posix_crypto_hooks.c    Reference snippets for POSIX integration
    crypto_server_cmake.txt CMakeLists.txt fragment

All scripts are idempotent -- existing files are skipped.
"""
import subprocess, sys, os

SCRIPTS = [
    "01_crypto_chacha20.py",
    "02_entropy_collect.py",
    "03_crypto_server.py",
    "04_posix_crypto_hooks.py",
    "05_cmake_fragment.py",
]

# Determine directory where this script lives
script_dir = os.path.dirname(os.path.abspath(__file__))

print("==============================")
print("  AIOS crypto_server setup")
print("==============================")
print()

for name in SCRIPTS:
    path = os.path.join(script_dir, name)
    if not os.path.exists(path):
        print(f"[ERROR] {path} not found -- skipping")
        continue
    print(f"--- Running {name} ---")
    result = subprocess.run([sys.executable, path])
    if result.returncode != 0:
        print(f"[ERROR] {name} failed with exit code {result.returncode}")
        sys.exit(1)
    print()

print("==============================")
print("  All files written to /tmp/")
print("==============================")
print()
print("Generated files:")
expected = [
    "crypto_chacha20.h",
    "crypto_chacha20.c",
    "entropy_collect.h",
    "entropy_collect.c",
    "crypto_server.h",
    "crypto_server.c",
    "posix_crypto_hooks.c",
    "crypto_server_cmake.txt",
]
for f in expected:
    full = f"/tmp/{f}"
    if os.path.exists(full):
        size = os.path.getsize(full)
        print(f"  {full}  ({size} bytes)")
    else:
        print(f"  {full}  [MISSING]")
