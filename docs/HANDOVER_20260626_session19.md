# HANDOVER -- session 19 (2026-06-26): errno + sys/stat; the 0.5.x line merged to `main`

Continues the userspace-kernel work (the 2026-06-24 pivot: AIOS as a gVisor-style userspace kernel
on Linux; verified seL4-on-x86-64 is the destination; verification is the soul). This session added
two libc-retarget chunks (errno, sys/stat + path ops), then **merged the entire 0.5.x line onto
`main`** and cleaned the branches. Live state: memory [[project_pivot_linux_userspace_kernel]].
Design: docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md.

## BIG WORKFLOW CHANGE (Bryan, s19)
- **The `uk/` userspace kernel now lives on `main`** -- the `userspace-kernel` branch was merged in
  (merge commit `0451298`) and deleted. **Only `main` remains locally.** All ongoing uk/ work happens
  **on `main`** now (we are 0.5.x). Commit on `main`; **Bryan pushes** (agents commit, never push).
- Remote still has `origin/userspace-kernel`, `origin/v0.4.x`, `origin/rpi4`, `origin/usb-hid-followups`
  -- Bryan deletes those via push if/when he wants; agents don't touch the remote.

## What shipped this session (all on `main`)
- **Merge `0451298`** -- userspace-kernel (M1..M3e.3) -> main. README conflict resolved by keeping the
  comprehensive pivot reframe + folding in Bryan's candid "Field note". The 0.4.x seL4 line stays
  intact on main as record/fallback.
- **M3e.3 errno** (`ecff431`, in the merge) -- a failing syscall returns a NEGATED AIOS error code
  (-errno, value in [-4095,-1]; >=0 = success). aios_abi.h AIOS_E* (match Linux) + AIOS_IS_ERR;
  pal_errno() = -errno; the kernel propagates it; libaios `int errno` + a __ret() helper threads it
  through the POSIX wrappers; real strerror(); shadow <errno.h>. The raw aios_* funcs keep returning
  the raw value (so the freestanding test programs are unaffected). Proof: prog_errno.c.
- **M3e.4 sys/stat + path ops** (`9cc9924`) -- 9 new syscalls (0x100D..0x1015): STAT/LSTAT (by path),
  GETCWD, CHDIR, UNLINK, MKDIR, RMDIR, RENAME, real GETPID. struct aios_stat expanded to a POSIX
  layout + full S_IF*; pal.h includes aios_abi.h; pal_host_stat/fstat fill struct aios_stat;
  pal_host_unlink/mkdir/rmdir/rename/chdir/getcwd. Shadow <sys/stat.h> (struct stat byte-matching
  aios_stat + S_IS*); rmdir in unistd.h, rename/remove in stdio.h. Proof: prog_fs.c.
- **BMad** -- ran the Generate-Project-Context skill: `_bmad-output/project-context.md` (24 rules, a
  lean AIOS map for AI agents) is tracked. `.claude/` + `_bmad/` are gitignored.
- **Housekeeping** -- dropped the redundant `disk/rpi4-firmware-fresh/` tracked copy (repointed the
  license at `hw/rpi4/firmware/`); kept `scripts/deploy_netconsole_cp.py`; removed the s12 stall-hunt
  scratch.

**AIOS ABI now (23 syscalls):** WRITE/READ/OPEN/CLOSE/EXIT/MMAP/FSTAT/LSEEK/EXEC/FORK/WAIT/PIPE/DUP2/
STAT/LSTAT/GETCWD/CHDIR/UNLINK/MKDIR/RMDIR/RENAME/GETPID. libaios is a real (if minimal) libc: FILE*
stdio, malloc, full-ish string/ctype/stdlib, the process + fs surface, errno, environ.

## Dev loop
- `uk/run.sh` (colima aarch64 `gcc:13` container, --cap-add=SYS_PTRACE; rc=0 gates the suite). The
  libc-program build class is `-nostdinc -isystem $(cc -print-file-name=include) -Ilib -Ilib/include`.
- HW: `scp -r uk pi@192.168.0.8:~/ && ssh pi@192.168.0.8 'cd ~/uk && make && ./aios-uk <prog>'`
  (login pi/aios; Linux 6.12). **CAVEAT: the Pi went OFFLINE mid-session** (ssh to .8 times out --
  powered off / rebooted / off-network). It was HW-verified through M3e.3; **M3e.4 is colima-verified,
  Pi-pending** (pure path/metadata host ops, no HW-specific behaviour, so colima is a strong signal --
  re-confirm on the Pi when it is back). Container `sh` is dash (no `${PIPESTATUS[*]}`). A ptrace hang
  is SILENT -> in-container `timeout N` (rc=124) + fprintf(stderr) in pal_linux.c.

## Key technical notes (carry forward)
- **Host-agnostic kernel invariant:** uk/kernel/aios_kernel.c includes only AIOS-owned headers; all
  host knowledge in uk/pal/pal_linux.c; the PAL seam is sacred + minimal (future verified boundary).
- **Driver:** classify stops with PTRACE_GET_SYSCALL_INFO (never assume entry/exit alternation);
  save/restore guest regs around injected syscalls; aarch64 syscall nr via NT_ARM_SYSTEM_CALL;
  release a process's fds on exit (pipe EOF depends on it).
