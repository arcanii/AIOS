/*
 * pal.h -- the Platform Abstraction Layer: the ONLY surface the AIOS kernel uses to reach the
 * host. Narrow by design, because this seam is the future *verified boundary* -- the smaller it
 * is, the smaller the eventual proof obligation when AIOS replants onto seL4.
 *
 * The AIOS kernel (kernel/aios_kernel.c) includes ONLY AIOS-owned headers (this one, aios_abi.h,
 * aios_version.h) -- never a host header -- so the kernel is host-agnostic. Today the backend is
 * pal/pal_linux.c (ptrace); a
 * future pal_sel4.c implements the SAME contract via seL4 IPC / a VMM. Swapping the backend does
 * not touch the kernel or any AIOS program.
 *
 * The trap model is gVisor-style: a guest program's syscall is *trapped* (not cooperatively
 * forwarded) and serviced here. pal_guest_trap_next() is the per-host trap primitive.
 */
#ifndef AIOS_PAL_H
#define AIOS_PAL_H

#include <stddef.h>
#include <stdint.h>
#include "aios_abi.h"   /* struct aios_stat (the metadata format the PAL fills) -- host-agnostic */

/* One intercepted guest syscall: the AIOS-ABI request a program made (see aios_abi.h). */
typedef struct {
    uint64_t nr;        /* AIOS syscall number */
    uint64_t arg[6];    /* arguments                                                       */
} pal_syscall_t;

/* A guest-process handle. Opaque to the AIOS kernel, which keys its process table by it and never
 * assumes its representation (the Linux PAL uses the tracee pid; a future seL4 PAL a TCB cap).
 * Positive when valid; PAL_PID_NONE otherwise. */
typedef int pal_pid_t;
#define PAL_PID_NONE ((pal_pid_t)-1)

/* --- guest world lifecycle (multi-process) ---
 *
 * The kernel runs MANY guests. The PAL no longer holds a single global guest: every primitive is
 * keyed by a pal_pid_t. The loop is "wait for the next event from ANY guest (pal_guest_next), then
 * service + resume THAT guest". Resume is owned by pal_guest_return / pal_guest_resume; waiting is
 * owned by pal_guest_next -- decoupled, the standard multi-tracee shape. A future seL4 PAL keeps
 * the same contract over real process objects; the kernel above does not change. */

/* Load + start the AIOS program at `path` (argv NULL-terminated, argv[0] = the program). Returns
 * the new guest's handle, or PAL_PID_NONE. The guest is stopped at its entry: the kernel registers
 * it, then pal_guest_resume()s it to set it running toward its first syscall. */
pal_pid_t pal_guest_spawn(const char *path, char *const argv[]);

/* Wait for the next event from ANY live guest. Skips internal artifacts (syscall exits, the stray
 * stops injection leaves behind). Returns:
 *   1  a syscall trapped on *who   -> *sc filled (NOT executed by the host); *who is at its entry
 *   0  *who exited                 -> *exit_code set (8-bit, POSIX-shaped)
 *   2  *who got an async signal    -> *exit_code = signal number (the kernel owns the policy: run
 *                                     the guest's handler, ignore it, or terminate)
 *  -1  no live guests / error                                                                   */
int pal_guest_next(pal_pid_t *who, pal_syscall_t *sc, int *exit_code);

/* Job control: pal_take_term_signal returns+clears a TERMINAL signal (^C -> SIGINT, ^Z -> SIGTSTP)
 * the PAL caught for the controlling terminal (or 0). pal_guest_next surfaces it as event 3; the
 * kernel then forwards it to the foreground process group (via the guests' own pending-signal path). */
int pal_take_term_signal(void);

/* Set the value `who` sees returned from the syscall it is stopped at, and resume it toward its
 * next syscall. Valid only when `who` is stopped at a syscall ENTRY (a normal trapped syscall, or
 * a parent parked in wait that the kernel is now waking). */
int pal_guest_return(pal_pid_t who, uint64_t retval);

/* Resume `who` without setting a return value -- for the inject primitives below (mmap/exec/fork),
 * which place the guest's result themselves, and to start a freshly spawned guest. */
int pal_guest_resume(pal_pid_t who);

/* --- guest memory --- */

