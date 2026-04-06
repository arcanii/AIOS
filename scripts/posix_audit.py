#!/usr/bin/env python3
"""AIOS POSIX Syscall Audit -- AUDIT_V2
Scans aios_posix.c for implemented syscall overrides and reports coverage.
"""
import os, re, sys

REPO = os.path.join(os.path.dirname(__file__), "..")
POSIX_C = os.path.join(REPO, "src", "lib", "aios_posix.c")

if not os.path.exists(POSIX_C):
    print(f"ERROR: {POSIX_C} not found")
    sys.exit(1)

with open(POSIX_C) as f:
    src = f.read()

# Extract syscall overrides: muslcsys_install_syscall(__NR_xxx, ...)
installed = set()
for m in re.finditer(r"muslcsys_install_syscall\s*\(\s*__NR_(\w+)", src):
    installed.add(m.group(1))

# Extract wrapped functions: __wrap_xxx
wrapped = set()
for m in re.finditer(r"__wrap_(\w+)", src):
    wrapped.add(m.group(1))

# POSIX required syscalls for a minimal UNIX (SUSv4 relevant subset)
POSIX_REQUIRED = {
    # File I/O
    "open": "File open",
    "openat": "File open relative",
    "read": "Read bytes",
    "readv": "Scatter read",
    "write": "Write bytes",
    "writev": "Gather write",
    "close": "Close fd",
    "lseek": "Seek in fd",
    "dup": "Duplicate fd",
    "dup3": "Duplicate fd with flags",
    "fcntl": "File control",
    # Stat
    "fstat": "Stat by fd",
    "fstatat": "Stat by path",
    # Directories
    "getdents64": "Read directory entries",
    "chdir": "Change directory",
    "mkdirat": "Create directory",
    "unlinkat": "Remove file/dir",
    # Identity
    "getpid": "Process ID",
    "getppid": "Parent process ID",
    "getuid": "User ID",
    "geteuid": "Effective user ID",
    "getgid": "Group ID",
    "getegid": "Effective group ID",
    # System
    "uname": "System name",
    "getcwd": "Current working dir",
    "ioctl": "Device control",
    # Access
    "access": "Check file access",
    "faccessat": "Check file access relative",
    "umask": "Set file creation mask",
    "utimensat": "Set file timestamps",
    # Time
    "clock_gettime": "Get clock",
    "gettimeofday": "Get time of day",
    "nanosleep": "High-res sleep",
    # Process
    "exit": "Exit process",
    "exit_group": "Exit all threads",
    # Memory
    "mmap": "Map memory",
    "munmap": "Unmap memory",
    "brk": "Adjust data segment",
    "madvise": "Memory advice",
    # Signals
    "rt_sigaction": "Signal handlers",
    "rt_sigprocmask": "Signal mask",
    "rt_sigpending": "Pending signals",
    "kill": "Send signal",
    "tgkill": "Thread-group kill",
    # Fork/Exec/Wait (via IPC wrappers)
    "clone": "Create child process",
    # Pipe
    "pipe2": "Create pipe pair",
}

# Check for IPC-based implementations (fork, waitpid, pipe via wrappers)
ipc_impl = set()
if "fork" in wrapped or "PIPE_FORK" in src:
    ipc_impl.add("fork")
if "waitpid" in wrapped or "PIPE_WAIT" in src:
    ipc_impl.add("waitpid")
if "pipe" in wrapped or "PIPE_CREATE" in src:
    ipc_impl.add("pipe")
if "execve" in wrapped or "PIPE_EXEC" in src:
    ipc_impl.add("execve")

# Memory syscalls provided by muslcsys (not via install_syscall)
muslcsys_provided = set()
for name in ["brk", "mmap", "munmap", "madvise"]:
    # These are built into musl C library support, always available
    muslcsys_provided.add(name)

print("=== AIOS POSIX Syscall Audit (v2) ===\n")

have = []
missing = []
for name, desc in sorted(POSIX_REQUIRED.items()):
    if name in installed:
        have.append((name, desc, "syscall"))
    elif name in ipc_impl:
        have.append((name, desc, "IPC"))
    elif name in muslcsys_provided:
        have.append((name, desc, "muslcsys"))
    else:
        missing.append((name, desc))

print(f"IMPLEMENTED ({len(have)}/{len(POSIX_REQUIRED)}):")
for name, desc, method in have:
    tag = f"[{method}]" if method != "syscall" else ""
    print(f"  + {name:20s} {desc} {tag}")

print(f"\nMISSING ({len(missing)}/{len(POSIX_REQUIRED)}):")
for name, desc in missing:
    print(f"  - {name:20s} {desc}")

print(f"\nWrapped functions ({len(wrapped)}):")
for w in sorted(wrapped):
    print(f"  __wrap_{w}")

print(f"\nSummary: {len(have)}/{len(POSIX_REQUIRED)} POSIX syscalls implemented")
print(f"  + {len(ipc_impl)} IPC-based (fork/exec/wait/pipe)")
print(f"  + {len(wrapped)} wrapped functions")
