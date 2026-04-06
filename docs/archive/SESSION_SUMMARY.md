# AIOS Session Summary — 2026-04-05
## v0.4.42 → v0.4.47 (builds 978 → 1055)

## Accomplished

### 1. sbase Tool Expansion (v0.4.43)
Added 14 new sbase tools: sha1sum, sha224sum, sha384sum, sha512-224sum,
sha512-256sum, cols, uudecode, uuencode, mknod, ed, tar, bc, renice, xinstall.
bc required yacc generation via bison. Total: 93 sbase tools, 113 programs on disk.

### 2. Multi-Round FS_LS IPC (v0.4.43)
Directory listings exceeded 952-byte MR limit. Fixed with offset-based chunked
protocol: client sends offset, server returns chunk, client loops.

### 3. tty_server Phase 1 (v0.4.44)
Replaced serial_server with tty_server. Supports cooked/raw modes, backspace,
Ctrl-C/U/W/D. Echo defaults OFF (mini_shell does its own). Foundation for
multi-user TTY architecture (5 phases planned in DESIGN_TTY.md).

### 4. fork() on seL4 (v0.4.45-v0.4.46) — HISTORIC
First known POSIX fork() on bare seL4 without CAmkES/Microkit.

Architecture: ELF reload + .data copy (299 pages) + stack copy (16 pages) +
cap copy at same slot numbers + AArch64 register setup (x0=badge, x1=msginfo,
x2=MR0=0) + parent tpidr preserved.

Root cause of all failures: off-by-one .data page count. Page-aligned base
was one page before vaddr, so the last page (containing TLS with
__sel4_ipc_buffer pointer) was not copied.

### 5. waitpid() (v0.4.46)
Parent blocks until child exits via SaveCaller deferred reply pattern.
Spin-reap after PIPE_EXIT: pipe_server waits for child fault after exit
IPC, then delivers exit status to waiting parent. Zombie table for
SMP race (child exits before parent calls waitpid).

### 6. Exit Codes (v0.4.46)
sel4runtime exit callback sends PIPE_EXIT(code) before faulting.
`return 42` from main → sel4runtime_exit(42) → aios_exit_cb(42) → PIPE_EXIT.
Parent receives exit code 42 via waitpid.

### 7. getpid() (v0.4.46)
Child resets aios_pid=0 in fork return path. getpid() lazily queries
PIPE_GETPID on first call. Avoids SMP race from querying during fork return.

### 8. exec() (v0.4.47)
PIPE_EXEC handler in pipe_server: destroys old process, loads new ELF,
spawns with fresh sel4runtime, preserves PID/ppid/fault_ep. Reuses
file-scope elf_buf (zero new memory). Successfully exec'd /bin/sysinfo
with full output from forked child.

## Current State (v0.4.47)

### Working
- fork() — parent gets child PID, child gets 0
- waitpid() — parent blocks until child exits
- getpid() — correct PIDs for parent and child
- Exit codes — delivered via sel4runtime callback
- exec() — replaces process image, preserves PID
- fork+exec+waitpid cycle — tested 12+ times without leaks
- Clean proc table after every cycle
- 93 sbase + 20 AIOS apps = 113 programs on disk
- POSIX compliance: 244/272 (90%) with fork/waitpid/exec added

### Not Yet Working
- exec argv passthrough (only path, not user arguments)
- _exit() exit code (bypasses sel4runtime, code lost)
- Child printf sometimes missing (SMP timing)

### Known Issues
- aios_root.c is ~2500 lines — needs modularization
- auth server interleaved output at boot (cosmetic)
- `pipe_ep` is non-static (changed for fork_test extern access)

## IPC Labels (complete)

| Range | Server | Labels |
|-------|--------|--------|
| 1-4 | tty_server | SER_PUTC, SER_GETC, SER_PUTS, KEY_PUSH |
| 10-18 | fs_thread | FS_READ, FS_WRITE, FS_STAT, FS_LS, ... |
| 20-26 | exec_thread | EXEC_RUN, EXEC_NICE, EXEC_RUN_BG, EXEC_FORK, EXEC_WAIT |
| 30-31 | thread_server | THREAD_CREATE, THREAD_JOIN |
| 40-52 | auth_server | AUTH_LOGIN, AUTH_CHECK, ... |
| 60-69 | pipe_server | PIPE_CREATE(60), WRITE(61), READ(62), CLOSE(63), KILL(64), FORK(65), GETPID(66), WAIT(67), EXIT(68), EXEC(69) |
| 70-78 | tty_server | TTY_WRITE(70), TTY_READ(71), TTY_IOCTL(72), TTY_INPUT(75) |