/* Copy `len` bytes out of / into guest `who`'s address space at `gaddr`. Returns bytes copied (0
 * on failure). The kernel bounces syscall buffers out, and writes results (read data, stat, wait
 * status) back -- including into a DIFFERENT guest than the one currently trapped (e.g. delivering
 * a wait status into a parked parent), which is why these are pid-keyed. */
size_t pal_guest_read (pal_pid_t who, uint64_t gaddr, void *dst, size_t len);
size_t pal_guest_write(pal_pid_t who, uint64_t gaddr, const void *src, size_t len);

/* Grow guest `who`'s address space by `len` bytes of fresh anonymous memory; returns the guest
 * address, or 0. The Linux PAL injects a real mmap into the (stopped) guest; a future seL4 PAL
 * maps frames via caps. Places the result in the guest itself; the kernel then pal_guest_resume()s.
 * Only valid while `who` is stopped servicing its mmap syscall. */
uint64_t pal_guest_mmap(pal_pid_t who, size_t len);

/* Replace guest `who`'s program image with the AIOS program at `abspath` -- an ABSOLUTE path the
 * kernel already resolved against the calling process's cwd (a KERNEL string, not a guest pointer;
 * the PAL stages it into `who`). argv/envp stay guest pointers (NULL-terminated arrays in `who`'s own
 * address space). Loader work, inherently a PAL concern (Linux injects execve; a future seL4 PAL
 * parses the ELF + builds the address space). When confined (M4.3) the path is clamped to the AIOS
 * root + canonicalized, so a guest can only launch in-root binaries. Returns 0 if the new image is
 * live (the kernel pal_guest_resume()s it -- its first syscall traps as usual), or -1 on failure (the
 * guest's result is set to -errno). The kernel's fd table for `who` is untouched, so AIOS fds survive
 * exec (POSIX -- how a shell wires up redirections before exec). */
int pal_guest_exec(pal_pid_t who, const char *abspath, uint64_t gargv, uint64_t genvp);

/* Fork guest `parent` (stopped at its fork syscall): create a child guest that is a copy of the
 * parent's address space, now also traced. The Linux PAL injects a real clone; a future seL4 PAL
 * copies the VSpace. Places the fork result in each guest (child pid in the parent, 0 in the
 * child); the kernel registers the child + pal_guest_resume()s BOTH. Returns the child handle, or
 * PAL_PID_NONE on failure (the parent's result is set to -1; the kernel resumes it). */
pal_pid_t pal_guest_fork(pal_pid_t parent);

/* Terminate guest `who` with exit status `code` (the Linux PAL injects exit_group; a future seL4
 * PAL destroys the process). `who`'s subsequent exit surfaces through pal_guest_next as event 0,
 * which drives the kernel's reap / wake-the-parent bookkeeping. */
int pal_guest_exit(pal_pid_t who, int code);

/* --- host-driver gateway --- */
/* A backing object the host provides for a file/stream. Opaque to the AIOS kernel, which keeps
 * these in its own fd table and never assumes their representation (the Linux PAL uses a host
 * fd; a future seL4 PAL might use a server cap). */
typedef int64_t pal_file_t;
#define PAL_FILE_INVALID ((pal_file_t)-1)

/* The host's standard streams as backing handles. which: 0=stdin 1=stdout 2=stderr. The AIOS
 * kernel seeds its reserved fds (AIOS_FD_*) from these. */
pal_file_t pal_host_std(int which);

/* Open a host-backed file. `aios_flags` are AIOS_O_* (aios_abi.h) -- the PAL translates them to
 * native host flags. Returns a backing handle (>= 0), or a negated AIOS error code (-errno < 0) on
 * failure. This is the AIOS kernel's route to real storage (Linux: open(2); seL4: an fs-server IPC).
 *
 * M4.2 confinement (a PAL-internal policy, NOT part of this contract -- the kernel passes the same
 * path strings either way): when the Linux backend is launched with AIOS_ROOT set, this and EVERY
 * path-taking primitive below resolve INSIDE that root (openat2 RESOLVE_IN_ROOT), so a serviced path
 * can never escape to an arbitrary host path. A future seL4 PAL gets the same view from an fs cap
 * rooted at the AIOS fs. Default (unset) = the whole host, behaviour unchanged. */
