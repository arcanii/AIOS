# What AIOS needs from the host kernel — the dependency manifest

AIOS is a **gVisor-style userspace kernel**: AIOS programs see only the AIOS ABI, and the AIOS kernel
(`uk/kernel/aios_kernel.c`) reaches the host *only* through the narrow PAL (`uk/include/pal.h`). The
host is the interim **substrate** (Linux today, a verified seL4 later). This file enumerates the exact
host-kernel surface the Linux PAL uses — so we can (a) build a **minimal Linux** that supports AIOS and
nothing more, and (b) state precisely what the eventual **seL4 PAL** must provide (the verification
proof obligation). It is generated from the actual calls in `uk/pal/pal_linux_common.c` +
`pal/pal_linux.c` + `pal/pal_seccomp.c` (the only host-aware files).

The whole point of the narrow PAL is that this list is **short**. Everything else AIOS does —
processes, fds, pipes, signals, job control, the VFS, confinement, termios, a regex engine, a libc —
is implemented *inside* AIOS, on top of these primitives.

## 1. Process tracing / control (the trap + inject mechanism)

The PAL drives guests as traced child processes. **ptrace is the load-bearing primitive.**

| Host facility | Used for | Kernel config |
|---|---|---|
| `ptrace(PTRACE_TRACEME / SETOPTIONS / CONT / SYSCALL)` | spawn + run each guest, stop at each AIOS syscall | always present (no opt-out in a normal build) |
| `ptrace(PTRACE_GETREGSET/SETREGSET)` — `NT_PRSTATUS`, `NT_ARM_SYSTEM_CALL` | read AIOS syscall nr/args from regs; neutralize/inject syscalls; signal-frame dance | `CONFIG_HAVE_ARCH_TRACEHOOK` (arm64: yes) |
| `ptrace(PTRACE_GET_SYSCALL_INFO)` | classify entry vs exit stops (SYSEMU backend) | Linux ≥ 5.3 |
| `ptrace(PTRACE_GETEVENTMSG)` + events `EXEC/FORK/VFORK/CLONE/SECCOMP` | fork/exec/clone bookkeeping; seccomp-trap stops | core ptrace |
| `process_vm_readv` / `process_vm_writev` | copy syscall buffers + results across the guest boundary | **`CONFIG_CROSS_MEMORY_ATTACH=y`** |
| injected `__NR_mmap` / `__NR_clone` / `__NR_execve` / `__NR_exit_group` | grow guest memory, fork, exec, exit (rewritten into the trapped `svc`) | core |

**seccomp backend only** (`make PAL=seccomp`):

| Host facility | Used for | Kernel config |
|---|---|---|
| `seccomp(SECCOMP_SET_MODE_FILTER)` + `SECCOMP_RET_TRACE` | trap a guest's syscalls via a BPF filter instead of blanket `PTRACE_SYSCALL` | **`CONFIG_SECCOMP=y` + `CONFIG_SECCOMP_FILTER=y`** |
| `prctl(PR_SET_NO_NEW_PRIVS)` | install a seccomp filter unprivileged | core (`CONFIG_SECCOMP`) |
| `PTRACE_O_TRACESECCOMP` / `PTRACE_EVENT_SECCOMP` | deliver a `RET_TRACE` action to the tracer | with `CONFIG_SECCOMP_FILTER` |

> **Note — the GATEWAY (`AIOS_GATEWAY` in `aios_abi.h`).** AIOS numbers its syscalls `≥ 0x1000` so a
> real Linux syscall is unambiguously an escape. seccomp on arm64 does **not** deliver a trap for
> out-of-range syscall numbers (proven by `uk/test/seccomp_probe.c`), so AIOS guests trap via an
> in-range real gateway syscall (`gettid`/178) in `x8` carrying the real AIOS number in `x9`. This is a
> property of seccomp's *table dispatch* vs ptrace's *instruction trap*; the gateway needs no kernel
> config, but it is why `gettid` must be a real, implemented syscall in the build.

## 2. Filesystem service + confinement (the VFS backing)

The AIOS kernel owns the fd namespace; the PAL provides the backing host I/O.

