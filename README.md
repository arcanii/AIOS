## Vision
AIOS is a microkernel operating system built on seL4, designed for stability,
security, and AI-native development. External AI (Claude, etc.) generates and
reviews code, which is compiled and deployed to AIOS. The long-term goal is
self-hosted development within AIOS itself. This is also a study on AI coding using Claude (Currently : Opus 4.6).

Security is also a topic theme : Un-hackable, Un-malwareable, Un-virusable. What can we do to make it better?

It currently is only tested in QEMU on aarch64 a53 

## Hardware Targets
- [x] - Development: QEMU virt (AArch64) — current platform
- [ ] Primary: Raspberry Pi 4/5 (BCM2711/BCM2712, AArch64)
- [ ] Primary++ : x86-64 (Ryzen Strix Halo)

The goal is to make it POSIX compatible, and Linux compatible.
Stretch goals : BSD, Win32, MacOS.

## Quick Start
Prerequisites:

  - Microkit SDK 2.1.0 (https://trustworthy.systems/projects/microkit/)
  - AArch64 cross-compiler (aarch64-linux-gnu-gcc or aarch64-elf-gcc)
  - QEMU (qemu-system-aarch64)
  - mtools (mformat, mcopy) - brew install mtools or apt install mtools

## AIOS Development Roadmap 
(see docs/ROADMAP.md)

## Milestone 1: POSIX Foundation (Current Mission)

## Future
- [ ] port to x86-64

## Architecture
(see docs/ARCHITECTURE.md)

TODO : Strong PD Auth_model (unhackable :smirk:) - the Authentican system is a PD, and can be multiple - to allow for layered authentican (too much? Sounded cool :unamused:)


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

## License

MIT License. See LICENSE file.

The LLM inference engine is based on llama2.c by Andrej Karpathy
(MIT License). https://github.com/karpathy/llama2.c
