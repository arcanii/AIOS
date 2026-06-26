/*
 * aios_version.h -- the AIOS userspace-kernel version.
 *
 * 0.5.x is a NEW design line: the gVisor-style userspace kernel on a commodity host (the 2026-06-24
 * pivot, docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md). It deliberately departs from the
 * 0.4.x seL4/RPi4 bare-metal line (preserved on `main` as the record/fallback), so it gets its own
 * major.minor. Patch advances per milestone. 0.5.0 covered M0..M3e (Linux substrate, the trap/VFS
 * foundation, the full process model, and the start of the libc retarget). 0.5.1 adds the rest of
 * M3e (errno, sys/stat, getopt/qsort, directory streams) + the *at family + readlink + a real
 * printf and time layer, and VENDORED sbase whose true/false/echo/cat/wc/mkdir/rm/ls compile
 * UNMODIFIED against AIOS's libc and run on the kernel. 0.5.2 reaches "operational": VENDORED dash
 * (the Debian Almquist Shell) compiles UNMODIFIED and runs as a real shell -- builtins, arithmetic,
 * control flow, loops, pipelines, command substitution, redirection (+ setjmp/signals/fcntl libc).
 * 30-syscall ABI. 0.5.3 = M4 boundary enforcement: the trap model is now SOUND -- the PAL
 * neutralizes every trapped syscall so a guest-chosen syscall NEVER executes on the host, and the
 * kernel kills any guest that emits a non-AIOS (real Linux) syscall (an escape attempt); only the
 * kernel's own injections reach Linux. 0.5.4 = M5 real signal delivery: the kernel runs a guest's
 * handler (sigaction/kill/sigreturn + a frame dance in the PAL), so dash trap/kill work AND
 * INTERACTIVE dash + ^C work -- ^C interrupts the prompt and dash survives (a do_read single-read
 * fix made interactive mode function). 0.5.5 = M4.2 the OTHER half of the boundary: filesystem
 * confinement. When the PAL is launched with AIOS_ROOT set, every guest file path is resolved INSIDE
 * that root via openat2(RESOLVE_IN_ROOT) -- absolute paths, ".." traversal, and symlinks (absolute
 * or "..") are clamped to the root -- so a serviced open()/stat()/... can reach ONLY an AIOS root,
 * never arbitrary host paths. It is an UNPRIVILEGED primitive (no chroot/CAP), purely a PAL policy:
 * the kernel + ABI are UNCHANGED (zero new syscalls). Proof: guest/prog_jail.c (every escape vector
 * denied; in-root access works). 0.5.6 = M4.3 exec confinement: a guest-issued exec (AIOS_SYS_EXEC)
 * is resolved INSIDE the root too (openat2 + canonical /proc/self/fd path), so a guest can only
 * launch binaries in its root; the INIT program the operator names is the trusted entry, exempt.
 * Proof: guest/prog_execjail.c (in-root binaries run; out-of-root host paths denied). 0.5.7 = four
 * more vendored sbase utilities run UNMODIFIED -- head / tail / cp / mv (+ libaios grew getline/
 * getdelim, creat, basename/dirname, llabs/sleep, and honest no-op/ENOSYS stubs for the file-metadata
 * ops cp/mv reach for; a real openat2-strict mode mask + a long-standing fopen-errno fix came with it).
 * 0.5.8 = a REAL clock: AIOS_SYS_CLOCK_GETTIME (ABI -> 35) reads the host clock via the PAL, so
 * time()/clock_gettime()/gettimeofday() are live (ls dates, dash timing); time() no longer returns 0.
 * 0.5.9 = a file-METADATA layer: 5 confinement-aware *at syscalls (FCHMODAT/FCHOWNAT/SYMLINKAT/
 * LINKAT/UTIMENSAT, ABI -> 40) turn the cp/mv stubs into real ops -- chmod/chown/symlink/link/utimes
 * work, so `cp -p` preserves mode+times and the `ln`/`chmod` utilities run. Confined single-target
 * ops resolve via openat2+/proc/self/fd so a symlink cannot redirect a metadata change to a host file.
 * 0.5.10 = PER-PROCESS cwd: cwd moved from a single PAL-global to the kernel's process table -- the
 * kernel pre-absolutes every guest path (incl. the exec path) against the calling process's cwd, so a
 * subshell's `cd` no longer leaks into siblings/parent (inherited across fork, preserved across exec).
 * No new ABI; the PAL is now cwd-free (chdir verify-only, exec takes a kernel-resolved absolute path).
 * 0.5.11 = PER-PROCESS umask (AIOS_SYS_UMASK, ABI -> 41): a real file-creation mask the kernel tracks
 * per process and applies on open(O_CREAT)/mkdir, inherited across fork AND preserved across exec; the
 * host umask is neutralized so this single mask governs created modes (was a no-op tracker before).
 * 0.5.12 = vendored sbase `sort` runs UNMODIFIED (no new ABI): libaios grew a real strtod (the numeric
 * compare -n needs it; aarch64 HW FP, no soft-float runtime) + the full libutf rune chain wired in.
 * dash config.h now sets HAVE_STRTOD so dash uses the real strtod instead of its no-op fallback.
 * 0.5.13 = vendored sbase `grep` runs UNMODIFIED (no new ABI -- the last major coreutil): libaios
 * grew a real POSIX regex engine (regcomp/regexec/regfree/regerror), a small BRE/ERE matcher that
 * parses to an AST, compiles to a Thompson NFA program, and matches by LINEAR NFA simulation -- no
 * catastrophic backtracking, guaranteed to halt. Supports literals, dot, bracket classes (+POSIX
 * [:class:]), anchors, word boundaries, grouping, alternation, the star/plus/quest and {m,n}
 * quantifiers, and REG_ICASE in both BRE and ERE (boolean match -- grep compiles REG_NOSUB; submatch
 * capture is not yet needed). Plus the
 * libc grep needs: fmemopen (a read-mode mem stream), sprintf, strcasestr, and a shadow <strings.h>.
 * Proof: guest/prog_regex.c (a 75-case regcomp/regexec battery) + sbase grep -EFHcilnvwx in run.sh.
 * 0.5.14 = a real passwd/group DB (no new ABI): getpwuid/getpwnam/getgrgid/getgrnam now parse
 * /etc/passwd and /etc/group (they returned NULL before), so ls -l shows real user/group NAMES
 * instead of numeric ids; a missing/unreadable file still yields NULL -> the numeric fallback (a
 * confined guest whose root lacks /etc/passwd is unaffected). Proof: guest/prog_pwgrp.c.
 * 0.5.15 = the JOB-CONTROL FOUNDATION (ABI -> 45): kernel-tracked PROCESS GROUPS + a controlling-
 * terminal foreground group. 4 syscalls SETPGID/GETPGID/TCSETPGRP/TCGETPGRP (+ getpgrp=getpgid(0),
 * killpg=kill(-pgrp)); proc_t.pgid is inherited across fork and preserved across exec (init is its
 * own leader). KILL now signals a process group for pid <= 0 (the group -pid, or the caller's group
 * for 0). This is the foundation only -- terminal-signal ROUTING to the foreground group and a
 * STOPPED state (^Z stop/continue) + dash JOBS=1 come next; dash stays JOBS=0, so M5 interactive ^C
 * is UNTOUCHED (zero regression). Proof: guest/prog_jobctl.c (pgid inheritance, setpgid leader,
 * kill-to-group delivery, tcsetpgrp/tcgetpgrp wiring + the ENOTTY guard).
 * 0.5.16 = job control increment 2: STOP/CONTINUE (no new ABI -- reuses KILL/WAIT). A process can be
 * STOPPED (SIGSTOP/SIGTSTP default action -> a new PS_STOPPED state, the kernel plants the syscall
 * result then does NOT resume) and CONTINUED (SIGCONT resumes a stopped process immediately), and the
 * parent learns of both via wait WUNTRACED (a stopped child, status (sig<<8)|0x7f) and WCONTINUED
 * (a continued child, status 0xffff); WNOHANG polls. KILL to a stopped process special-cases SIGCONT.
 * Still no terminal-signal ROUTING (that + dash JOBS=1 is increment 3), so M5 ^C stays untouched.
 * Proof: guest/prog_stop.c (SIGSTOP -> WIFSTOPPED, SIGCONT -> WIFCONTINUED, then terminate + reap).
 * 0.5.17 = job control increment 3 (part 1): a real SIGPROCMASK (ABI -> 46). proc_t.sig_mask (a
 * bitmask of BLOCKED signals, inherited across fork) -- a blocked pending signal stays pending (kreturn/
 * the async path leave it) until sigprocmask unblocks it, delivered then; SIGKILL/SIGSTOP are never
 * blockable. dash JOBS=1 needs this for its sigblockall/sigclearmask critical sections. The pending
 * slot is single (one masked signal at a time -- a documented simplification). Proof: guest/prog_sigmask.c
 * (block SIGUSR1 -> raise -> handler does NOT run; unblock -> it is delivered).
 * 0.5.18 = job control increment 3 (part 2): TERMINAL-SIGNAL ROUTING (no new ABI). The guests are
 * moved OFF the kernel's host process group (setpgid in the spawn child), so the host pty delivers
 * ^C/^Z only to the KERNEL; the kernel catches SIGINT/SIGTSTP (a SIGTSTP handler stops the kernel
 * itself being suspended), and pal_guest_next surfaces a caught terminal signal as a new event (3).
 * The kernel then forwards it to ONLY the FOREGROUND process group (g_fg_pgrp) -- via the guests' own
 * pending-signal path (NOT a host kill of a tracee, which would hit a setret/run-to-exit hazard on a
 * guest stopped at a not-yet-serviced syscall): a RUNNING guest takes it at its next syscall, a parked
 * guest's blocked syscall returns EINTR with the signal delivered; the special syscalls read/write/wait
 * gained an entry-time pending-signal check (they bypass kreturn). So ^C now kills the foreground job
 * and the shell survives -- proven INTERACTIVELY on a pty by test/ctrlc_job_pty.c (a foreground
 * ./prog_loop, ^C, dash returns to its prompt) in addition to the existing ctrlc_pty. dash is still
 * JOBS=0 (so the fg group is everything); rebuilding dash JOBS=1 for ^Z/fg/bg is the last part.
 *
 * Host-agnostic by construction (pure version macros), so the kernel may include it without taking
 * on any host dependency.
 */
#ifndef AIOS_VERSION_H
#define AIOS_VERSION_H

#define AIOS_VERSION_MAJOR 0
#define AIOS_VERSION_MINOR 5
#define AIOS_VERSION_PATCH 18

#define _AIOS_STR(x)  #x
#define _AIOS_XSTR(x) _AIOS_STR(x)
#define AIOS_VERSION_STR \
    _AIOS_XSTR(AIOS_VERSION_MAJOR) "." _AIOS_XSTR(AIOS_VERSION_MINOR) "." _AIOS_XSTR(AIOS_VERSION_PATCH)

#define AIOS_VERSION_LINE "userspace kernel"   /* the 0.5.x design line */

#endif /* AIOS_VERSION_H */
