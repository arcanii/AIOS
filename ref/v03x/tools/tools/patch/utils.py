"""Shared utilities for AIOS source patchers."""

import os, shutil

DRY_RUN = False
BACKUP = True
_module = "PATCH"

def configure(dry_run=False, backup=True):
    global DRY_RUN, BACKUP
    DRY_RUN = dry_run
    BACKUP = backup

def set_module(name):
    global _module
    _module = name

def log(msg):
    prefix = '[DRY] ' if DRY_RUN else ''
    print(f"  {prefix}[{_module}] {msg}")

def read_file(path):
    with open(path, 'r') as f:
        return f.read()

def write_file(path, content):
    if DRY_RUN:
        log(f"Would write {len(content)} bytes to {path}")
        return
    if BACKUP and os.path.exists(path):
        bak = path + '.bak'
        if not os.path.exists(bak):
            shutil.copy2(path, bak)
            log(f"Backup: {bak}")
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with open(path, 'w') as f:
        f.write(content)
    log(f"Wrote {path}")

def create_file(path, content):
    d = os.path.dirname(path)
    if d and not DRY_RUN:
        os.makedirs(d, exist_ok=True)
    if d:
        log(f"Ensure dir: {d}")
    write_file(path, content)

def replace_block(src, old, new, label="block"):
    """Replace old text with new in src. Returns (new_src, success)."""
    if old in src:
        src = src.replace(old, new, 1)
        log(f"Replaced {label}")
        return src, True
    else:
        log(f"WARNING: Could not find {label}")
        return src, False

def insert_after(src, marker, text, label="insert"):
    """Insert text after marker. Returns (new_src, success)."""
    if marker in src:
        src = src.replace(marker, marker + text, 1)
        log(f"Inserted {label}")
        return src, True
    else:
        log(f"WARNING: Could not find marker for {label}")
        return src, False

def insert_before(src, marker, text, label="insert"):
    """Insert text before marker. Returns (new_src, success)."""
    if marker in src:
        src = src.replace(marker, text + marker, 1)
        log(f"Inserted {label}")
        return src, True
    else:
        log(f"WARNING: Could not find marker for {label}")
        return src, False

def find_function(src, func_name):
    """Find function boundaries by brace counting. Returns (start, end) or None."""
    pattern = func_name
    idx_start = src.find(pattern)
    if idx_start < 0:
        return None
    brace_count = 0
    idx = idx_start
    found_first = False
    while idx < len(src):
        if src[idx] == '{':
            brace_count += 1
            found_first = True
        elif src[idx] == '}':
            brace_count -= 1
            if found_first and brace_count == 0:
                return (idx_start, idx + 1)
        idx += 1
    return None

def replace_function(src, func_sig, new_body, label="function"):
    """Replace entire function (from signature to closing brace)."""
    bounds = find_function(src, func_sig)
    if bounds:
        start, end = bounds
        src = src[:start] + new_body + src[end:]
        log(f"Replaced {label}")
        return src, True
    else:
        log(f"WARNING: Could not find {label}")
        return src, False
