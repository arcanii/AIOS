/*
 * aios_abi.h -- the AIOS ABI. THIS is what AIOS programs see.
 *
 * Host-agnostic by construction: identical whether the AIOS kernel runs on Linux (syscalls
 * trapped via ptrace) or on a future verified seL4 (syscalls delivered via IPC). An AIOS
 * program invokes the platform syscall instruction with an AIOS syscall number in the syscall-
 * number register; AIOS -- not Linux -- owns these numbers and their semantics.
 *
 * The numbers are deliberately disjoint from any host kernel's so that "this is the AIOS ABI,
 * not the host ABI" is explicit, and any accidental host-ABI leakage is caught (an unknown
 * AIOS syscall returns -1). Grow this surface as the kernel grows (M1 = WRITE + EXIT).
 */
#ifndef AIOS_ABI_H
#define AIOS_ABI_H

#define AIOS_SYS_WRITE   0x1000   /* (fd, buf, len)         -> bytes written        */
#define AIOS_SYS_EXIT    0x1001   /* (code)                -> does not return       */
#define AIOS_SYS_OPEN    0x1002   /* (path, flags, mode)   -> aios fd, or -1        */
#define AIOS_SYS_READ    0x1003   /* (fd, buf, len)        -> bytes read (0 = EOF)  */
#define AIOS_SYS_CLOSE   0x1004   /* (fd)                  -> 0, or -1              */
#define AIOS_SYS_MMAP    0x1005   /* (len)                 -> guest addr, or 0      */
#define AIOS_SYS_FSTAT   0x1006   /* (fd, struct aios_stat*) -> 0, or -1           */
#define AIOS_SYS_LSEEK   0x1007   /* (fd, offset, whence)  -> new offset, or -1    */
#define AIOS_SYS_EXEC    0x1008   /* (path, argv, envp)    -> -1, or does not return (new image) */
#define AIOS_SYS_FORK    0x1009   /* ()                    -> child pid (parent), 0 (child), -1   */
#define AIOS_SYS_WAIT    0x100A   /* (pid, int *status, flags) -> reaped pid, or -1 (no children) */
#define AIOS_SYS_PIPE    0x100B   /* (int fds[2])          -> 0 (fds[0]=read end, fds[1]=write), -1 */
#define AIOS_SYS_DUP2    0x100C   /* (oldfd, newfd)        -> newfd, or -1                        */
#define AIOS_SYS_STAT    0x100D   /* (path, struct aios_stat*) -> 0, or -errno (follows symlinks) */
#define AIOS_SYS_LSTAT   0x100E   /* (path, struct aios_stat*) -> 0, or -errno (no follow)        */
#define AIOS_SYS_GETCWD  0x100F   /* (buf, size)           -> length written, or -errno           */
#define AIOS_SYS_CHDIR   0x1010   /* (path)                -> 0, or -errno                         */
#define AIOS_SYS_UNLINK  0x1011   /* (path)                -> 0, or -errno                         */
#define AIOS_SYS_MKDIR   0x1012   /* (path, mode)          -> 0, or -errno                         */
#define AIOS_SYS_RMDIR   0x1013   /* (path)                -> 0, or -errno                         */
#define AIOS_SYS_RENAME  0x1014   /* (oldpath, newpath)    -> 0, or -errno                         */
#define AIOS_SYS_GETPID  0x1015   /* ()                    -> the caller's pid                     */
#define AIOS_SYS_GETDENTS 0x1016  /* (fd, buf, len) -> bytes of aios_dirent records, 0 = end, -errno */
#define AIOS_SYS_OPENAT   0x1017  /* (dirfd, path, flags, mode) -> aios fd, or -errno              */
#define AIOS_SYS_FSTATAT  0x1018  /* (dirfd, path, aios_stat*, flags) -> 0, or -errno              */
#define AIOS_SYS_UNLINKAT 0x1019  /* (dirfd, path, flags) -> 0, or -errno (AT_REMOVEDIR -> rmdir)  */
#define AIOS_SYS_FACCESSAT 0x101A /* (dirfd, path, amode) -> 0, or -errno                          */
#define AIOS_SYS_READLINK 0x101B  /* (path, buf, bufsize) -> bytes (no NUL), or -errno             */
#define AIOS_SYS_FCNTL    0x101C  /* (fd, cmd, arg) -> result, or -errno (F_DUPFD/F_GETFD/F_SETFD) */
#define AIOS_SYS_SIGACTION 0x101D /* (signum, handler, tramp) -> old handler (0=DFL,1=IGN,else addr) */
#define AIOS_SYS_SIGRETURN 0x101E /* () -> does not return normally (the kernel restores pre-signal regs) */
#define AIOS_SYS_KILL     0x101F  /* (pid, signum) -> 0, or -errno; signum 0 = existence check       */
#define AIOS_SYS_ISATTY   0x1020  /* (fd) -> 1 if a terminal, 0 if not, or -errno                     */
#define AIOS_SYS_CLOCK_GETTIME 0x1021 /* (clk_id, struct aios_timespec*) -> 0, or -errno              */
/* file-metadata *at family (resolve `path` relative to a dirfd, or AIOS_AT_FDCWD). All return 0 or
 * -errno. chmod/chown/utimensat follow the final symlink unless AIOS_AT_SYMLINK_NOFOLLOW; symlinkat/
 * linkat CREATE a link. The plain chmod/chown/lchown/symlink/link/utimensat are these with AT_FDCWD. */
