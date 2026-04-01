# AIOS Development Roadmap

## Current Status

**Version 0.2.x** reached a design limitation with threading and process
isolation. The architecture required 8 separate sandbox PDs which made
pthreads and efficient fork/exec impractical.

**Version 0.3.x** introduces a new sandbox kernel design - a single sandbox
PD acting as a user-space kernel, managing all processes and threads
internally. See `docs/sandbox_kernel_design.md` for details.

---

## Milestone 1: POSIX Foundation ✅ (88% Complete)

- [x] libc with POSIX wrappers (open, read, write, close, stat, readdir)
- [x] VFS server with file descriptor table
- [x] Process server (sandbox lifecycle management)
- [x] /dev/console for stdin/stdout
- [x] Shell as a POSIX program
- [x] Core utilities: ls, cat, echo, cp, rm, mkdir
- [x] Signals (sigemptyset, sigaction, kill, raise)
- [x] Socket API (socket, bind, connect, send, recv)
- [x] stdio (printf, fprintf, sprintf, fopen, fread, fwrite)
- [x] Real setjmp/longjmp (aarch64 asm)
- [x] Real sscanf/fscanf parser
- [ ] pthreads API (blocked on 0.3.x)
- [ ] semaphores (blocked on 0.3.x)

## Milestone 2: Sandbox Kernel (0.3.x) 🔄 In Progress

Consolidate 14 PDs down to 8. Single sandbox PD with internal scheduling.

- [ ] Phase 2a: Consolidate aios.system - single sandbox PD, 512 MB memory
- [ ] Phase 2b: Rewrite sandbox.c as sandbox kernel with process/thread tables
- [ ] Phase 2c: Move scheduler from orchestrator into sandbox kernel
- [ ] Phase 2d: Simplify orchestrator to pure service router
- [ ] Phase 2e: Add preemption via orchestrator notification tick
- [ ] Phase 2f: Add pthread API for user programs

Target PD layout after 0.3.x:
```
serial_driver   (254)   UART hardware
blk_driver      (250)   Disk hardware (virtio-blk)
fs_server       (240)   ext2 filesystem
net_driver      (230)   Network hardware (virtio-net)
net_server      (210)   TCP/IP stack
auth_server     (210)   Authentication + credentials
orchestrator    (200)   Service router, policy
sandbox         (150)   User-space kernel
```

## Milestone 3: Network Stack ✅ (Mostly Complete)

- [x] virtio-net driver (QEMU)
- [x] TCP/IP stack (lwIP port)
- [x] Socket API in libc
- [x] HTTP client (for AI API access)
- [ ] HTTP server (status UI)

## Milestone 4: Status Web UI

- [ ] /api/status JSON endpoint
- [ ] /api/log real-time stream
- [ ] /api/priority user guidance endpoint
- [ ] HTML/JS dashboard

## Milestone 5: Raspberry Pi Port

- [ ] Microkit RPi4 board support
- [ ] BCM2711 UART driver
- [ ] SD card driver
- [ ] USB ethernet / WiFi

## Milestone 6: Build System Inside AIOS

- [ ] Port TCC as AIOS process
- [ ] libc headers on filesystem
- [ ] cc command, simple make tool

## Milestone 7: Git and Self-Hosted Development

- [ ] Minimal git client over HTTPS
- [ ] AI agent reads/modifies source
- [ ] Automated test suite
- [ ] Push at milestone intervals

## Milestone 8: AI Agent Autonomy

- [ ] AI agent PD with LLM API access
- [ ] Task queue from web UI
- [ ] Generate, compile, test, commit cycle
- [ ] Human review via web UI

---

## Hardware Targets

| Platform | Status | Notes |
|----------|--------|-------|
| QEMU virt (AArch64) | ✅ Primary | Current development platform |
| Raspberry Pi 4/5 | 🔜 Planned | BCM2711/BCM2712 |
| x86-64 (Ryzen Strix Halo) | 🔜 Stretch | Future target |

## Compatibility Goals

| Standard | Status | Notes |
|----------|--------|-------|
| POSIX.1 | 88% | See `docs/POSIX_COMPLIANCE.md` |
| Linux ABI | 🔜 Planned | Stretch goal |
| BSD | 🔜 Stretch | Future |
| Win32 | 🔜 Stretch | Future |
