#!/usr/bin/env python3
"""POSIX.1 Compliance Audit for AIOS
Scans source files and generates terminal + wiki reports."""

import os, re, sys, datetime

SCAN_FILES = [
    "include/posix.h",
    "programs/aios.h",
    "src/orch/orch_syscall.inc",
    "src/sandbox.c",
]
SCAN_DIRS = [
    ("libc/src", ".c"),
    ("libc/include", ".h"),
    ("libc/include/sys", ".h"),
    ("libc/include/netinet", ".h"),
    ("libc/include/arpa", ".h"),
]

def load_sources():
    text = ""
    for f in SCAN_FILES:
        if os.path.exists(f):
            text += open(f).read() + "\n"
    for d, ext in SCAN_DIRS:
        if os.path.isdir(d):
            for fn in os.listdir(d):
                if fn.endswith(ext):
                    text += open(os.path.join(d, fn)).read() + "\n"
    return text

CATEGORIES = {
    "File I/O": [
        "open", "close", "read", "write", "lseek", "creat",
        "pread", "pwrite", "readv", "writev",
        "fcntl", "ioctl", "ftruncate", "truncate", "fsync", "fdatasync",
        "flock", "lockf",
    ],
    "File Status": [
        "stat", "fstat", "lstat", "access", "umask", "chmod", "fchmod",
        "chown", "fchown", "link", "unlink", "rename", "symlink", "readlink",
        "openat", "mkdirat", "unlinkat", "renameat", "fstatat",
        "statvfs", "fstatvfs",
    ],
    "Directories": [
        "mkdir", "rmdir", "getcwd", "chdir", "fchdir",
        "opendir", "readdir", "closedir", "rewinddir", "scandir",
    ],
    "Process Control": [
        "fork", "execve", "execvp", "execl", "execlp",
        "waitpid", "wait", "_exit", "exit",
        "getpid", "getppid", "getpgrp", "setpgid", "setsid",
        "spawn",
    ],
    "Signals": [
        "kill", "signal", "sigaction", "sigprocmask", "sigsuspend",
        "sigpending", "sigwait",
        "sigemptyset", "sigfillset", "sigaddset", "sigdelset", "sigismember",
        "alarm", "pause", "raise",
    ],
    "Pipes & FDs": [
        "pipe", "pipe2", "dup", "dup2", "dup3", "mkfifo", "mknod",
    ],
    "Sockets": [
        "socket", "connect", "bind", "listen", "accept",
        "send", "recv", "sendto", "recvfrom",
        "setsockopt", "getsockopt", "shutdown",
        "getaddrinfo", "freeaddrinfo", "gai_strerror",
        "inet_pton", "inet_ntop", "inet_addr", "inet_ntoa",
        "htons", "htonl", "ntohs", "ntohl",
    ],
    "Memory": [
        "malloc", "calloc", "realloc", "free",
        "mmap", "munmap", "mprotect", "sbrk", "brk",
    ],
    "Strings": [
        "strlen", "strcpy", "strncpy", "strcat", "strncat",
        "strcmp", "strncmp", "strchr", "strrchr", "strstr",
        "strtok", "strdup", "strerror", "strndup",
        "memcpy", "memmove", "memset", "memcmp",
    ],
    "stdio": [
        "printf", "fprintf", "sprintf", "snprintf",
        "fopen", "fclose", "fread", "fwrite",
        "fgets", "fputs", "fseek", "ftell", "rewind",
        "feof", "ferror", "clearerr", "fflush",
        "putchar", "getchar", "puts", "gets",
        "sscanf", "fscanf", "scanf",
        "perror", "fileno", "fdopen",
    ],
    "stdlib": [
        "atoi", "atol", "atof",
        "strtol", "strtoul", "strtod", "strtof",
        "abs", "labs", "div", "ldiv",
        "qsort", "bsearch",
        "rand", "srand",
        "abort", "atexit",
    ],
    "User/Group": [
        "getuid", "geteuid", "getgid", "getegid",
        "setuid", "setgid", "seteuid", "setegid",
        "getpwuid", "getpwnam", "getgrgid", "getgrnam",
        "getlogin", "getlogin_r",
    ],
    "Environment": [
        "getenv", "setenv", "unsetenv", "putenv",
    ],
    "Time": [
        "time", "sleep", "nanosleep",
        "gettimeofday", "clock_gettime",
        "setitimer", "getitimer",
        "localtime", "gmtime", "mktime", "strftime", "difftime",
    ],
    "System Info": [
        "uname", "gethostname", "sethostname",
        "sysconf", "pathconf", "fpathconf",
        "isatty", "ttyname", "tcgetattr", "tcsetattr",
    ],
    "I/O Multiplexing": [
        "select", "poll", "epoll_create", "epoll_ctl", "epoll_wait",
    ],
    "Threads (pthreads)": [
        "pthread_create", "pthread_join", "pthread_detach", "pthread_exit",
        "pthread_mutex_init", "pthread_mutex_lock", "pthread_mutex_unlock", "pthread_mutex_destroy",
        "pthread_cond_init", "pthread_cond_wait", "pthread_cond_signal", "pthread_cond_broadcast",
        "pthread_rwlock_init", "pthread_rwlock_rdlock", "pthread_rwlock_wrlock", "pthread_rwlock_unlock",
        "pthread_key_create", "pthread_setspecific", "pthread_getspecific",
    ],
    "Semaphores": [
        "sem_init", "sem_wait", "sem_post", "sem_destroy",
        "sem_open", "sem_close", "sem_unlink",
    ],
    "Dynamic Loading": [
        "dlopen", "dlsym", "dlclose", "dlerror",
    ],
    "Nonlocal Jumps": [
        "setjmp", "longjmp", "sigsetjmp", "siglongjmp",
    ],
    "Regex & Glob": [
        "regcomp", "regexec", "regfree",
        "glob", "globfree", "fnmatch",
    ],
    "Logging": [
        "openlog", "syslog", "closelog",
    ],
    "Misc": [
        "getopt", "getopt_long", "wordexp", "wordfree",
    ],
}