#define AIOS_SYS_FCHMODAT  0x1022 /* (dirfd, path, mode, flags)                  -> 0, or -errno       */
#define AIOS_SYS_FCHOWNAT  0x1023 /* (dirfd, path, owner, group, flags)          -> 0, or -errno       */
#define AIOS_SYS_SYMLINKAT 0x1024 /* (target, newdirfd, linkpath)                -> 0, or -errno       */
#define AIOS_SYS_LINKAT    0x1025 /* (olddirfd, oldpath, newdirfd, newpath, flags) -> 0, or -errno     */
#define AIOS_SYS_UTIMENSAT 0x1026 /* (dirfd, path, struct aios_timespec[2], flags) -> 0, or -errno     */
#define AIOS_SYS_UMASK     0x1027 /* (mask) -> the previous mask (per-process; applied on create)      */
/* process groups + controlling-terminal foreground group (the foundation for job control). The kernel
 * tracks a per-process pgid (inherited across fork, preserved across exec) and one foreground process
 * group for the controlling terminal. KILL with a pid <= 0 signals a process group (this increment). */
#define AIOS_SYS_SETPGID   0x1028 /* (pid, pgid) -> 0, or -errno; pid 0 = caller, pgid 0 = pid (leader) */
#define AIOS_SYS_GETPGID   0x1029 /* (pid) -> pgid, or -errno; pid 0 = caller (getpgrp = getpgid(0))    */
#define AIOS_SYS_TCSETPGRP 0x102A /* (fd, pgrp) -> 0, or -errno; sets the tty foreground process group  */
#define AIOS_SYS_TCGETPGRP 0x102B /* (fd) -> the tty foreground process group, or -errno (ENOTTY)       */
#define AIOS_SYS_SIGPROCMASK 0x102C /* (how, set*, old*) -> 0; blocked signals stay pending until unblocked */
#define AIOS_SYS_TCGETATTR 0x102D  /* (fd, struct aios_termios*) -> 0, or -errno (ENOTTY if not a tty)   */
#define AIOS_SYS_TCSETATTR 0x102E  /* (fd, actions, struct aios_termios*) -> 0, or -errno                */