pal_file_t pal_host_open(const char *path, uint64_t aios_flags, uint64_t mode);

/* Read/write/close a backing object. write is also the kernel's diagnostic + stdout path. Return
 * bytes transferred / 0 on EOF (read) or close-OK, or a NEGATED AIOS error code (-errno, see
 * aios_abi.h) on failure. The codes are host-agnostic AIOS values: a future seL4 PAL maps its own
 * errors onto the same set. PAL_EWOULDBLOCK / PAL_EPIPE are named conveniences for the two the pipe
 * path tests by name (= -AIOS_EAGAIN / -AIOS_EPIPE). */
#define PAL_EWOULDBLOCK (-11)   /* a non-blocking pipe end would block (empty for read / full write) */
#define PAL_EPIPE       (-32)   /* write to a pipe whose read ends are all closed                   */
long pal_host_read (pal_file_t f, void *buf, size_t len);
long pal_host_write(pal_file_t f, const void *buf, size_t len);
int  pal_host_close(pal_file_t f);

/* Create a pipe: a unidirectional byte stream with a read end (*rd) and a write end (*wr), both
 * NON-BLOCKING so the single-threaded kernel never wedges on one guest's I/O -- it parks that guest
 * and services others. Returns 0, or -1. (Linux: pipe2(O_NONBLOCK); a future seL4 PAL builds an
 * in-kernel ring or notification pair.) */
int pal_host_pipe(pal_file_t *rd, pal_file_t *wr);

/* Reposition (whence is AIOS_SEEK_*; returns the new absolute offset, or -errno) and stat by fd
 * (fills *out; 0 or -errno). The PAL maps these onto the host (Linux: lseek/fstat). */
long long pal_host_lseek(pal_file_t f, long long off, int whence);
int       pal_host_fstat(pal_file_t f, struct aios_stat *out);

/* Read directory entries from a directory backing object (opened via pal_host_open). Fills `buf`
 * with a packed stream of `struct aios_dirent` records (aios_abi.h); returns bytes written (>0), 0
 * at end of directory, or a negated AIOS error code. (Linux: getdents64, translated record-by-
 * record; a future seL4 PAL lists its fs server into the same record format.) */
long pal_host_getdents(pal_file_t f, void *buf, size_t len);

/* --- filesystem namespace ops (by path) --- All return 0 (or a length, getcwd) on success, or a
 * negated AIOS error code. The Linux PAL maps to stat/lstat/unlink/mkdir/rmdir/rename/chdir/getcwd;
 * a future seL4 PAL routes them to its fs server. `follow` selects stat (1) vs lstat (0). cwd is a
 * single host-side (tracer) cwd for now -- correct for a shell + its sequential commands; per-process
 * cwd comes when concurrent subshells need it. */
int  pal_host_stat  (const char *path, struct aios_stat *out, int follow);
int  pal_host_unlink(const char *path);
int  pal_host_mkdir (const char *path, unsigned int mode);
int  pal_host_rmdir (const char *path);
int  pal_host_rename(const char *oldpath, const char *newpath);
int  pal_host_chdir (const char *path);
long pal_host_getcwd(char *buf, size_t size);

/* --- the *at family (resolve a path relative to a directory backing object) ---
 * `dir` is a directory backing object, or PAL_AT_FDCWD to resolve relative to the host cwd (the
 * kernel passes this sentinel when the guest used AIOS_AT_FDCWD). The Linux PAL maps it to the
 * host's AT_FDCWD and uses openat/fstatat/unlinkat/faccessat; a future seL4 PAL routes them to its
 * fs server. follow selects stat (1) vs lstat (0); removedir selects rmdir; amode is the AIOS_?_OK
 * set. All return 0 / a fd on success, or a negated AIOS error code. */
#define PAL_AT_FDCWD ((pal_file_t)-100)
pal_file_t pal_host_openat   (pal_file_t dir, const char *path, uint64_t aios_flags, uint64_t mode);
int        pal_host_fstatat  (pal_file_t dir, const char *path, struct aios_stat *out, int follow);
int        pal_host_unlinkat (pal_file_t dir, const char *path, int removedir);
int        pal_host_faccessat(pal_file_t dir, const char *path, int amode);

