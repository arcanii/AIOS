# Audit: what syscalls does our shim handle vs what GNU/toybox needs

have = [
    "open", "openat", "read", "readv", "write", "writev",
    "close", "lseek", "fstat", "fstatat",
]

need_easy = {
    "getcwd":     "return CWD string — shell already tracks it",
    "getpid":     "return PID from proc_table",
    "getppid":    "return parent PID (always 1)",
    "getuid":     "return 0 (root)",
    "geteuid":    "return 0",
    "getgid":     "return 0",
    "getegid":    "return 0",
    "uname":      "fill utsname struct with AIOS info",
    "isatty":     "return 1 for fd 0,1,2",
    "ioctl":      "stub — return 0 for TIOCGWINSZ",
    "dup":        "copy fd entry",
    "dup2":       "copy fd to specific number",
    "access":     "try open, return result",
    "faccessat":  "same via openat",
    "clock_gettime": "read ARM counter",
    "gettimeofday":  "read ARM counter",
    "nanosleep":  "busy-wait or yield loop",
    "getenv":     "return from env table",
    "fcntl":      "stub most, handle F_GETFL/F_SETFL",
    "mmap":       "already handled by muslcsys",
    "munmap":     "already handled by muslcsys",
    "brk":        "already handled by muslcsys",
    "madvise":    "already handled by muslcsys",
}

need_medium = {
    "getdents64":  "directory reading — IPC to fs_thread, new FS_GETDENTS op",
    "stat":        "path-based stat via IPC (have fstatat, need __NR_stat)",
    "lstat":       "same as stat (no symlinks)",
    "mkdir":       "ext2 write support needed",
    "rmdir":       "ext2 write support needed",
    "unlink":      "ext2 write support needed",
    "rename":      "ext2 write support needed",
    "pipe":        "in-kernel IPC buffer between two fds",
    "dup3":        "dup2 with flags",
}

need_hard = {
    "fork":        "full process clone — need COW or shared memory",
    "execve":      "replace process image — load new ELF",
    "wait4/waitpid": "wait for child exit — need parent-child tracking",
    "sigaction":   "signal delivery via fault handler",
    "kill":        "signal sending between processes",
    "pipe2":       "pipe with flags",
    "socket":      "networking stack",
}

print("=== POSIX Syscall Audit ===\n")
print(f"HAVE ({len(have)}):")
for s in have: print(f"  ✅ {s}")
print(f"\nEASY STUBS ({len(need_easy)}) — 1-10 lines each:")
for s, d in need_easy.items(): print(f"  ⬜ {s}: {d}")
print(f"\nMEDIUM ({len(need_medium)}) — need new IPC or ext2 features:")
for s, d in need_medium.items(): print(f"  ⬜ {s}: {d}")
print(f"\nHARD ({len(need_hard)}) — architectural changes:")
for s, d in need_hard.items(): print(f"  ⬜ {s}: {d}")
print(f"\nTotal: {len(have)} done, {len(need_easy)} easy, {len(need_medium)} medium, {len(need_hard)} hard")
print(f"Easy stubs unlock: cat, echo, head, wc, true, false, yes, basename, dirname, printf, sleep, uname, env, id, tty")
print(f"Medium (getdents) unlocks: ls, find, du, cp -r")
print(f"Hard (fork/exec) unlocks: shell scripts, pipes, job control")
