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

```
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

```
