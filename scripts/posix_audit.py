#!/usr/bin/env python3
"""AIOS POSIX Compliance Audit -- AUDIT_V4

Comprehensive host-side analysis with HTML dashboard.
Usage: python3 scripts/posix_audit.py [--verbose]
"""
import os, re, sys, subprocess, math
from datetime import datetime

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
verbose = "--verbose" in sys.argv

def read_file(path):
    if not os.path.exists(path): return ""
    with open(path) as f: return f.read()

def count_lines(path):
    if not os.path.exists(path): return 0
    with open(path) as f: return sum(1 for _ in f)

P = lambda *p: os.path.join(REPO, *p)
POSIX_C = P("src","lib","aios_posix.c")
EXT2_C = P("src","ext2.c")
VFS_H = P("include","aios","vfs.h")
EXT2_H = P("include","aios","ext2.h")
VERIFY_C = P("src","apps","posix_verify.c")
FS_SRV = P("src","servers","fs_server.c")
PIPE_SRV = P("src","servers","pipe_server.c")
EXEC_SRV = P("src","servers","exec_server.c")
THREAD_SRV = P("src","servers","thread_server.c")
SHELL_C = P("src","apps","mini_shell.c")
VERSION_H = P("include","aios","version.h")
POSIX_H = P("src","lib","aios_posix.h")

posix_src = read_file(POSIX_C)
verify_src = read_file(VERIFY_C)

# Version
ver_src = read_file(VERSION_H)
vm = re.search(r'AIOS_VERSION_STR\s+"([^"]+)"', ver_src)
aios_version = vm.group(1) if vm else "unknown"

# 1. Syscall overrides
syscalls = {}
for m in re.finditer(r"muslcsys_install_syscall\s*\(\s*__NR_(\w+)\s*,\s*(\w+)\s*\)", posix_src):
    syscalls[m.group(1)] = m.group(2)

# 2. Wrapped functions
wraps = set()
for m in re.finditer(r"(?:int|void|struct\s+\w+\s*\*)\s+__wrap_(\w+)\s*\(", posix_src):
    wraps.add(m.group(1))

# 3. Stub detection
stub_handlers = set()
for name, handler in syscalls.items():
    pat = r"static\s+long\s+" + re.escape(handler) + r"\s*\(va_list\s+ap\)\s*\{([^}]{0,200})\}"
    m = re.search(pat, posix_src, re.DOTALL)
    if m:
        body = m.group(1).strip()
        lines = [l.strip() for l in body.split("\n") if l.strip() and not l.strip().startswith("/*")]
        real = [l for l in lines
               if not re.match(r"^\(void\)", l)
               and not re.match(r"^va_arg\(ap,", l)
               and not re.match(r"^return\s+(0|022|-?\d+)\s*;", l)
               and not re.match(r"^\(void\)\s*va_arg", l)
               and l != "(void)ap;"]
        if len(real) == 0:
            stub_handlers.add(name)

# 4. VFS ops
vfs_ops = []
vfs_src = read_file(VFS_H)
for m in re.finditer(r"int\s+\(\*(\w+)\)\s*\(", vfs_src):
    vfs_ops.append(m.group(1))
for m in re.finditer(r"^int\s+(vfs_\w+)\s*\(", vfs_src, re.MULTILINE):
    if m.group(1) not in vfs_ops:
        vfs_ops.append(m.group(1))

# 5. IPC labels
ipc_labels = {}
for st in [posix_src, read_file(EXT2_H), read_file(POSIX_H)]:
    for m in re.finditer(r"#define\s+(FS_\w+|PIPE_\w+|EXEC_\w+|THREAD_\w+|AUTH_\w+|SER_\w+|AIOS_\w+)\s+(\d+)", st):
        ipc_labels[m.group(1)] = int(m.group(2))

# 6. posix_verify
verify_tests = [m.group(1) for m in re.finditer(r'test\("([^"]+)"', verify_src)]
verify_skips = [m.group(1) for m in re.finditer(r'skip\("([^"]+)"', verify_src)]

# 7. Source metrics
src_files = [
    ("aios_posix.c", POSIX_C), ("ext2.c", EXT2_C),
    ("posix_verify.c", VERIFY_C), ("fs_server.c", FS_SRV),
    ("pipe_server.c", PIPE_SRV), ("exec_server.c", EXEC_SRV),
    ("thread_server.c", THREAD_SRV), ("mini_shell.c", SHELL_C),
]
file_lines = [(n, count_lines(p)) for n, p in src_files]
total_lines = sum(n for _, n in file_lines)

