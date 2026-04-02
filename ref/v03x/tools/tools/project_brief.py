#!/usr/bin/env python3
"""
AIOS Project Briefing Generator

Generates a comprehensive project briefing document that gives an AI assistant
full context to continue development. Scans source files, extracts architecture,
syscalls, IPC layouts, memory maps, and current state.

Usage: python3 tools/project_brief.py [--output FILE] [--compact]
"""

import os, sys, re, glob, datetime

COMPACT = '--compact' in sys.argv
OUTPUT = None
for i, arg in enumerate(sys.argv):
    if arg == '--output' and i + 1 < len(sys.argv):
        OUTPUT = sys.argv[i + 1]


def read_file(path):
    try:
        with open(path, 'r', errors='replace') as f:
            return f.read()
    except:
        return ""


def count_lines(path):
    try:
        with open(path, 'r', errors='replace') as f:
            return sum(1 for _ in f)
    except:
        return 0


def find_files(pattern, dirs=None):
    results = []
    if dirs is None:
        dirs = ['.']
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in files:
                if glob.fnmatch.fnmatch(f, pattern):
                    results.append(os.path.join(root, f))
    return sorted(results)


def extract_defines(src, prefix):
    results = []
    for m in re.finditer(r'#define\s+(' + prefix + r'\w*)\s+(\S+)(?:\s*/\*(.+?)\*/)?', src):
        results.append((m.group(1), m.group(2), (m.group(3) or '').strip()))
    return results


def extract_syscalls(src):
    results = []
    for m in re.finditer(r'case\s+(SYS_\w+)\s*:', src):
        results.append(m.group(1))
    return results


def extract_structs(src, name=None):
    results = []
    pattern = r'typedef\s+struct\s*\{([^}]+)\}\s*(\w+)\s*;'
    for m in re.finditer(pattern, src, re.DOTALL):
        sname = m.group(2)
        if name and sname != name:
            continue
        fields = []
        for line in m.group(1).strip().split('\n'):
            line = line.strip().rstrip(';')
            if line and not line.startswith('//') and not line.startswith('/*'):
                fields.append(line)
        results.append((sname, fields))
    return results


def get_version():
    src = read_file("include/aios/version.h")
    major = re.search(r'AIOS_VERSION_MAJOR\s+(\d+)', src)
    minor = re.search(r'AIOS_VERSION_MINOR\s+(\d+)', src)
    patch = re.search(r'AIOS_VERSION_PATCH\s+(\d+)', src)
    build = "?"
    if os.path.exists(".build_number"):
        build = read_file(".build_number").strip()
    if major and minor and patch:
        return f"{major.group(1)}.{minor.group(1)}.{patch.group(1)} (build {build})"
    return "unknown"


def get_file_tree(max_depth=3):
    tree = []
    skip_dirs = {'.git', 'build', '__pycache__', 'node_modules'}
    skip_ext = {'.o', '.elf', '.img', '.pyc'}

    for root, dirs, files in os.walk('.'):
        dirs[:] = sorted([d for d in dirs if d not in skip_dirs])
        depth = root.count(os.sep)
        if depth >= max_depth:
            dirs.clear()
            continue
        indent = '  ' * depth
        dirname = os.path.basename(root) or '.'
        tree.append(f"{indent}{dirname}/")
        for f in sorted(files):
            ext = os.path.splitext(f)[1]
            if ext in skip_ext or f.endswith('.bak'):
                continue
            fpath = os.path.join(root, f)
            size = os.path.getsize(fpath)
            lines = count_lines(fpath) if ext in ('.c', '.h', '.py', '.inc', '.ld', '.system', '.md', '.sh') else 0
            indent2 = '  ' * (depth + 1)
            if lines > 0:
                tree.append(f"{indent2}{f} ({lines} lines)")
            elif size > 0:
                if size > 1024 * 1024:
                    tree.append(f"{indent2}{f} ({size // (1024*1024)} MB)")
                elif size > 1024:
                    tree.append(f"{indent2}{f} ({size // 1024} KB)")
                else:
                    tree.append(f"{indent2}{f} ({size} B)")
    return '\n'.join(tree)