/* ---- the Linux/aarch64 PAL trap convention (NOT part of the host-agnostic AIOS ABI) ----
 * AIOS owns its syscall NUMBERS (>= 0x1000, above); how a guest physically TRAPS into the kernel is a
 * per-host PAL detail. On Linux/aarch64 a guest traps with `svc #0`. The original convention put the
 * AIOS number directly in x8 -- which the ptrace PAL reads fine (ptrace traps the instruction). But
 * seccomp (the second PAL) only delivers a trap for IN-RANGE, IMPLEMENTED Linux syscall numbers: an
 * out-of-range number like 0x1000 is short-circuited to ENOSYS WITHOUT running the filter (proven on
 * arm64 6.8/6.12 by test/seccomp_probe.c). So an AIOS guest cannot trap seccomp using a 0x1000+ number
 * in x8. The fix, shared by BOTH Linux PALs: trap via an in-range real "GATEWAY" syscall in x8 (so
 * seccomp sees it) and carry the real AIOS number in x9. The PAL decodes: x8 == AIOS_GATEWAY -> an AIOS
 * syscall (number in x9); any other x8 -> a real Linux syscall the guest emitted = an escape attempt
 * (M4), surfaced as-is so the kernel kills it. The gateway is gettid (178): real + stable on every
 * arm64, side-effect-free, never issued by an AIOS guest, and the PAL always neutralizes it (x8 = -1)
 * so it never actually runs. A future seL4 PAL traps via IPC and ignores this entirely. */
#define AIOS_GATEWAY 178   /* __NR_gettid on aarch64 -- the in-range seccomp-trappable trap gateway */

/* sigprocmask `how` (match the shadow <signal.h>): a sigset_t is a bitmask, bit (1<<signum). */
#define AIOS_SIG_BLOCK   0
#define AIOS_SIG_UNBLOCK 1
#define AIOS_SIG_SETMASK 2

/* clock ids for AIOS_SYS_CLOCK_GETTIME (AIOS-owned; the PAL maps to the host's CLOCK_*). */
#define AIOS_CLOCK_REALTIME  0   /* wall-clock seconds since the Unix epoch (time/gettimeofday)   */
#define AIOS_CLOCK_MONOTONIC 1   /* a steadily-increasing clock from an unspecified start         */

/* signal dispositions (handler arg to SIGACTION): an address, or these two sentinels (match POSIX). */
#define AIOS_SIG_DFL 0   /* default action */
#define AIOS_SIG_IGN 1   /* ignore         */
#define AIOS_NSIG    65  /* signals 1..64  */

/* fcntl commands (AIOS-owned; match Linux). F_DUPFD = lowest free fd >= arg, sharing the backing --
 * dash uses it to park its script fd above 10. FD flags (close-on-exec) are accepted + ignored
 * (AIOS fds survive exec today). */
#define AIOS_F_DUPFD         0
#define AIOS_F_GETFD         1
#define AIOS_F_SETFD         2
#define AIOS_F_GETFL         3
#define AIOS_F_SETFL         4
#define AIOS_F_DUPFD_CLOEXEC 1030
#define AIOS_FD_CLOEXEC      1

/* The *at family resolves `path` relative to a directory fd, or -- when dirfd == AIOS_AT_FDCWD --
 * relative to the process cwd (the recurse-based utilities: rm, ls, cp, ...). AT_* are AIOS-owned;
 * the host PAL maps them to its own. amode for FACCESSAT is the AIOS_?_OK set. */
#define AIOS_AT_FDCWD             (-100)   /* dirfd sentinel: "relative to cwd" (matches Linux) */
#define AIOS_AT_SYMLINK_NOFOLLOW  0x100    /* FSTATAT/etc: do not follow a final symlink (lstat) */
#define AIOS_AT_REMOVEDIR         0x200    /* UNLINKAT: remove a directory (rmdir) not a file   */
#define AIOS_AT_SYMLINK_FOLLOW    0x400    /* LINKAT: follow a final symlink in oldpath          */
#define AIOS_F_OK 0   /* faccessat: exists      */
#define AIOS_X_OK 1   /* executable             */
#define AIOS_W_OK 2   /* writable               */
#define AIOS_R_OK 4   /* readable               */

/* AIOS_SYS_WAIT: pid selector + the wait status it stores via *status. pid == -1 waits for ANY
 * child; a positive pid waits for that child. The status encodes a normal exit as
 * (exit_code & 0xff) << 8 -- AIOS_WEXITSTATUS decodes it (a POSIX-shaped subset; signals later). */