- **errno:** failure = -errno; libaios __ret sets errno + returns -1; AIOS_E* match Linux.
- **sys/stat gotcha:** glibc <sys/stat.h> #defines st_atime/st_mtime/st_ctime as macros, so they are
  #undef'd in pal_linux.c before fill_aios_stat (host timespec members st_atim/st_mtim/st_ctim used
  directly). **struct aios_stat (aios_abi.h) and shadow struct stat (sys/stat.h) MUST stay
  byte-identical.**
- **cwd** is a single host-side (tracer) cwd for now -- correct for a shell + its sequential commands;
  per-process cwd resolution comes when concurrent subshells need it.

## NEXT -> real sbase, then dash (the road to operational)
1. Finish the libc gaps real sbase needs: **getopt**, **qsort**, **opendir/readdir/closedir** (a
   getdents-backed directory-stream syscall + struct dirent -- the last big piece, for `ls`). Maybe
   signals (`signal`/`kill`) -- dash wants them eventually.
2. **VENDOR real `sbase`** (suckless, MIT; NOT in the repo -- fetch via the Pi/a container + commit
   for reproducibility) and compile its utilities UNMODIFIED with the libc-program class. Start
   trivial (true/false/echo/yes/cat/wc), then ls/rm/mkdir (need readdir/stat). sbase shares a
   `util.h`/libutil (eprintf/estrtol/ARGBEGIN) -- vendor that too.
3. Then **dash** (vendor + compile; leans harder on signals/job-control/getcwd). Replace prog_sh.
4. Then **M4** boundary (seccomp/namespaces so a guest CANNOT bypass the kernel) · **M5** sched_ext
   (custom RPi kernel w/ CONFIG_SCHED_CLASS_EXT) · **M6** `pal_sel4.c` (the replant seam).

## SEED PROMPT (next session)

>>> SEED PROMPT <<<

Continue building AIOS as a **gVisor-style userspace kernel on Linux** (the 2026-06-24 pivot off
seL4/RPi4 -- Linux is the interim substrate, verified seL4-on-x86-64 is the destination; verification
is the soul; programs see only the AIOS ABI, the host sits behind a narrow PAL). READ FIRST: memory
[[project_pivot_linux_userspace_kernel]] + docs/HANDOVER_20260626_session19.md +
docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md + uk/README.md.

WORKING BRANCH = **`main`** (the 0.5.x userspace kernel was merged to main; only main remains; commit
on main, Bryan pushes). The `uk/` tree: a host-agnostic kernel (kernel/aios_kernel.c includes ONLY
aios_abi.h + aios_version.h + pal.h) over the ONLY host-aware file (pal/pal_linux.c, a multi-process
PTRACE_SYSCALL driver) + libaios (a C runtime on the AIOS ABI) + shadow standard headers (lib/include,
used with -nostdinc). DONE: M0..M3c, the FULL PROCESS MODEL (M3d: exec/fork/wait/exit/pipe/dup2 -- a
multi-process kernel + the `prog_sh` shell running real pipelines), and the LIBC RETARGET so far
(M3e.1 -nostdinc shadow headers + POSIX surface, M3e.2 FILE* buffered stdio, M3e.3 errno, M3e.4
sys/stat + path ops: stat/lstat/getcwd/chdir/unlink/mkdir/rmdir/rename/getpid). 23-syscall AIOS ABI.
Real C compiles -nostdinc against the shadow headers and runs (prog_libc/stdio/errno/fs). The driver
classifies stops with PTRACE_GET_SYSCALL_INFO; fds released on exit; errno = negated return; struct
aios_stat must stay byte-identical to the shadow struct stat. Validated colima (+ native RPi4 through
M3e.3; the Pi went offline mid-s19, M3e.4 is colima-verified, Pi-pending -- re-confirm when it's back).

DEV LOOP: `uk/run.sh` (colima aarch64 container, --cap-add=SYS_PTRACE; rc=0 gates the suite); HW via
`scp -r uk pi@192.168.0.8:~/ && ssh pi@192.168.0.8 'cd ~/uk && make && ./aios-uk <prog>'` (login
pi/aios; Linux 6.12). libc-program build flags: `-nostdinc -isystem $(cc -print-file-name=include)
-Ilib -Ilib/include`. DEBUG: a ptrace hang is SILENT -> in-container `timeout N` + fprintf(stderr);
container sh is dash (no ${PIPESTATUS[*]}).

PRIMARY TASK -> **real sbase, then dash** (operational). Finish the libc gaps sbase needs: **getopt**,
**qsort**, **opendir/readdir/closedir** (a getdents-backed directory-stream syscall + struct dirent --
the last big piece, for ls); maybe signals. Then **VENDOR real sbase** (suckless/MIT; not in the repo
-- fetch via the Pi/container + commit) and compile its utilities UNMODIFIED with the libc-program
class (true/echo/cat/wc first, then ls/rm/mkdir; vendor util.h/libutil too). Then **dash**. Keep
kernel/aios_kernel.c host-agnostic + the PAL seam minimal (the future verified boundary). Commit per
milestone on `main`; validate colima (+ Pi when reachable); Bryan pushes.

THEN: M4 enforce the boundary (seccomp/namespaces); M5 sched_ext (custom RPi kernel lacks
CONFIG_SCHED_CLASS_EXT); M6 pal_sel4.c (the replant seam). The seL4 stall + lead-#3 keyboard are
MOOTED by leaving the platform (seL4 tree preserved on main/origin as record/fallback).