def get_system_description():
    src = read_file("aios.system")
    if not src:
        return "aios.system not found"
    lines = []

    pds = re.findall(r'<protection_domain\s+name="(\w+)"[^>]*priority="(\d+)"', src)
    if pds:
        lines.append("Protection Domains:")
        for name, pri in pds:
            lines.append(f"  {name} (priority={pri})")

    mrs = re.findall(r'<memory_region\s+name="(\w+)"\s+size="(0x[\da-fA-F]+)"', src)
    if mrs:
        lines.append("\nMemory Regions:")
        for name, size in mrs:
            sz = int(size, 16)
            if sz >= 1024 * 1024:
                lines.append(f"  {name}: {sz // (1024*1024)} MB")
            elif sz >= 1024:
                lines.append(f"  {name}: {sz // 1024} KB")
            else:
                lines.append(f"  {name}: {sz} B")

    channels = re.findall(r'<channel>\s*<end\s+pd="(\w+)"\s+id="(\d+)"[^/]*/>\s*<end\s+pd="(\w+)"\s+id="(\d+)"', src)
    if channels:
        lines.append("\nChannels:")
        for pd1, id1, pd2, id2 in channels:
            lines.append(f"  {pd1}[{id1}] <-> {pd2}[{id2}]")

    return '\n'.join(lines)


def get_ipc_layout():
    lines = []
    files = [
        ("src/auth_server.c", "AUTH_"),
        ("include/aios/ipc.h", "SBX_"),
        ("include/aios/ipc.h", "FS_"),
        ("include/aios/ipc.h", "LLM_"),
    ]
    for path, prefix in files:
        src = read_file(path)
        if not src:
            continue
        defines = extract_defines(src, prefix)
        if defines:
            lines.append(f"\n{path} ({prefix}*):")
            for name, val, comment in defines:
                c = f"  /* {comment} */" if comment else ""
                lines.append(f"  {name:<30} = {val}{c}")
    return '\n'.join(lines)


def get_syscall_table():
    lines = []

    # Syscall numbers from ipc.h
    src = read_file("include/aios/ipc.h")
    defs = extract_defines(src, "SYS_")
    if defs:
        lines.append("Syscall numbers:")
        for name, val, comment in defs:
            c = f"  /* {comment} */" if comment else ""
            lines.append(f"  {name:<25} = {val}{c}")

    # Handlers
    src2 = read_file("src/orch/orch_syscall.inc")
    handlers = extract_syscalls(src2)
    if handlers:
        lines.append(f"\nOrchestrator handlers ({len(handlers)} total):")
        for h in handlers:
            lines.append(f"  {h}")

    return '\n'.join(lines)


def get_memory_map():
    lines = ["Sandbox Memory Layout:"]
    lines.append("  IO page:  0x20000000 (4 KB)   — IPC with orchestrator")
    lines.append("  Heap:     0x20100000 (16 MB)  — malloc, proc_state at offset 0")
    lines.append("  Code:     0x21100000 (4 MB)   — loaded program binary (RWX)")
    lines.append("")
    lines.append("Orchestrator Memory Mappings:")
    lines.append("  Sandbox views:  0x30000000+  — orchestrator maps all sandbox memory")
    lines.append("  Swap region:    0x50000000    (256 MB) — process swap storage")

    # Extract HEAP_SIZE etc from sandbox.c
    src = read_file("src/sandbox.c")
    m = re.search(r'#define\s+HEAP_SIZE\s+\(([^)]+)\)', src)
    if m:
        lines.append(f"  HEAP_SIZE = ({m.group(1)})")
    m = re.search(r'#define\s+FORK_STACK_SAVE_MAX\s+(\S+)', src)
    if m:
        lines.append(f"  FORK_STACK_SAVE_MAX = {m.group(1)}")
    m = re.search(r'#define\s+FORK_STACK_OFFSET\s+\(([^)]+)\)', src)
    if m:
        lines.append(f"  FORK_STACK_OFFSET = ({m.group(1)})")

    return '\n'.join(lines)