#define AIOS_WAIT_ANY        ((unsigned long)-1)
#define AIOS_WEXITSTATUS(s)  (((s) >> 8) & 0xff)
/* wait options (match the shadow <sys/wait.h>): also report STOPPED (WUNTRACED) and CONTINUED
 * (WCONTINUED) children, and WNOHANG = poll, do not block. A stopped status is ((sig)<<8)|0x7f, a
 * continued status is 0xffff (so WIFSTOPPED/WSTOPSIG/WIFCONTINUED in <sys/wait.h> decode them). */
#define AIOS_WNOHANG     1
#define AIOS_WUNTRACED   2
#define AIOS_WCONTINUED  8

/* ---- error reporting ----
 * A syscall reports failure by returning a NEGATED error code: a value in [-4095, -1] means
 * -errno; any value >= 0 (or <= -4096, e.g. a high mmap address) is success. (Same convention as
 * Linux's MAX_ERRNO.) The codes are AIOS-owned but chosen to match Linux values, so the Linux PAL
 * maps host errno straight through; a future seL4 PAL maps its own errors onto these. libaios sets
 * `errno` from the negated return (see lib/include/errno.h). */
#define AIOS_EPERM        1   /* operation not permitted */
#define AIOS_ENOENT       2   /* no such file or directory */
#define AIOS_ESRCH        3   /* no such process */
#define AIOS_EINTR        4   /* interrupted */
#define AIOS_EIO          5   /* I/O error */
#define AIOS_EBADF        9   /* bad file descriptor */
#define AIOS_ECHILD      10   /* no child processes */
#define AIOS_EAGAIN      11   /* try again / would block */
#define AIOS_ENOMEM      12   /* out of memory */
#define AIOS_EACCES      13   /* permission denied */
#define AIOS_EFAULT      14   /* bad address */
#define AIOS_EBUSY       16   /* device or resource busy */
#define AIOS_EEXIST      17   /* file exists */
#define AIOS_ENOTDIR     20   /* not a directory */
#define AIOS_EISDIR      21   /* is a directory */
#define AIOS_EINVAL      22   /* invalid argument */
#define AIOS_EMFILE      24   /* too many open files */
#define AIOS_ESPIPE      29   /* illegal seek */
#define AIOS_EPIPE       32   /* broken pipe */
#define AIOS_ERANGE      34   /* result out of range */
#define AIOS_ENAMETOOLONG 36  /* file name too long */
#define AIOS_ENOSYS      38   /* function not implemented */
#define AIOS_ENXIO        6   /* no such device or address */
#define AIOS_E2BIG        7   /* argument list too long */
#define AIOS_ENOEXEC      8   /* exec format error */
#define AIOS_EXDEV       18   /* cross-device link */
#define AIOS_ENOTTY      25   /* not a typewriter */
#define AIOS_ETXTBSY     26   /* text file busy */
#define AIOS_ENOSPC      28   /* no space left on device */
#define AIOS_EDOM        33   /* math arg out of domain */
#define AIOS_ENOTEMPTY   39   /* directory not empty */
#define AIOS_ELOOP       40   /* too many symbolic links */
#define AIOS_IS_ERR(r)   ((r) < 0 && (r) >= -4095)   /* is a syscall return an -errno code? */

/* lseek whence (AIOS-owned; the PAL maps to host SEEK_*). */
#define AIOS_SEEK_SET    0
#define AIOS_SEEK_CUR    1
#define AIOS_SEEK_END    2

/* File metadata returned by AIOS_SYS_FSTAT / STAT / LSTAT (the kernel fills it in the guest's
 * memory). mode bits follow the conventional layout (matches host st_mode), so AIOS_S_IF* decode
 * the type. POSIX field names so this is byte-identical to the shadow <sys/stat.h> `struct stat`
 * (THEY MUST MATCH -- the kernel writes these bytes, the program reads them as struct stat). The
 * time fields are timespecs (sec + nsec); aios_timespec has the SAME layout as the shadow <time.h>
 * struct timespec (two 8-byte fields on LP64), so struct stat's st_atim/st_mtim/st_ctim line up. */