## Next Steps (Priority Order)

### 1. Pass exec argv (small)
Currently exec only sends path via PIPE_EXEC MRs. Need to also pass argv
strings. Options:
- Pack argv after path in MRs (limited by 952-byte MR space)
- Use a shared memory page for longer argument lists
- Use pipe to stream argv data

### 2. Modularize aios_root.c (medium)
Split into logical units:
```
src/aios_root.c          — boot, init, main (~400 lines)
src/servers/exec_server.c — exec_thread_fn (~300 lines)
src/servers/pipe_server.c — pipe_server_fn + fork/wait/exec (~600 lines)
src/servers/thread_server.c — thread_server_fn (~200 lines)
src/process/fork.c       — do_fork, fork helpers (~400 lines)
src/process/reap.c       — reap_check, zombie table (~100 lines)
```
Challenge: these all run as threads in root's VSpace sharing globals
(active_procs, vka, vspace, etc). Extract shared state to a header.

### 3. TTY Phase 2: getty/shell separation (medium)
Use fork+exec to implement Unix login flow:
```
init → fork → exec getty → getty reads username →
     → fork → exec login → login verifies password →
     → exec mini_shell → shell runs
```
This replaces the current monolithic mini_shell that has login baked in.

### 4. TTY Phase 3: multiple virtual terminals (larger)
Ctrl-A 1/2/3/4 switches between 4 VTs, each with independent getty/shell.
Requires tty_server to manage multiple line disciplines and per-VT buffers.

### 5. _exit() support (small)
Intercept __NR_exit_group in the musl syscall shim to send PIPE_EXIT
before the raw svc. Currently _exit() does a raw syscall that seL4
doesn't understand, causing immediate fault with exit code lost.

### 6. Copy-on-write fork (larger, v0.5.x)
Current fork eagerly copies 299 .data pages (~1.2MB). COW would:
- Share .data pages read-only between parent and child
- Handle write faults by copying the faulted page on demand
- Requires seL4 fault handler for page faults on COW pages
- Significant performance improvement for fork+exec (most pages never written)

## Key Files

| File | Lines | Purpose |
|------|-------|---------|
| src/aios_root.c | ~2500 | Root task: boot, servers, fork, exec |
| src/lib/aios_posix.c | ~1500 | POSIX syscall shim |
| src/apps/mini_shell.c | ~1250 | Interactive shell |
| src/apps/tty_server.c | ~300 | TTY line discipline |
| src/apps/fork_test.c | ~25 | fork+exec test |
| src/procfs.c | ~200 | Process table + /proc filesystem |
| docs/FORK_IMPLEMENTATION.md | ~500 | Definitive fork/waitpid guide |
| docs/DESIGN_TTY.md | ~200 | TTY architecture (5 phases) |

## Build Commands

```bash
# Full rebuild
cd ~/Desktop/github_repos/AIOS
rm -rf build-04 && mkdir build-04 && cd build-04
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../deps/kernel/gcc.cmake \
    -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- ..
ninja

# Rebuild sbase (after aios_posix.c changes)
python3 scripts/build_sbase.py --clean --jobs 16

# Install to disk
python3 scripts/mkdisk.py disk/disk_ext2.img 128 \
    --rootfs disk/rootfs \
    --install-elfs build-04/sbase \
    --install-elfs build-04/projects/aios/

# Boot
qemu-system-aarch64 -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G -nographic -serial mon:stdio \
    -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

## Key Learnings Reference

See docs/FORK_IMPLEMENTATION.md for 15 detailed learnings covering:
- AArch64 seL4 syscall ABI (x0=badge, NOT msginfo)
- Off-by-one .data page count (root cause of all fork failures)
- Static TLS + morecore (zero dynamic allocation in seL4 userspace)
- Frame cap ownership (CNode_Copy before mapping)
- SaveCaller pattern for deferred reply
- Spin-reap pattern for PIPE_EXIT → fault timing
- Zombie table for SMP race conditions
- sel4runtime exit callback for exit code delivery
- Child badge re-minting for correct IPC routing