/* Read a symlink's target into `buf` (no NUL terminator, like POSIX readlink). Returns bytes, or a
 * negated AIOS error code. (Linux: readlink(2); a future seL4 PAL asks its fs server.) */
long pal_host_readlink(const char *path, char *buf, size_t bufsize);

/* Is a backing object a terminal? 1 / 0 / -errno. (Linux: isatty/tcgetattr.) */
int pal_host_isatty(pal_file_t f);

/* Terminal line-discipline attributes. get fills `out`, set applies `in` (actions = TCSANOW/DRAIN/
 * FLUSH). 0, or a negated AIOS error. The PAL translates struct aios_termios <-> the host's; when a
 * guest sets raw mode the host tty enters it, so the kernel's reads then return char-at-a-time.
 * (Linux: tcgetattr/tcsetattr; a future seL4 PAL talks to its terminal server.) */
int pal_host_tcgetattr(pal_file_t f, struct aios_termios *out);
int pal_host_tcsetattr(pal_file_t f, int actions, const struct aios_termios *in);

/* Read a clock into *out (clk_id is AIOS_CLOCK_REALTIME / AIOS_CLOCK_MONOTONIC). 0, or a negated
 * AIOS error code. The Linux PAL maps to clock_gettime(2); a future seL4 PAL reads its own timer
 * source. This is the kernel's only wall-clock/monotonic time source. */
int pal_host_clock_gettime(int clk_id, struct aios_timespec *out);

/* --- file-metadata *at family (mode / owner / symlink / hardlink / times) ---
 * `dir` is a directory backing object or PAL_AT_FDCWD; `nofollow`/`follow` select the final-symlink
 * behaviour. All return 0 or a negated AIOS error code. When the PAL is confined (M4.2), the
 * single-target ops (chmod/chown/utimensat) resolve the path INSIDE the root first, so a final
 * symlink the guest planted cannot redirect the metadata change to a host file; the create ops
 * (symlinkat/linkat) confine the parent directory. `times` (utimensat) is a 2-element aios_timespec
 * array, or NULL for "now". (Linux: fchmodat/fchownat/symlinkat/linkat/utimensat.) */
int pal_host_fchmodat (pal_file_t dir, const char *path, unsigned int mode, int nofollow);
int pal_host_fchownat (pal_file_t dir, const char *path, unsigned int owner, unsigned int group, int nofollow);
int pal_host_symlinkat(const char *target, pal_file_t newdir, const char *linkpath);
int pal_host_linkat   (pal_file_t olddir, const char *oldpath, pal_file_t newdir, const char *newpath, int follow);
int pal_host_utimensat(pal_file_t dir, const char *path, const struct aios_timespec *times, int nofollow);

/* Set the value `who` sees returned from the syscall it is stopped at, WITHOUT resuming it -- it
 * stays stopped at the syscall exit. (pal_guest_return = this + pal_guest_resume.) The kernel uses
 * it to interpose signal delivery between finishing a syscall and resuming. */
int pal_guest_setret(pal_pid_t who, uint64_t retval);

/* --- signal delivery (register manipulation is host-specific, so it lives in the PAL) ---
 * The guest `who` is stopped at a syscall EXIT (its result already set via pal_guest_setret).
 * pal_guest_deliver makes it RUN its handler: it saves the guest's full post-syscall register state
 * into `savebuf` (opaque, >= PAL_SIGSAVE_SIZE), then sets pc = handler, arg0 = signum, return-
 * address = tramp, and resumes into the handler. When the handler returns it traps via the
 * trampoline (AIOS_SYS_SIGRETURN); pal_guest_sigreturn then restores `savebuf` so the guest resumes
 * right after the interrupted syscall. The kernel keeps `savebuf` per-process (host-agnostic bytes);
 * a future seL4 PAL builds a real signal frame instead. */
#define PAL_SIGSAVE_SIZE 512
int pal_guest_deliver  (pal_pid_t who, uint64_t handler, uint64_t signum, uint64_t tramp, void *savebuf);
int pal_guest_sigreturn(pal_pid_t who, const void *savebuf);

#endif /* AIOS_PAL_H */