struct aios_timespec { long long tv_sec; long long tv_nsec; };
struct aios_stat {
    unsigned long long st_dev;
    unsigned long long st_ino;
    unsigned int       st_mode;    /* type + permission bits */
    unsigned int       st_nlink;
    unsigned int       st_uid;
    unsigned int       st_gid;
    unsigned long long st_rdev;    /* device id (for special files); was padding */
    long long          st_size;    /* file size in bytes */
    long long          st_blksize;
    long long          st_blocks;
    struct aios_timespec st_atim;  /* access / modify / status-change times (sec + nsec) */
    struct aios_timespec st_mtim;
    struct aios_timespec st_ctim;
};
#define AIOS_S_IFMT      0xF000
#define AIOS_S_IFREG     0x8000
#define AIOS_S_IFDIR     0x4000
#define AIOS_S_IFLNK     0xA000
#define AIOS_S_IFCHR     0x2000
#define AIOS_S_IFBLK     0x6000
#define AIOS_S_IFIFO     0x1000
#define AIOS_S_IFSOCK    0xC000

/* Directory entries returned by AIOS_SYS_GETDENTS: a packed stream of variable-length records in
 * the guest's buffer, each `d_reclen` bytes (so the next record begins at this one + d_reclen).
 * The kernel writes these bytes; libaios's readdir parses them into a fixed `struct dirent`. The
 * layout is host-agnostic AIOS-owned: the Linux PAL translates getdents64 into it record-by-record
 * (the two headers are field-identical, so a translated batch never outgrows its source); a future
 * seL4 PAL fills it from its fs server. d_name is NUL-terminated within the record. */
struct aios_dirent {
    unsigned long long d_ino;       /* inode number                                            */
    long long          d_off;       /* opaque cookie for the next entry (libaios iterates by reclen) */
    unsigned short     d_reclen;    /* length of THIS record (header + name + any padding)      */
    unsigned char      d_type;      /* AIOS_DT_*                                                */
    char               d_name[];    /* NUL-terminated file name                                 */
};
/* d_type values -- the conventional DT_* layout, so the Linux PAL passes them through unchanged. */
#define AIOS_DT_UNKNOWN   0
#define AIOS_DT_FIFO      1
#define AIOS_DT_CHR       2
#define AIOS_DT_DIR       4
#define AIOS_DT_BLK       6
#define AIOS_DT_REG       8
#define AIOS_DT_LNK      10
#define AIOS_DT_SOCK     12

/* Terminal attributes (tcgetattr/tcsetattr). The flag-bit values + c_cc indices in the shadow
 * <termios.h> match the host's, so the PAL translation is a field copy (like AIOS_E* errno); a future
 * seL4 PAL would remap them. The kernel treats this as an opaque blob it copies guest<->PAL. */
#define AIOS_NCCS 32
struct aios_termios {
    unsigned int  c_iflag, c_oflag, c_cflag, c_lflag;  /* input/output/control/local modes */
    unsigned char c_line;                              /* line discipline */
    unsigned char c_cc[AIOS_NCCS];                     /* control characters (indexed by AIOS_V*) */
    unsigned int  c_ispeed, c_ospeed;                  /* input/output baud */
};

/* AIOS open flags -- AIOS owns these values; the host PAL translates them to its native flags
 * (Linux O_*). The low 2 bits are the access mode. */
#define AIOS_O_RDONLY    0x0000
#define AIOS_O_WRONLY    0x0001
#define AIOS_O_RDWR      0x0002
#define AIOS_O_ACCMODE   0x0003
#define AIOS_O_CREAT     0x0100
#define AIOS_O_TRUNC     0x0200
#define AIOS_O_APPEND    0x0400
#define AIOS_O_CLOEXEC   0x0800   /* close-on-exec (advisory today: AIOS fds survive exec)        */
#define AIOS_O_DIRECTORY 0x1000   /* fail unless the path is a directory                          */
#define AIOS_O_EXCL      0x2000   /* with O_CREAT: fail if the path exists                        */
#define AIOS_O_NONBLOCK  0x4000   /* non-blocking open/IO                                         */

/* Reserved AIOS file descriptors (the kernel seeds these from the PAL std streams). */
#define AIOS_FD_STDIN    0
#define AIOS_FD_STDOUT   1
#define AIOS_FD_STDERR   2

#endif /* AIOS_ABI_H */