| Host facility | Used for | Kernel config |
|---|---|---|
| `open/openat/read/write/close/lseek/fstat/stat/lstat/dup` | file I/O behind the AIOS VFS | core (`CONFIG_BLOCK` not required for tmpfs-only) |
| `getdents64` | `readdir` / `ls` | core |
| `unlinkat/mkdirat/renameat/fstatat/readlinkat/symlinkat/linkat/fchmodat/fchownat/utimensat` | path ops + the file-metadata layer | core |
| `pipe2(O_NONBLOCK)` | AIOS pipes (the kernel parks/wakes, never wedges) | core |
| `openat2(RESOLVE_IN_ROOT)` | **M4.2 fs confinement** — clamp every guest path inside the AIOS root | Linux ≥ 5.6 (no config) |
| `faccessat2` | `faccessat` under confinement | Linux ≥ 5.8 |
| `/proc/self/fd/<n>` (`readlink`) | canonicalize a confined `O_PATH` handle to a real path (M4.2/M4.3) | **`CONFIG_PROC_FS=y`** |
| `clock_gettime(CLOCK_REALTIME/MONOTONIC)` | the AIOS wall/monotonic clock | `CONFIG_POSIX_TIMERS=y` |

## 3. Terminal / console (interactive AIOS)

| Host facility | Used for | Kernel config |
|---|---|---|
| `isatty` / `tcgetattr` / `tcsetattr` | termios + raw mode; job-control `tcsetpgrp` | **`CONFIG_TTY=y`** |
| a console device | the appliance's stdin/stdout/stderr (and controlling terminal) | a serial driver, e.g. **`CONFIG_SERIAL_AMBA_PL011[_CONSOLE]=y`** for QEMU `virt` |
| `setpgid`/`getpgid`/`setsid` | process groups / job control | `CONFIG_MULTIUSER=y` |
| host signals `SIGINT`/`SIGTSTP` (the kernel catches + routes `^C`/`^Z`) | terminal-signal routing to the AIOS foreground group | core |

## 4. Bootstrap / process model

| Host facility | Used for | Kernel config |
|---|---|---|
| `fork` / `execv` / `wait4` / `exit_group` | the kernel's own process management of guests | core |
| ELF loading (the host loads `aios-uk` + each guest binary) | every guest is launched by exec of an AIOS-ABI ELF | **`CONFIG_BINFMT_ELF=y`** |
| `getenv("AIOS_ROOT")` / `umask` / `prctl` | confinement root, mask, no-new-privs | core |

## 5. The minimal appliance (see `uk/appliance/`)

A Linux that supports AIOS and little else needs, beyond a working arm64 base:

```
# trap + inject
CONFIG_CROSS_MEMORY_ATTACH=y          # process_vm_readv/writev
CONFIG_SECCOMP=y                      # seccomp PAL backend
CONFIG_SECCOMP_FILTER=y
# service + confinement
CONFIG_PROC_FS=y                      # /proc/self/fd canonicalization
CONFIG_POSIX_TIMERS=y                 # clock_gettime
CONFIG_MULTIUSER=y                    # setpgid / job control
# run programs
CONFIG_BINFMT_ELF=y
# console + tty
CONFIG_TTY=y
CONFIG_SERIAL_AMBA_PL011=y            # QEMU virt console (ttyAMA0)
CONFIG_SERIAL_AMBA_PL011_CONSOLE=y
CONFIG_DEVTMPFS=y
# boot the AIOS userland from RAM
CONFIG_BLK_DEV_INITRD=y               # initramfs holds aios-uk + the AIOS root
CONFIG_PRINTK=y                       # boot diagnostics
```

`openat2`/`RESOLVE_IN_ROOT`/`faccessat2`/`process_vm_*` are syscalls present in any modern kernel; the
only *features* to ensure are the ones above. Everything else a default arm64 config carries (block
devices, networking, most filesystems, most drivers) is **not** needed by AIOS and can be dropped — the
appliance is "Linux as a thin substrate under the AIOS userspace kernel."

## 6. What this means for the seL4 replant

The eventual `pal_sel4.c` must provide an equivalent of each row above from a verified base: the trap +
register/inject mechanism (seL4 fault IPC + a VMM, instead of ptrace), guest memory copy (cap-mapped
frames, instead of `process_vm_*`), the fs/console/clock service (seL4 servers, instead of host
syscalls), and the confinement view (an fs cap rooted at the AIOS root, instead of
`openat2(RESOLVE_IN_ROOT)`). The shortness of this list **is** the verification argument.