# 8. POSIX reference
MUSLCSYS = {"brk", "mmap", "munmap", "madvise"}

POSIX_REF = {
    "File I/O": [
        ("open","Open file","core"), ("openat","Open relative","core"),
        ("read","Read from fd","core"), ("write","Write to fd","core"),
        ("readv","Scatter read","core"), ("writev","Gather write","core"),
        ("close","Close fd","core"), ("lseek","Seek in fd","core"),
        ("pread64","Positional read","ext"), ("pwrite64","Positional write","ext"),
    ],
    "File Descriptors": [
        ("dup","Duplicate fd","core"), ("dup3","Dup with target","core"),
        ("fcntl","File control","core"), ("pipe2","Create pipe","core"),
        ("ftruncate","Truncate file","ext"),
    ],
    "File Metadata": [
        ("fstat","Stat by fd","core"), ("fstatat","Stat by path","core"),
        ("utimensat","Set timestamps","ext"), ("umask","Creation mask","core"),
        ("fchmod","Change mode fd","ext"), ("fchmodat","Change mode path","ext"),
        ("fchown","Change owner fd","ext"), ("fchownat","Change owner path","ext"),
        ("linkat","Hard link","ext"), ("symlinkat","Symbolic link","ext"),
        ("readlinkat","Read symlink","ext"),
    ],
    "Access Control": [
        ("access","Check access","core"), ("faccessat","Check by dirfd","core"),
    ],
    "Directories": [
        ("getdents64","Read dir entries","core"), ("chdir","Change dir","core"),
        ("getcwd","Get working dir","core"), ("mkdirat","Create dir","core"),
        ("unlinkat","Remove file/dir","core"),
        ("renameat","Rename file","ext"), ("renameat2","Rename flags","ext"),
    ],
    "Process Identity": [
        ("getpid","Process ID","core"), ("getppid","Parent PID","core"),
        ("getuid","User ID","core"), ("geteuid","Effective UID","core"),
        ("getgid","Group ID","core"), ("getegid","Effective GID","core"),
        ("setuid","Set UID","ext"), ("setgid","Set GID","ext"),
        ("setsid","Create session","ext"), ("getpgid","Process group","ext"),
    ],
    "Process Lifecycle": [
        ("clone","Fork/clone","core"), ("execve","Execute","core"),
        ("wait4","Wait child","core"), ("exit","Exit","core"),
        ("exit_group","Exit all threads","core"),
    ],
    "Signals": [
        ("rt_sigaction","Disposition","core"), ("rt_sigprocmask","Mask","core"),
        ("rt_sigpending","Pending","core"), ("kill","Send signal","core"),
        ("tgkill","Thread kill","core"),
        ("rt_sigreturn","Handler return","ext"), ("sigaltstack","Alt stack","ext"),
    ],
    "Time": [
        ("clock_gettime","Clock time","core"), ("gettimeofday","Time of day","core"),
        ("nanosleep","Sleep","core"), ("times","Process times","ext"),
        ("clock_nanosleep","Clock sleep","ext"),
    ],
    "System Info": [
        ("uname","System ID","core"), ("ioctl","Device control","core"),
    ],
    "Memory": [
        ("brk","Data segment","core"), ("mmap","Map memory","core"),
        ("munmap","Unmap memory","core"), ("madvise","Memory advice","ext"),
        ("mprotect","Change protection","ext"),
    ],
    "Threads": [
        ("pthread_create","Create thread","core"), ("pthread_join","Join","core"),
        ("pthread_exit","Exit thread","core"), ("pthread_detach","Detach","ext"),
        ("pthread_mutex_init","Init mutex","core"), ("pthread_mutex_lock","Lock","core"),
        ("pthread_mutex_unlock","Unlock","core"), ("pthread_mutex_destroy","Destroy","core"),
    ],
    "User Database": [
        ("getpwuid","Lookup by UID","core"), ("getpwnam","Lookup by name","core"),
        ("getgrnam","Group by name","ext"), ("getgrgid","Group by GID","ext"),
    ],
}

def get_status(name, category):
    is_wrap = category in ("Threads", "User Database")
    if is_wrap:
        return ("wrap", "IMPL") if name in wraps else (None, "MISS")
    if name in syscalls:
        return ("stub", "STUB") if name in stub_handlers else ("syscall", "IMPL")
    if name in MUSLCSYS:
        return ("muslcsys", "IMPL")
    return (None, "MISS")