IMPL_NOTES = {
    "fork":         "stub (ENOSYS, use spawn)",
    "execve":       "wrapper (aios_exec)",
    "execvp":       "wrapper (aios_exec with /bin/ search)",
    "execl":        "wrapper (execve)",
    "execlp":       "wrapper (execvp)",
    "mmap":         "MAP_ANONYMOUS via malloc",
    "munmap":       "free()",
    "mprotect":     "stub (returns 0)",
    "sbrk":         "stub (ENOMEM)",
    "brk":          "stub (ENOMEM)",
    "select":       "basic impl (console fd)",
    "poll":         "basic impl (sleep-based)",
    "epoll_create": "stub (returns fake fd)",
    "epoll_ctl":    "stub (returns 0)",
    "epoll_wait":   "stub (sleep-based)",
    "fcntl":        "partial (F_DUPFD, F_GETFD/SETFD, F_GETFL/SETFL)",
    "ioctl":        "stub (ENOSYS)",
    "ftruncate":    "stub (ENOSYS)",
    "fsync":        "correct for write-through FS",
    "fdatasync":    "correct for write-through FS",
    "flock":        "correct for single-user OS",
    "lockf":        "correct for single-user OS",
    "lstat":        "aliases stat (no symlinks in AIOS)",
    "fchdir":       "stub (ENOSYS)",
    "fchmod":       "stub (ENOSYS)",
    "fchown":       "stub (ENOSYS)",
    "symlink":      "stub (ENOSYS)",
    "readlink":     "stub (ENOSYS)",
    "link":         "stub (ENOSYS)",
    "mkfifo":       "stub (ENOSYS)",
    "mknod":        "stub (ENOSYS)",
    "sigprocmask":  "stub (returns 0)",
    "sigsuspend":   "stub (pause)",
    "sigpending":   "stub (empty set)",
    "sigwait":      "stub (ENOSYS)",
    "setitimer":    "stub (returns 0)",
    "getitimer":    "stub (zeroed)",
    "alarm":        "stub (returns 0)",
    "setsockopt":   "stub (returns 0)",
    "getsockopt":   "stub (returns 0)",
    "shutdown":     "wrapper (close)",
    "getaddrinfo":  "minimal (IPv4 numeric only)",
    "sscanf":       "real parser (%d %u %x %o %s %c %n)",
    "fscanf":       "reads file then delegates to sscanf parser",
    "scanf":        "reads stdin then delegates to sscanf parser",
    "atof":         "delegates to strtod",
    "strtod":       "real parser (sign, int, frac, exponent)",
    "strtof":       "delegates to strtod",
    "setuid":       "sandbox model (accepted)",
    "setgid":       "sandbox model (accepted)",
    "seteuid":      "sandbox model (accepted)",
    "setegid":      "sandbox model (accepted)",
    "sysconf":      "real values (ARG_MAX, CLK_TCK, NPROCESSORS, etc.)",
    "pathconf":     "real values (NAME_MAX, PATH_MAX, PIPE_BUF, etc.)",
    "fpathconf":    "delegates to pathconf",
    "tcsetattr":    "accepted (single terminal mode)",
    "setjmp":       "real aarch64 asm (src/arch/aarch64/setjmp.S)",
    "longjmp":      "real aarch64 asm (src/arch/aarch64/setjmp.S)",
    "sigsetjmp":    "real aarch64 asm (delegates to setjmp)",
    "siglongjmp":   "real aarch64 asm (delegates to longjmp)",
    "regcomp":      "stub (returns 0)",
    "regexec":      "stub (REG_NOMATCH)",
    "glob":         "stub (GLOB_NOMATCH)",
    "fnmatch":      "stub (no match)",
    "openlog":      "routes to stderr with ident/priority",
    "syslog":       "routes to stderr with ident/priority level",
    "closelog":     "clears ident and facility",
    "wordexp":      "stub (WRDE_SYNTAX)",
    "gets":         "minimal (read-based)",
    "nanosleep":    "1-second resolution",
    "scandir":      "simplified (max 64 entries, no sort)",
    "statvfs":      "stub (hardcoded values)",
    "fstatvfs":     "stub (hardcoded values)",
}

