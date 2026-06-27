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
 * 0.5.19 = job control increment 3 (part 3, THE LAST): dash rebuilt JOBS=1 -- FULL interactive job
 * control (no new ABI; all the kernel pieces were already in place). dash setpgid's each job into its
 * own pgrp + tcsetpgrp's the foreground, so ^C reaches ONLY the foreground job (not the shell), ^Z
 * suspends it (SIGTSTP -> PS_STOPPED -> WUNTRACED -> dash "[1]+ Stopped"), and fg/bg resume it. Needed
 * a shadow <termios.h> (jobs.c includes it; dash calls no line-discipline fns), the fg/bg builtins
 * REGENERATED into builtins.{def,c,h} from builtins.def.in with JOBS=1 (dash's own mkbuiltins), and
 * libaios strsignal extended to 31 (so SIGTSTP prints "Stopped", not "Unknown signal"). Proven on a
 * pty: test/ctrlz_pty.c (^Z suspend -> fg resume -> ^C kill) joins ctrlc_pty + ctrlc_job_pty. The
 * JOB-CONTROL ARC (M7 inc 1..3) is COMPLETE.
 * 0.5.20 = a real TERMIOS line-discipline layer (ABI -> 48): TCGETATTR/TCSETATTR proxy to the host
 * tty, so a program can switch the terminal to RAW mode (cfmakeraw clears ICANON/ECHO/ISIG) for
 * char-at-a-time, unechoed input -- when a guest sets raw mode the host pty enters it, so the kernel's
 * reads then return one keypress at a time. struct aios_termios + a full shadow <termios.h> (flag
 * values match the host so the PAL translation is a field copy); cfmakeraw + the cf-speed helpers are
 * inline in the header. Replaces the JOBS=1 stub <termios.h>. Proof: guest/prog_rawkey.c via
 * test/rawkey_pty.c (one byte, NO Enter -> "rawkey got: Z", unechoed -- canonical mode would block).
 * 0.5.21 = SIGPIPE on a broken pipe (no new ABI): a guest that writes to a pipe with NO readers left
 * now gets SIGPIPE (signal 13) routed via kreturn -- default action TERMINATES the writer (so
 * `producer | head` dies quietly instead of printing a spurious write error), while a guest that
 * IGNORES or catches SIGPIPE still gets -EPIPE. The kernel process keeps ignoring host SIGPIPE (it
 * does pipe writes on guests' behalf). Proof: guest/prog_sigpipe.c (default -> writer exits 141;
 * ignored -> write returns -1/EPIPE) + ls -l | head is now quiet.
 * 0.5.22 = a SECOND PAL backend (the portability proof, no new ABI -- the kernel is BYTE-IDENTICAL):
 * `make PAL=seccomp` builds aios-uk over a seccomp SECCOMP_RET_TRACE trap mechanism instead of
 * PTRACE_SYSCALL. A BPF filter classifies the guest's syscalls and traps ONLY them (as a
 * PTRACE_EVENT_SECCOMP stop); the guest runs via PTRACE_CONT in between -- the syscall-interception
 * HOT PATH is now BPF-filtered seccomp, not blanket ptrace. The two backends share the entire Linux
 * host-driver + ptrace INJECTOR core (pal/pal_linux_common.c): Linux has no userspace-only way to
 * inject memory/processes or rewrite another process's registers, so mmap/exec/fork/exit + the
 * signal-frame dance stay ptrace either way (a host property, not a seam leak) -- they run at the
 * seccomp-event stop, AFTER the filter, so rewriting the syscall number dispatches the real host
 * syscall without re-filtering. The one knob the injectors need is PAL_RESUME(pid) = "resume to the
 * next trap" (PTRACE_SYSCALL vs PTRACE_CONT). pal_linux.c + pal_seccomp.c are thin trap front-ends
 * over the shared core. Proof: the WHOLE 16-key run.sh gate passes a SECOND time with PAL=seccomp
 * (escape still killed under seccomp; mmap/fork/exec/pipes/signals/^C/^Z/raw-mode/confinement all
 * work), kernel/aios_kernel.c unchanged. This is the dress rehearsal for the seL4/x86-64 pal_sel4.c.
 * 0.5.23 = the SYSTEM LAYER, increment 1 (no new ABI; pure AIOS-ABI programs + libaios): AIOS boots
 * into a managed system, not a bare shell. A real AIOS init (guest/init.c -- the first guest) runs
 * the console login + respawns it on logout; login (guest/login.c) prompts for a username + a password
 * (read with terminal ECHO off via the termios layer), authenticates against /etc/shadow, and becomes
 * the user's login shell (exec argv[0] "-sh" so dash sources /etc/profile). mkaiosroot.sh installs
 * /sbin/init + /bin/login + /etc/shadow + /etc/profile + /home/aios; the appliance launches /sbin/init
 * instead of /bin/sh, so the appliance boots to an AIOS LOGIN. KEY FIX: login reads stdin UNBUFFERED
 * (buffered fgets over-reads the pipe into the FILE buffer, so the exec'd shell would lose its input).
 * INCREMENT-1 scope (honest): /etc/shadow passwords are compared PLAINTEXT and the session keeps the
 * kernel's existing identity (no uid/gid switch) -- crypt() hashing + a SETUID/SETGID ABI are
 * increment 2. Proof: test/login_pty.c (a pty drives init -> login -> a password-checked session ->
 * logout -> a respawned login; PASS under BOTH PAL backends), wired into the gate.
 * 0.5.24 = the SYSTEM LAYER, increment 2 (part 1): PROCESS IDENTITY (ABI -> 54). Six syscalls
 * GETUID/GETEUID/GETGID/GETEGID/SETUID/SETGID give each process real/effective/saved uid+gid that the
 * kernel tracks as its OWN model -- decoupled from the host user the kernel runs as, exactly like fs
 * confinement is kernel-owned policy. Identity is inherited across fork and preserved across exec; the
 * launched (init) guest is seeded as AIOS root (uid 0). setuid/setgid follow POSIX privilege: euid 0
 * sets real+effective+saved, otherwise the new id must equal the real or saved id (EPERM else) -- so
 * login can drop from uid 0 to the authenticated user and that drop is irreversible. Replaces the
 * libaios stubs (getuid/geteuid/getgid/getegid returned a fixed 0). Proof: guest/prog_id.c (seeded
 * root -> setgid/setuid drop -> EPERM on regaining root -> identity inherited across fork, no leak
 * back to the parent) + the sbase `whoami` util (geteuid -> getpwuid) runs UNMODIFIED. login switching
 * the user + crypt() password hashing + more utils are the next parts of increment 2.
 * 0.5.25 = the SYSTEM LAYER, increment 2 (part 2): login SWITCHES USER (no new ABI). On a successful
 * auth, login -- running as init's child (AIOS root, uid 0) -- setgid's then setuid's to the
 * authenticated user before becoming their shell, so the WHOLE session (motd, shell, every command)
 * runs as that user: whoami/id/$LOGNAME reflect them. The drop is privileged + irreversible (the user
 * cannot regain uid 0). AIOS identity is the kernel's model (the host still owns real file ownership),
 * so this is identity, not yet uid-based file-access control. libaios grew getlogin() ($LOGNAME/$USER,
 * else the real uid's passwd entry); sbase `logname` joins `whoami`, both UNMODIFIED, in the image.
 * Proof: test/login_pty.c now also asserts the session sees `whoami` == the logged-in user (aios), not
 * root, alongside the existing login -> session -> logout -> respawn loop.
 * 0.5.26 = the SYSTEM LAYER, increment 2 (part 3): real crypt() PASSWORD HASHING (no new ABI). /etc/shadow
 * now stores SHA-512 ("$6$") crypt HASHES, not plaintext; login recomputes crypt(typed_pw, stored_hash)
 * and compares. libaios gained a from-scratch SHA-512 (FIPS 180-4) + the SHA-512-crypt scheme (Ulrich
 * Drepper's spec), producing hashes BYTE-IDENTICAL to host glibc / `openssl passwd -6` -- a real,
 * verifiable algorithm, no host call (aarch64 has native 64-bit ops, so the -nostdlib guest needs no
 * runtime helpers). A non-'$' secret is still accepted as legacy plaintext (transitional). Proof:
 * guest/prog_crypt.c (crypt of aios/root reproduces the host openssl reference vectors exactly; a wrong
 * password does not match; the verify round-trip holds; an unsupported $1$ scheme -> NULL), and
 * login_pty still authenticates aios/aios end-to-end against the hashed shadow.
 * 0.5.27 = the SYSTEM LAYER, increment 2 (part 4): more sbase utils run UNMODIFIED -- uname / env /
 * printenv / pwd / tty / date (no new ABI). The headline is uname: it reports AIOS's OWN identity
 * ("AIOS <version> ... aarch64"), NOT the host's "Linux" -- proof that a guest sees the AIOS kernel.
 * libaios grew: uname() over a new shadow <sys/utsname.h> (AIOS constants + /etc/hostname); the
 * environment-mutation surface putenv/setenv/unsetenv (for env); mktime (the UTC inverse of gmtime,
 * so date prints + computes time matching the host exactly); a read-only clock_settime (EPERM -- the
 * AIOS clock has no set syscall, so `date -s` honestly refuses while reading works); and ttyname
 * (/dev/console on a tty -- AIOS has no /dev/pts). date is UTC-only (AIOS has no timezone). seq +
 * printf + tr + cut are deferred to the next part (they need float printf %f/%g and the libutf chain).
 * 0.5.28 = the SYSTEM LAYER, increment 2 (part 5): sbase tr + cut run UNMODIFIED (no new ABI). tr
 * (translate/squeeze/delete, incl. POSIX [:class:] sets) wires the full libutf rune chain (the is*rune
 * classifiers + to{lower,upper}rune + ef{get,put}rune/utflen) -- the same machinery sort/grep use; cut
 * (-b/-c/-f with -d) adds memmem. Both purely Makefile wiring against the existing libaios (the rune
 * layer was already there). seq + printf (the util) still need float printf (%f/%g) -- next part; and
 * /etc/inittab services + a clean shutdown remain for increment 2.
 *
 * Host-agnostic by construction (pure version macros), so the kernel may include it without taking
 * on any host dependency.
 */
#ifndef AIOS_VERSION_H
#define AIOS_VERSION_H

#define AIOS_VERSION_MAJOR 0
#define AIOS_VERSION_MINOR 5
#define AIOS_VERSION_PATCH 28

#define _AIOS_STR(x)  #x
#define _AIOS_XSTR(x) _AIOS_STR(x)
#define AIOS_VERSION_STR \
    _AIOS_XSTR(AIOS_VERSION_MAJOR) "." _AIOS_XSTR(AIOS_VERSION_MINOR) "." _AIOS_XSTR(AIOS_VERSION_PATCH)

#define AIOS_VERSION_LINE "userspace kernel"   /* the 0.5.x design line */

#endif /* AIOS_VERSION_H */