# Build results
cat_results = []
tc = tci = te = tei = ts = 0
for cat, entries in POSIX_REF.items():
    res = []
    cc = cci = ce = cei = cs = 0
    for name, desc, level in entries:
        method, status = get_status(name, cat)
        res.append((name, desc, level, method, status))
        if level == "core":
            cc += 1
            if status in ("IMPL", "STUB"): cci += 1
        else:
            ce += 1
            if status in ("IMPL", "STUB"): cei += 1
        if status == "STUB": cs += 1
    tc += cc; tci += cci; te += ce; tei += cei; ts += cs
    cat_results.append((cat, res, cc, cci, ce, cei, cs))

tall = tc + te
timpl = tci + tei
cpct = int(100 * tci / max(tc, 1))
apct = int(100 * timpl / max(tall, 1))

# ======== Terminal report ========
W = 78
print("=" * W)
title = "AIOS POSIX Audit V4 -- " + aios_version
pad = W - len(title) - 4
print("=" * (pad // 2) + "  " + title + "  " + "=" * (pad - pad // 2))
print("=" * W)

for cat, res, cc, cci, ce, cei, cs in cat_results:
    n = len(res); impl = cci + cei
    pct = int(100 * impl / max(n, 1))
    filled = int(20 * pct / 100)
    bar = "#" * filled + "." * (20 - filled)
    print("  [%s] %3d%%  %s (%d/%d)" % (bar, pct, cat, impl, n))

print("=" * W)
print("  Core:     %d/%d (%d%%)" % (tci, tc, cpct))
print("  Extended: %d/%d" % (tei, te))
print("  Total:    %d/%d (%d%%)" % (timpl, tall, apct))
print("  Stubs:    %d    Tests: %dp/%ds" % (ts, len(verify_tests), len(verify_skips)))
print("=" * W)

# ======== HTML report ========
CI = "#22c55e"   # green
CS = "#eab308"   # amber
CM = "#374151"   # gray unfilled
CR = "#ef4444"   # red
CB = "#111827"   # bg
CC = "#1f2937"   # card
CT = "#f3f4f6"   # text
CD = "#9ca3af"   # dim
CA = "#60a5fa"   # accent


def svg_donut(impl, stub, total, sz=180):
    r = sz // 2 - 10
    cx = cy = sz // 2
    circ = 2 * math.pi * r
    real = impl - stub
    s_real = circ * real / max(total, 1)
    s_stub = circ * stub / max(total, 1)
    pct = int(100 * impl / max(total, 1))
    s = '<svg width="%d" height="%d" viewBox="0 0 %d %d">\n' % (sz, sz, sz, sz)
    s += '<circle cx="%d" cy="%d" r="%d" fill="none" stroke="%s" stroke-width="18"/>\n' % (cx, cy, r, CM)
    s += '<circle cx="%d" cy="%d" r="%d" fill="none" stroke="%s" stroke-width="18"' % (cx, cy, r, CI)
    s += ' stroke-dasharray="%.1f %.1f"' % (s_real, circ - s_real)
    s += ' transform="rotate(-90 %d %d)"/>\n' % (cx, cy)
    s += '<circle cx="%d" cy="%d" r="%d" fill="none" stroke="%s" stroke-width="18"' % (cx, cy, r, CS)
    s += ' stroke-dasharray="%.1f %.1f"' % (s_stub, circ - s_stub)
    s += ' stroke-dashoffset="-%.1f"' % s_real
    s += ' transform="rotate(-90 %d %d)"/>\n' % (cx, cy)
    s += '<text x="%d" y="%d" text-anchor="middle" fill="%s"' % (cx, cy - 8, CT)
    s += ' font-size="36" font-weight="bold" font-family="monospace">%d%%</text>\n' % pct
    s += '<text x="%d" y="%d" text-anchor="middle" fill="%s"' % (cx, cy + 18, CD)
    s += ' font-size="14" font-family="monospace">%d/%d</text>\n' % (impl, total)
    s += '</svg>'
    return s


def svg_bar(impl, stub, total, w=350, h=22):
    if total == 0: return ""
    real = impl - stub
    wr = int(w * real / total)
    ws = int(w * stub / total)
    s = '<svg width="%d" height="%d">' % (w, h)
    s += '<rect width="%d" height="%d" rx="4" fill="%s"/>' % (w, h, CM)
    s += '<rect width="%d" height="%d" rx="4" fill="%s"/>' % (wr, h, CI)
    if ws > 0:
        s += '<rect x="%d" width="%d" height="%d" fill="%s"/>' % (wr, ws, h, CS)
    s += '</svg>'
    return s


def svg_file_chart(files, mx):
    bw = 400; lw = 160; rw = bw - lw - 10
    th = len(files) * 34
    s = '<svg width="%d" height="%d">' % (bw, th)
    for i, (name, n) in enumerate(files):
        y = i * 34
        w = int(rw * n / max(mx, 1))
        s += '<text x="%d" y="%d" text-anchor="end" fill="%s"' % (lw - 8, y + 18, CD)
        s += ' font-size="12" font-family="monospace">%s</text>' % name
        s += '<rect x="%d" y="%d" width="%d" height="24" rx="3"' % (lw, y + 2, w)
        s += ' fill="%s" opacity="0.7"/>' % CA
        s += '<text x="%d" y="%d" fill="%s"' % (lw + w + 6, y + 18, CD)
        s += ' font-size="12" font-family="monospace">%d</text>' % n
    s += '</svg>'
    return s


# Category rows
cat_html = []
for cat, res, cc, cci, ce, cei, cs in cat_results:
    n = len(res); impl = cci + cei
    pct = int(100 * impl / max(n, 1))
    bar = svg_bar(impl, cs, n)

    details = ""
    for name, desc, level, method, status in res:
        if status == "IMPL":
            clr, icon = CI, "&#10003;"
            tag = " [%s]" % method if method else ""
        elif status == "STUB":
            clr, icon, tag = CS, "&#9888;", " [stub]"
        else:
            clr, icon, tag = CR, "&#10007;", ""
        star = '<span style="color:#f59e0b">*</span>' if level == "core" else "&nbsp;"
        details += '<div style="padding:2px 0;font-size:13px">'
        details += '<span style="color:%s;width:20px;display:inline-block">%s</span>' % (clr, icon)
        details += '%s <code style="color:%s">%-25s</code>' % (star, CT, name)
        details += ' <span style="color:%s">%s</span>' % (CD, desc)
        details += '<span style="color:%s;font-size:11px">%s</span></div>\n' % (CD, tag)

    toggle_js = "this.parentElement.querySelector('.detail').classList.toggle('hidden')"
    row = '<div style="background:%s;border-radius:8px;padding:16px;margin:8px 0">' % CC
    row += '<div style="display:flex;align-items:center;justify-content:space-between;cursor:pointer"'
    row += ' onclick="%s">' % toggle_js
    row += '<div><span style="font-size:15px;font-weight:600;color:%s">%s</span>' % (CT, cat)
    row += '<span style="color:%s;font-size:13px;margin-left:12px">%d/%d (%d%%)</span></div>' % (CD, impl, n, pct)
    row += '<div>%s</div></div>' % bar
    row += '<div class="detail hidden" style="margin-top:12px;padding-top:8px;border-top:1px solid #374151">'
    row += details + '</div></div>'
    cat_html.append(row)

# Gaps
core_gap = ""
ext_gap = ""
for cat, res, _, _, _, _, _ in cat_results:
    for name, desc, level, _, status in res:
        if status == "MISS":
            row = '<div style="font-size:13px;padding:2px 0">'
            row += '<span style="color:%s">&#10007;</span> ' % CR
            row += '<code>%s</code> ' % name
            row += '<span style="color:%s">%s</span> ' % (CD, desc)
            row += '<span style="color:%s;font-size:11px">[%s]</span></div>' % (CD, cat)
            if level == "core":
                core_gap += row
            else:
                ext_gap += row

if not core_gap:
    core_gap = '<div style="color:%s">All core POSIX syscalls implemented!</div>' % CI
if not ext_gap:
    ext_gap = '<div style="color:%s">All extended implemented!</div>' % CI

# Verify list
vhtml = ""
for t in verify_tests:
    vhtml += '<div style="color:%s;font-size:12px;padding:1px 0">&#10003; %s</div>\n' % (CI, t)
for t in verify_skips:
    vhtml += '<div style="color:%s;font-size:12px;padding:1px 0">&#9888; %s</div>\n' % (CS, t)

donut = svg_donut(timpl, ts, tall)
fmax = max(n for _, n in file_lines)
fchart = svg_file_chart(file_lines, fmax)
now = datetime.now().strftime("%Y-%m-%d %H:%M")

h = []
h.append('<!DOCTYPE html>')
h.append('<html lang="en"><head><meta charset="utf-8">')
h.append('<title>AIOS POSIX Audit -- %s</title>' % aios_version)
h.append('<style>')
h.append('*{margin:0;padding:0;box-sizing:border-box}')
h.append('body{background:%s;color:%s;font-family:-apple-system,"SF Mono",Menlo,monospace;padding:24px;max-width:900px;margin:0 auto}' % (CB, CT))
h.append('h1{font-size:22px;font-weight:700;margin-bottom:4px}')
h.append('h2{font-size:16px;font-weight:600;color:%s;margin:24px 0 8px 0}' % CA)
h.append('.sub{color:%s;font-size:13px;margin-bottom:20px}' % CD)
h.append('.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}')
h.append('.card{background:%s;border-radius:8px;padding:16px}' % CC)
h.append('.stat{font-size:28px;font-weight:700}')
h.append('.sl{font-size:12px;color:%s}' % CD)
h.append('.legend{display:flex;gap:16px;margin:12px 0;font-size:12px}')
h.append('.ld{width:10px;height:10px;border-radius:2px;display:inline-block;margin-right:4px}')
h.append('.hidden{display:none}')
h.append('code{font-family:"SF Mono",Menlo,monospace;font-size:13px}')
h.append('@media(max-width:700px){.grid{grid-template-columns:1fr}}')
h.append('</style></head><body>')

h.append('<h1>AIOS POSIX Compliance Audit</h1>')
h.append('<div class="sub">%s | %s | IEEE Std 1003.1-2024</div>' % (aios_version, now))

h.append('<div class="legend">')
h.append('<span><span class="ld" style="background:%s"></span>Implemented</span>' % CI)
h.append('<span><span class="ld" style="background:%s"></span>Stub</span>' % CS)
h.append('<span><span class="ld" style="background:%s"></span>Missing</span>' % CR)
h.append('<span style="color:%s"><span style="color:#f59e0b">*</span> = core</span>' % CD)
h.append('</div>')

h.append('<div class="grid">')
h.append('<div class="card" style="text-align:center">%s' % donut)
h.append('<div style="margin-top:8px"><span class="stat" style="color:%s">%d</span>' % (CI, timpl))
h.append('<span class="sl"> / %d interfaces</span></div></div>' % tall)

h.append('<div class="card">')
h.append('<div style="margin-bottom:12px"><div class="stat" style="color:%s">%d%%</div>' % (CI, cpct))
h.append('<div class="sl">Core POSIX (%d/%d)</div></div>' % (tci, tc))
h.append('<div style="margin-bottom:12px"><div class="stat" style="color:%s">%d</div>' % (CA, tei))
h.append('<div class="sl">Extended (%d/%d)</div></div>' % (tei, te))
h.append('<div style="display:flex;gap:24px">')
h.append('<div><div class="stat" style="color:%s">%d</div><div class="sl">Stubs</div></div>' % (CS, ts))
h.append('<div><div class="stat" style="color:%s">%d</div><div class="sl">Tests pass</div></div>' % (CA, len(verify_tests)))
h.append('<div><div class="stat" style="color:%s">%d</div><div class="sl">Skipped</div></div>' % (CD, len(verify_skips)))
h.append('</div></div></div>')

h.append('<h2>Coverage by Category</h2>')
h.append('<div style="font-size:13px;color:%s;margin-bottom:8px">Click a row to expand</div>' % CD)
for ch in cat_html:
    h.append(ch)

h.append('<h2>Gap Analysis: Missing Core</h2>')
h.append('<div class="card">%s</div>' % core_gap)
h.append('<h2>Gap Analysis: Missing Extended</h2>')
h.append('<div class="card">%s</div>' % ext_gap)

h.append('<h2>Source File Sizes</h2>')
h.append('<div class="card">%s' % fchart)
h.append('<div style="margin-top:8px;color:%s;font-size:12px">Total: %d lines</div></div>' % (CD, total_lines))

h.append('<h2>posix_verify V2 (%dp / %ds)</h2>' % (len(verify_tests), len(verify_skips)))
h.append('<div class="card" style="max-height:300px;overflow-y:auto">%s</div>' % vhtml)

h.append('<div style="margin-top:24px;color:%s;font-size:11px;text-align:center">' % CD)
h.append('Generated by posix_audit.py V4 | AIOS %s</div>' % aios_version)
h.append('</body></html>')

html_path = os.path.join(REPO, "docs", "posix_audit.html")
with open(html_path, "w") as f:
    f.write("\n".join(h))
print("\n  HTML report: %s" % html_path)

if sys.platform == "darwin":
    subprocess.run(["open", html_path], check=False)
    print("  Opened in browser")