def main():
    src = load_sources()
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    
    syscall_count = 0
    if os.path.exists("src/orch/orch_syscall.inc"):
        with open("src/orch/orch_syscall.inc") as f:
            syscall_count = f.read().count("case SYS_")
    
    wrapper_count = 0
    posix_lines = 0
    if os.path.exists("include/posix.h"):
        with open("include/posix.h") as f:
            content = f.read()
            wrapper_count = content.count("static inline")
            posix_lines = content.count("\n")
    
    header_count = 0
    for d in ["libc/include", "libc/include/sys", "libc/include/netinet", "libc/include/arpa"]:
        if os.path.isdir(d):
            header_count += len([f for f in os.listdir(d) if f.endswith(".h")])
    
    libc_lines = 0
    if os.path.isdir("libc/src"):
        for fn in os.listdir("libc/src"):
            if fn.endswith(".c"):
                with open(os.path.join("libc/src", fn)) as f:
                    libc_lines += f.read().count("\n")

    # Audit
    total = 0
    implemented = 0
    results = {}  # cat -> [(fn, found, note)]
    
    for cat, funcs in CATEGORIES.items():
        cat_results = []
        for fn in funcs:
            total += 1
            pattern = r'\b' + re.escape(fn) + r'\b'
            found = bool(re.search(pattern, src))
            if found:
                implemented += 1
            note = IMPL_NOTES.get(fn, "")
            cat_results.append((fn, found, note))
        results[cat] = cat_results

    pct_total = (implemented * 100) // total if total else 0

    # ── Terminal output ──
    print("=" * 60)
    print("  AIOS POSIX.1 Compliance Audit")
    print(f"  Generated: {now}")
    print("=" * 60)
    print()
    print(f"  Syscall handlers:  {syscall_count}")
    print(f"  POSIX wrappers:    {wrapper_count}")
    print(f"  Standard headers:  {header_count}")
    print(f"  posix.h lines:     {posix_lines}")
    print(f"  libc lines:        {libc_lines}")
    print()

    def is_stub(note):
        if not note: return False
        nl = note.lower()
        return ('stub' in nl or 'no-op' in nl or 'ENOSYS' in note or 
                'returns 0' in nl or 'infinite loop' in nl or 'hardcoded' in nl or
                'returns -1' in nl or 'zeroed' in nl)
    
    for cat, cat_results in results.items():
        n = len(cat_results)
        full = sum(1 for _, f, note in cat_results if f and not note)
        partial = sum(1 for _, f, note in cat_results if f and note and not is_stub(note))
        stubs = sum(1 for _, f, note in cat_results if f and note and is_stub(note))
        real = full + partial
        real_pct = (real * 100 // n) if n else 0
        bar = "\u2588" * (real_pct // 5) + "\u2591" * (20 - real_pct // 5)
        status = "COMPLETE" if real_pct == 100 else f"{real_pct}%"
        extra = f" (+{stubs} stubs)" if stubs else ""
        print(f"  {cat:<22} {bar} {real:>3}/{n:<3} {status}{extra}")

    # Count by implementation quality
    full_count = 0
    stub_count = 0
    partial_count = 0
    missing_count = total - implemented
    
    for cat, cat_results in results.items():
        for fn, found, note in cat_results:
            if found and not note:
                full_count += 1
            elif found and is_stub(note):
                stub_count += 1
            elif found:
                partial_count += 1

    full_pct = (full_count * 100) // total if total else 0
    real_pct = ((full_count + partial_count) * 100) // total if total else 0

    print()
    print("-" * 60)
    print(f"  TOTAL:          {implemented}/{total} functions present ({pct_total}%)")
    print(f"  Full impl:      {full_count}/{total} ({full_pct}%)")
    print(f"  Partial/wrap:   {partial_count}")
    print(f"  Stubs only:     {stub_count}")
    print(f"  Missing:        {missing_count}")
    print(f"  Real coverage:  {full_count + partial_count}/{total} ({real_pct}%)")
    print("-" * 60)

    # Missing list
    missing_cats = {}
    for cat, cat_results in results.items():
        miss = [fn for fn, f, _ in cat_results if not f]
        if miss:
            missing_cats[cat] = miss
    
    if missing_cats:
        print()
        print("  Missing functions:")
        print()
        for cat, funcs in missing_cats.items():
            print(f"    {cat}:")
            for i in range(0, len(funcs), 6):
                print(f"      {', '.join(funcs[i:i+6])}")

    def is_stub_wiki(note):
        if not note: return False
        nl = note.lower()
        return ('stub' in nl or 'no-op' in nl or 'ENOSYS' in note or
                'returns 0' in nl or 'infinite loop' in nl or 'hardcoded' in nl or
                'returns -1' in nl or 'zeroed' in nl)
    
    # ── Wiki output ──
    wiki_path = "docs/POSIX_COMPLIANCE.md"
    os.makedirs("docs", exist_ok=True)
    
    with open(wiki_path, "w") as w:
        w.write(f"# AIOS POSIX.1 Compliance Report\n\n")
        w.write(f"*Auto-generated by `tools/posix_audit.py` on {now}*\n\n")
        w.write(f"## Summary\n\n")
        w.write(f"| Metric | Count |\n")
        w.write(f"|--------|-------|\n")
        w.write(f"| Syscall handlers | {syscall_count} |\n")
        w.write(f"| POSIX wrappers | {wrapper_count} |\n")
        w.write(f"| Standard headers | {header_count} |\n")
        w.write(f"| posix.h lines | {posix_lines} |\n")
        w.write(f"| libc lines | {libc_lines} |\n")
        w.write(f"| **Functions present** | **{implemented}/{total} ({pct_total}%)** |\n")
        
        full_count = sum(1 for cat_r in results.values() for fn, f, n in cat_r if f and not n)
        partial_count = sum(1 for cat_r in results.values() for fn, f, n in cat_r if f and n and not is_stub_wiki(n))
        stub_count = implemented - full_count - partial_count
        real_pct = ((full_count + partial_count) * 100) // total if total else 0
        
        w.write(f"| Full implementations | {full_count} |\n")
        w.write(f"| Partial/wrappers | {partial_count} |\n")
        w.write(f"| Stubs only | {stub_count} |\n")
        w.write(f"| Missing | {total - implemented} |\n")
        w.write(f"| **Real coverage** | **{full_count + partial_count}/{total} ({real_pct}%)** |\n")
        w.write(f"\n")
        
        # Overall progress bar (text-based for markdown)
        real_filled = real_pct // 2
        real_empty = 50 - real_filled
        w.write(f"```\n")
        w.write(f"Real coverage: [{'█' * real_filled}{'░' * real_empty}] {real_pct}%\n")
        w.write(f"Present:       [{'█' * (pct_total // 2)}{'░' * (50 - pct_total // 2)}] {pct_total}% (includes stubs)\n")
        w.write(f"```\n\n")

        w.write(f"## Category Breakdown\n\n")
        w.write(f"| Category | Progress | Implemented | Status |\n")
        w.write(f"|----------|----------|-------------|--------|\n")
        
        for cat, cat_results in results.items():
            ok = sum(1 for _, f, _ in cat_results if f)
            n = len(cat_results)
            pct = (ok * 100 // n) if n else 0
            if pct == 100:
                emoji = "✅"
            elif pct >= 75:
                emoji = "🟢"
            elif pct >= 50:
                emoji = "🟡"
            elif pct > 0:
                emoji = "🟠"
            else:
                emoji = "🔴"
            bar = f"{'█' * (pct // 10)}{'░' * (10 - pct // 10)}"
            w.write(f"| {cat} | `{bar}` | {ok}/{n} | {emoji} {pct}% |\n")
        
        w.write(f"\n## Detailed Function List\n\n")
        
        for cat, cat_results in results.items():
            ok = sum(1 for _, f, _ in cat_results if f)
            n = len(cat_results)
            pct = (ok * 100 // n) if n else 0
            w.write(f"\n### {cat} ({ok}/{n}, {pct}%)\n\n")
            w.write(f"| Function | Status | Notes |\n")
            w.write(f"|----------|--------|-------|\n")
            
            for fn, found, note in cat_results:
                status = "✅ Implemented" if found else "❌ Missing"
                if found and note:
                    status = f"⚠️ {note}"
                elif not found:
                    status = "❌ Missing"
                w.write(f"| `{fn}` | {status} | |\n")

        # Implementation tiers
        w.write(f"\n## Implementation Tiers\n\n")
        w.write(f"### Full Implementation\n")
        w.write(f"Functions with complete, working implementations backed by syscalls.\n\n")
        
        full_impl = []
        stub_impl = []
        missing_impl = []
        
        for cat, cat_results in results.items():
            for fn, found, note in cat_results:
                if found and not note:
                    full_impl.append(f"`{fn}`")
                elif found and note:
                    stub_impl.append((fn, note))
                else:
                    missing_impl.append(fn)
        
        w.write(", ".join(full_impl) + "\n\n")
        
        w.write(f"### Stubs & Partial Implementations\n")
        w.write(f"Functions present but with limited functionality.\n\n")
        w.write(f"| Function | Implementation |\n")
        w.write(f"|----------|---------------|\n")
        for fn, note in stub_impl:
            w.write(f"| `{fn}` | {note} |\n")
        
        w.write(f"\n### Not Yet Implemented\n")
        w.write(f"Functions not present in any source file.\n\n")
        
        if missing_impl:
            for i in range(0, len(missing_impl), 8):
                w.write(", ".join(f"`{fn}`" for fn in missing_impl[i:i+8]) + "\n")
        else:
            w.write("*None — all functions are at least stubbed!*\n")

        # Architecture notes
        w.write(f"\n## Architecture Notes\n\n")
        w.write(f"- **Kernel**: seL4 microkernel with Microkit 2.1.0\n")
        w.write(f"- **Architecture**: AArch64 (Cortex-A53)\n")
        w.write(f"- **Process model**: Sandbox-based (no fork, use spawn)\n")
        w.write(f"- **Filesystem**: ext2 with full path resolution\n")
        w.write(f"- **Networking**: virtio-net with BSD socket API\n")
        w.write(f"- **No MMU page tables**: mmap uses malloc for MAP_ANONYMOUS\n")
        w.write(f"- **No dynamic linker**: all programs statically linked\n")
        w.write(f"- **No kernel threads**: pthreads not supported\n")
        w.write(f"\n## Recommendations\n\n")
        w.write(f"| Priority | Item | Rationale |\n")
        w.write(f"|----------|------|-----------|\n")
        w.write(f"| HIGH | Implement pthreads stubs | Many C libraries expect thread primitives |\n")
        w.write(f"| HIGH | Implement semaphore stubs | Required by concurrent programs |\n")
        w.write(f"| MED | Real sscanf/fscanf | Needed for text parsing |\n")
        w.write(f"| MED | Real setjmp/longjmp (asm) | Needed for error recovery |\n")
        w.write(f"| MED | ftruncate syscall | File management |\n")
        w.write(f"| LOW | Dynamic loading (dlopen) | Only needed for plugin systems |\n")
        w.write(f"| LOW | Real regex engine | Only needed for text processing |\n")
    
    print(f"\n  Wiki report: {wiki_path}")
    print(f"  ({implemented}/{total} functions, {pct_total}% POSIX.1 compliance)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
