# AIOS Architecture

## Vision
AIOS is a microkernel operating system built on seL4, designed for stability,
security, and AI-native development. External AI (Claude, etc.) generates and
reviews code, which is compiled and deployed to AIOS. The long-term goal is
self-hosted development within AIOS itself.

## Design Principles
- **Microkernel**: minimal trusted computing base, all services in userspace
- **POSIX-compatible**: applications expect a POSIX-like interface
- **AI-native**: AI agent is a first-class system component
- **Linux as reference**: re-engineer Linux patterns for microkernel security
- **Network-first**: status reporting, remote management, AI API access

## Hardware Targets
- Primary: Raspberry Pi 4/5 (BCM2711/BCM2712, AArch64)
- Development: QEMU virt (AArch64) — current platform

## Architecture Overview
```
┌─────────────────────────────────────────────────────┐ │ User Space │
│ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────────┐    │
│ │ shell│ │ httpd│ │ sshd │ │ prog │ │ ai_agent │    │
│ └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘ └────┬─────┘    │
│    │        │        │        │          │          │
│ ┌──┴────────┴────────┴────────┴───────────┴──────┐  │
│ │ libc (POSIX interface)                         │  │
│ └──┬────────┬────────┬────────┬──────────────────┘  │
│    │        │        │        │                     │
│ ┌──┴───┐  ┌─┴────┐ ┌─┴────┐  ┌┴──────┐ ┌─────────┐  │
│ │ vfs  │  │ net  │ │ proc │  │ devfs │ │ llm_srv │  │
│ │server│  │server│ │server│  │       │ │         │  │
│ └──┬───┘  └──┬───┘ └──┬───┘  └──┬───┘  └─────────┘  │
│    │         │        │         │                   │
│ ┌──┴───┐   ┌─┴────┐   │     ┌───┴────┐              │
│ │ fs   │   │ net  │   │     │ serial │              │
│ │server│   │driver│   │     │ driver │              │
│ └──┬───┘   └──┬───┘   │     └────────┘              │
│    │          │       │                             │
│ ┌──┴───┐    ┌─┴────┐  │                             │
│ │ blk  │    │ eth  │  │                             │
│ │driver│    │driver│  │                             │
│ └──────┘    └──────┘  │                             │
├───────────────────────┼──────────────────────────────┤
│ seL4 Microkernel (Microkit)                          │
├──────────────────────────────────────────────────────┤
│ Hardware (RPi / QEMU)                                │
└──────────────────────────────────────────────────────┘
```

## Service Architecture (PPC-based)

All inter-service communication uses seL4 Protected Procedure Calls (PPC):

- **blk_driver** (priority 250): raw block I/O, virtio-blk / SD card
- **fs_server** (priority 240): FAT16/FAT32/ext2 filesystem
- **vfs_server** (priority 230): virtual filesystem, mount points, POSIX fd table
- **net_driver** (priority 250): ethernet/WiFi raw frames
- **net_server** (priority 230): TCP/IP stack (lwIP or smoltcp)
- **proc_server** (priority 220): process management, sandbox lifecycle
- **orchestrator** (priority 200): system init, service coordination
- **libc**: linked into each process, translates POSIX calls to PPC syscalls

## Syscall Interface

Programs link against libc which translates POSIX calls to PPC:

```c
// libc/open.c
int open(const char *path, int flags) {
    seL4_SetMR(0, SYS_OPEN);
    seL4_SetMR(1, (seL4_Word)path);
    seL4_SetMR(2, flags);
    seL4_Call(VFS_EP, msg);
    return seL4_GetMR(0);
}
Boot Sequence
seL4 kernel starts, Microkit initializes all PDs
Drivers init: serial, block, network
fs_server mounts root filesystem
orchestrator reads /etc/init.cfg
Auto-loads AI model if present
Starts network services (httpd for status UI)
Starts shell on serial console
AI agent begins autonomous operation
Status Reporting
HTTP server on port 80 serves system status JSON + web UI
Reports: uptime, memory, disk, network, AI model status, build log
Interactive: user can submit priorities, approve/reject AI proposals
WebSocket for real-time log streaming
Development Workflow
Phase 1 (Current): External AI
Claude generates code on host
Cross-compile with aarch64-gcc
Deploy via disk image or network
Test in QEMU, then on RPi
Phase 2: Assisted Development
AIOS hosts git client
External AI pushes code via network
AIOS compiles (TCC/GCC port) and tests
Results reported via status UI
Phase 3: Self-Hosted
AI agent runs inside AIOS (larger model or API access)
Reads own source, proposes changes
Compiles, tests, commits to git
Human reviews via web UI EOF
echo "Created docs/ARCHITECTURE.md"


Now let's create the roadmap with concrete milestones:

```bash
cat > docs/ROADMAP.md << 'EOF'
# AIOS Development Roadmap

## Milestone 1: POSIX Foundation (Current Sprint)
Goal: Programs can use open/read/write/close/stat/readdir

- [ ] VFS server with file descriptor table
- [ ] libc with POSIX wrappers (open, read, write, close, stat, readdir)
- [ ] Process server (sandbox lifecycle management)
- [ ] /dev/console for stdin/stdout
- [ ] Shell as a POSIX program (reads stdin, writes stdout)
- [ ] Core utilities: ls, cat, echo, cp, mv, rm, mkdir

## Milestone 2: Network Stack
Goal: AIOS can communicate over the network

- [ ] virtio-net driver (QEMU)
- [ ] TCP/IP stack (lwIP port)
- [ ] Socket API in libc (socket, bind, listen, accept, connect)
- [ ] DNS resolver
- [ ] HTTP client (for AI API access)
- [ ] HTTP server (status UI)

## Milestone 3: Status Web UI
Goal: Browser-based system management

- [ ] Static file serving from /www
- [ ] /api/status JSON endpoint
- [ ] /api/log real-time log stream
- [ ] /api/priority POST endpoint for user guidance
- [ ] Simple HTML/JS dashboard
- [ ] AI task queue visible in UI

## Milestone 4: Raspberry Pi Port
Goal: AIOS boots on real hardware

- [ ] Microkit RPi4 board support (exists upstream)
- [ ] BCM2711 UART driver (replaces PL011)
- [ ] SD card driver (replaces virtio-blk)
- [ ] USB ethernet or WiFi driver
- [ ] Device tree parsing
- [ ] Boot from SD card

## Milestone 5: Build System Inside AIOS
Goal: Compile C code within AIOS

- [ ] Port TCC to run as AIOS process
- [ ] libc headers on filesystem
- [ ] `cc` command compiles .c to executable
- [ ] `make` equivalent (simple build tool)
- [ ] Can rebuild own utilities

## Milestone 6: Git and Self-Hosted Development
Goal: AIOS manages its own source

- [ ] Port minimal git client
- [ ] Clone/pull/commit/push over HTTPS
- [ ] AI agent reads source, proposes patches
- [ ] Automated test suite
- [ ] Push at milestone intervals

## Milestone 7: AI Agent Autonomy
Goal: AI drives development with human oversight

- [ ] AI agent PD with network access to LLM API
- [ ] Task queue: reads priorities from web UI
- [ ] Generates code, compiles, tests
- [ ] Submits for human review via web UI
- [ ] Approved changes committed and pushed
```