def get_proc_state():
    src = read_file("include/aios/proc_state.h")
    if not src:
        return "proc_state.h not found"
    structs = extract_structs(src, "proc_state_t")
    if not structs:
        return "proc_state_t not found"
    name, fields = structs[0]
    lines = [f"proc_state_t ({len(fields)} fields):"]
    for f in fields:
        lines.append(f"  {f}")

    # Key constants
    defs = extract_defines(src, "PROC_")
    if defs:
        lines.append("\nConstants:")
        for name, val, comment in defs:
            c = f"  /* {comment} */" if comment else ""
            lines.append(f"  {name} = {val}{c}")

    return '\n'.join(lines)


def get_scheduler_info():
    src = read_file("src/proc_sched.h")
    if not src:
        return "proc_sched.h not found"
    lines = []
    for sname, fields in extract_structs(src):
        lines.append(f"\n{sname}:")
        for f in fields:
            lines.append(f"  {f}")

    states = re.findall(r'(PROC_(?:FREE|QUEUED|READY|RUNNING|BLOCKED|ZOMBIE))', src)
    if states:
        lines.append(f"\nProcess states: {', '.join(dict.fromkeys(states))}")

    defs = extract_defines(src, "SCHED_")
    if defs:
        lines.append("\nScheduler constants:")
        for name, val, comment in defs:
            lines.append(f"  {name} = {val}")

    return '\n'.join(lines)


def get_programs():
    lines = []
    bins = sorted(glob.glob("programs/*.BIN"))
    srcs = sorted(glob.glob("programs/*.c"))

    if srcs:
        lines.append(f"Programs ({len(srcs)} sources, {len(bins)} binaries):")
        for s in srcs:
            name = os.path.basename(s)
            lc = count_lines(s)
            binname = name.replace('.c', '.BIN').upper()
            binpath = os.path.join("programs", binname)
            binsz = os.path.getsize(binpath) if os.path.exists(binpath) else 0
            lines.append(f"  {name:<25} {lc:>5} lines  ->  {binname} ({binsz} bytes)")

    return '\n'.join(lines)


def get_etc_files():
    lines = []
    if os.path.isdir("disk/etc"):
        lines.append("Configuration files (disk/etc/):")
        for f in sorted(os.listdir("disk/etc")):
            if f.endswith('.bak'):
                continue
            path = os.path.join("disk/etc", f)
            content = read_file(path).strip()
            lines.append(f"\n  /etc/{f}:")
            for line in content.split('\n'):
                lines.append(f"    {line}")
    return '\n'.join(lines)


def get_build_info():
    lines = []
    mk = read_file("Makefile")

    board = re.search(r'BOARD\s*:=\s*(\S+)', mk)
    config = re.search(r'CONFIG\s*:=\s*(\S+)', mk)
    sdk = re.search(r'MICROKIT_SDK\s*\?\=\s*(.+)', mk)

    if board: lines.append(f"Board:   {board.group(1)}")
    if config: lines.append(f"Config:  {config.group(1)}")
    if sdk: lines.append(f"SDK:     {sdk.group(1).strip()}")

    pds = re.search(r'PDS\s*:=\s*(.+)', mk)
    if pds:
        pdlist = pds.group(1).strip().split()
        lines.append(f"PDs:     {len(pdlist)} ({', '.join(pdlist)})")

    targets = re.findall(r'^([\w-]+):', mk, re.MULTILINE)
    if targets:
        lines.append(f"Targets: {', '.join(sorted(set(targets)))}")

    return '\n'.join(lines)


def get_qemu_command():
    mk = read_file("Makefile")
    # Extract the qemu command from the run target
    m = re.search(r'run:.*?\n((?:\t.*\n)+)', mk)
    if m:
        cmd_lines = m.group(1).strip().split('\n')
        cmd = ' '.join(line.strip().rstrip('\\') for line in cmd_lines)
        return cmd
    return ("qemu-system-aarch64 -machine virt,virtualization=on -cpu cortex-a53 "
            "-smp 4 -m 2G -nographic -serial mon:stdio -kernel build/loader.img "
            "-drive file=disk_ext2.img,format=raw,if=none,id=hd0 "
            "-device virtio-blk-device,drive=hd0 "
            "-device virtio-net-device,netdev=net0 "
            "-netdev user,id=net0,hostfwd=tcp::8888-:80")


def get_key_source_snippets():
    snippets = []

    # aios_syscalls_t — use the dedicated header if it exists, else aios.h
    for path in ["include/aios/syscalls_t.h", "include/aios/aios.h", "programs/aios.h"]:
        src = read_file(path)
        m = re.search(r'(typedef\s+struct\s*\{.+?\}\s*aios_syscalls_t\s*;)', src, re.DOTALL)
        if m:
            snippets.append((f"aios_syscalls_t ({path})", m.group(1)))
            break

    # Channel definitions
    src = read_file("include/aios/channels.h")
    if src:
        snippets.append(("Channel definitions (include/aios/channels.h)", src.strip()))

    # Context save/restore
    src = read_file("src/arch/aarch64/context.h")
    if src:
        snippets.append(("AArch64 context save/restore", src.strip()))

    # proc_state.h
    src = read_file("include/aios/proc_state.h")
    if src:
        snippets.append(("Process state (include/aios/proc_state.h)", src.strip()))

    return snippets


def get_recent_changes():
    try:
        import subprocess
        result = subprocess.run(
            ['git', 'log', '--oneline', '-20'],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except:
        pass
    return "(git not available)"


def get_posix_summary():
    try:
        import subprocess
        result = subprocess.run(
            ['python3', 'tools/posix_audit.py'],
            capture_output=True, text=True, timeout=30
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except:
        pass
    return "(posix_audit.py not available)"


def get_line_counts():
    categories = {
        'Kernel PDs (src/*.c)': ('src', '*.c'),
        'Orchestrator includes (src/orch/)': ('src/orch', '*.inc'),
        'Headers (include/)': ('include', '*.h'),
        'Architecture (src/arch/)': ('src/arch', '*.h'),
        'Programs (programs/*.c)': ('programs', '*.c'),
        'Program headers (programs/*.h)': ('programs', '*.h'),
        'libc (libc/)': ('libc', '*.c'),
        'libc headers (libc/include/)': ('libc/include', '*.h'),
        'Tools (tools/*.py)': ('tools', '*.py'),
        'Filesystem (src/fs/)': ('src/fs', '*.c'),
    }
    lines = []
    total = 0
    for desc, (d, pat) in categories.items():
        count = 0
        nfiles = 0
        for root, _, files in os.walk(d):
            for f in files:
                if glob.fnmatch.fnmatch(f, pat):
                    count += count_lines(os.path.join(root, f))
                    nfiles += 1
        if count > 0:
            lines.append(f"  {desc:<45} {nfiles:>3} files  {count:>6} lines")
            total += count
    lines.append(f"  {'TOTAL':<45} {'':>3}        {total:>6} lines")
    return '\n'.join(lines)


def get_current_work_context():
    """Extract what was recently changed for continuity."""
    lines = []

    # Check for uncommitted changes
    try:
        import subprocess
        result = subprocess.run(
            ['git', 'diff', '--stat'],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0 and result.stdout.strip():
            lines.append("Uncommitted changes:")
            lines.append(result.stdout.strip())
            lines.append("")

        result = subprocess.run(
            ['git', 'diff', '--cached', '--stat'],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0 and result.stdout.strip():
            lines.append("Staged changes:")
            lines.append(result.stdout.strip())
            lines.append("")
    except:
        pass

    # Check for .bak files (recent patches)
    bak_files = []
    for root, _, files in os.walk('src'):
        for f in files:
            if f.endswith('.bak'):
                bak_files.append(os.path.join(root, f))
    for root, _, files in os.walk('tools'):
        for f in files:
            if f.endswith('.bak'):
                bak_files.append(os.path.join(root, f))
    if bak_files:
        lines.append("Recently patched files (have .bak backups):")
        for f in sorted(bak_files):
            lines.append(f"  {f}")
        lines.append("")

    return '\n'.join(lines) if lines else "No uncommitted changes detected."


# ══════════════════════════════════════════════════════════
#  Generate briefing
# ══════════════════════════════════════════════════════════
def generate():
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    version = get_version()

    sections = []

    def section(title, content):
        if content and content.strip():
            sections.append(f"\n{'=' * 70}\n  {title}\n{'=' * 70}\n\n{content}")

    sections.append(f"""AIOS (Open Aries) — Project Briefing
Generated: {now}
Version: {version}

This document provides full context for an AI assistant to continue
development on the AIOS project. It is auto-generated by tools/project_brief.py.
""")

    section("PROJECT OVERVIEW", """
AIOS (Open Aries) is a custom operating system built on the seL4 microkernel
using Microkit 2.1.0, targeting AArch64 (QEMU virt, cortex-a53, 4 cores, 2GB RAM).

The system uses a protection domain (PD) architecture where an orchestrator PD
manages up to 8 sandbox child PDs. Programs are loaded from an ext2 disk image
into sandbox slots, and syscalls are implemented via seL4's protected procedure
call (PPC) mechanism.

Key design principles:
- Minimal trusted computing base (TCB) via seL4 isolation
- POSIX-compatible userspace API (programs use standard C functions)
- Process management via sandbox slots with swap-based suspend/resume
- Multi-user support with /etc/passwd authentication (7-field UNIX format)
- Network stack with BSD sockets API
- Orchestrator provides built-in shell (/bin/osh) or launches external shell
""")

    section("BUILD SYSTEM", get_build_info())

    section("QEMU RUN COMMAND", get_qemu_command())

    section("BUILD & TEST WORKFLOW", """
# Build protection domains:
make

# Build sandbox programs:
cd programs && make && cd ..

# Create ext2 disk + inject files:
python3 tools/mkext2.py disk_ext2.img 128
python3 tools/ext2_inject.py disk_ext2.img programs/*.BIN
python3 tools/ext2_inject.py disk_ext2.img disk/etc/passwd disk/etc/motd disk/etc/hostname disk/etc/services.conf disk/hello.txt

# Or use Makefile targets:
make ext2-disk

# Run:
make run

# Login: root / root (SHA-256 hashed in /etc/passwd)
# Default shell for root: /bin/osh (built-in orchestrator shell)
""")

    section("CODEBASE SIZE", get_line_counts())

    section("SYSTEM DESCRIPTION (aios.system)", get_system_description())

    section("MEMORY MAP", get_memory_map())

    section("IPC LAYOUT", get_ipc_layout())

    section("SYSCALL TABLE", get_syscall_table())

    section("PROCESS STATE (proc_state_t)", get_proc_state())

    section("SCHEDULER", get_scheduler_info())

    section("PROGRAMS", get_programs())

    section("CONFIGURATION FILES", get_etc_files())

    if not COMPACT:
        section("POSIX COMPLIANCE", get_posix_summary())

    if not COMPACT:
        snippets = get_key_source_snippets()
        for title, code in snippets:
            section(f"SOURCE: {title}", code)

    section("RECENT GIT HISTORY", get_recent_changes())

    section("CURRENT WORK CONTEXT", get_current_work_context())

    if not COMPACT:
        section("FILE TREE", get_file_tree())

    section("ARCHITECTURE NOTES", """
Boot sequence:
  1. seL4 loader starts, initializes all 4 CPUs
  2. Kernel bootstraps, drops to userspace
  3. CapDL initializer starts all protection domain threads
  4. Each PD runs init(): SERIAL, BLK, FS, NET, NET_SRV, AUTH, LLM, ECHO
  5. Orchestrator prints version banner
  6. Orchestrator reads /etc/hostname -> hostname global
  7. Orchestrator loads /etc/passwd into AUTH PD (7-field: user:hash:uid:gid:gecos:home:shell)
  8. Orchestrator reads and displays /etc/motd
  9. Login prompt displayed
 10. After authentication, AUTH PD returns uid/gid/home/shell
 11. If shell == "/bin/osh" or empty: use built-in orchestrator CLI
     If shell == "/bin/shell.bin" or other: auto-exec that program in sandbox slot
 12. session_shell_slot tracks which slot has the login shell
 13. When login shell exits: auth_logout_sync(), respawn login prompt

Fork model (child-inherits-slot):
  1. Parent calls fork() -> SYS_FORK PPC to orchestrator
  2. Orchestrator saves parent code+heap to swap region (256MB at 0x50000000)
  3. Writes pending_result (child PID) at swap_base + swap_off + code_copy + 276
  4. Child inherits the physical sandbox slot, fork returns 0
  5. Child runs to completion, calls exit_proc()
  6. Orchestrator restores parent from swap, sends SBX_CMD_RESUME
  7. run_program() detects PROC_STATE_MAGIC, restores globals + stack snapshot
  8. arch_restore_context jumps back, parent's fork() returns child PID

Exec model:
  1. Program calls exec(path, args) -> SYS_EXEC PPC
  2. Sandbox writes path to IO page at 0x200, args to SBX_ARGS
  3. Orchestrator opens file from FS, validates (rejects shebang), loads binary
  4. Handles ELF files (parse in-place) or flat binaries
  5. New program starts from _start with fresh heap

Sandbox memory layout:
  IO page:  0x20000000 (4 KB)   — IPC with orchestrator
  Heap:     0x20100000 (16 MB)  — malloc (bump allocator), proc_state at offset 0
  Code:     0x21100000 (4 MB)   — loaded program binary (RWX)

Critical implementation details:
  - arch_save_context: __attribute__((noinline)), uses "add x2, sp, #0x10"
    to compensate for its own 16-byte stack frame
  - arch_restore_context: __attribute__((noinline)), switches SP then ret
  - Resume MUST be delegated to run_program() (not inline in notified())
    because notified() runs on Microkit handler stack which overlaps
  - proc_state_t magic = 0x50524F43, stored at heap[0]
  - PROC_STATE_RESERVE = 8KB at start of heap for state + stack snapshot
  - Environment variables: in-process _posix_environ array (does NOT survive exec)
  - Static variables in programs live in code .data section (overwritten on swap restore)
  - Programs linked with -ffunction-sections + link.ld placing *(.text._start) first
  - Login/session: orchestrator globals current_uid/current_gid/current_shell/current_home
    (global, not yet per-slot — needs per-slot for true multi-user)
""")

    section("KNOWN ISSUES / TODO", """
- FD state preservation across fork (test skipped — fd_pos restores from pre-fork snapshot)
- waitpid integration with fork resume (child exit code not captured by parent)
- Environment variables don't survive exec() (stored in program .data section)
  → Fix: store env in IO page region that persists across exec
- Per-slot uid needed for true multi-user (currently global current_uid)
  → SYS_GETUID should read from sched_proc_t.owner_uid instead of global
- SYS_SETUID syscall not yet implemented
- getpwnam() is hardcoded in posix.h (needs syscall to AUTH PD)
- Timer-driven preemption for uncooperative processes
- pthreads/semaphores not implemented (0% in POSIX audit)
- setjmp/longjmp are stubs (need AArch64 assembly)
- init.bin daemon for pre-login service startup not yet implemented
- Disk image must be rebuilt with tools/mkext2.py (no mkfs.ext2 on macOS)
- ext2_inject.py needs to be run manually to populate disk
""")

    return '\n'.join(sections)


def main():
    brief = generate()

    if OUTPUT:
        os.makedirs(os.path.dirname(OUTPUT) or '.', exist_ok=True)
        with open(OUTPUT, 'w') as f:
            f.write(brief)
        print(f"Project briefing written to {OUTPUT}")
        print(f"  {len(brief)} characters, {brief.count(chr(10))} lines")
    else:
        print(brief)

    return 0


if __name__ == '__main__':
    sys.exit(main())
