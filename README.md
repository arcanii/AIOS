## Vision
AIOS is a microkernel operating system built on seL4, designed for stability,
security, and AI-native development. External AI (Claude, etc.) generates and
reviews code, which is compiled and deployed to AIOS. The long-term goal is
self-hosted development within AIOS itself. This is also a study on AI coding using Claude (Currently : Opus 4.6).

Security is also a topic theme : Un-hackable, Un-malwareable, Un-virusable. What can we do to make it better?

It currently is only tested in QEMU on aarch64 a53 

## Current Progress update

Created by tools/posix_audit.py
```
============================================================
  AIOS POSIX.1 Compliance Audit
  Generated: 2026-03-30 18:47
============================================================

  Syscall handlers:  54
  POSIX wrappers:    208
  Standard headers:  20
  posix.h lines:     1665
  libc lines:        874

  File I/O               █████████████░░░░░░░  12/18  66% (+6 stubs)
  File Status            █████████████░░░░░░░  14/21  66% (+7 stubs)
  Directories            ██████████████████░░   9/10  90% (+1 stubs)
  Process Control        ██████████████████░░  14/15  93% (+1 stubs)
  Signals                █████████████░░░░░░░  10/15  66% (+5 stubs)
  Pipes & FDs            ██████████████░░░░░░   5/7   71% (+2 stubs)
  Sockets                ██████████████████░░  21/23  91% (+2 stubs)
  Memory                 █████████████░░░░░░░   6/9   66% (+3 stubs)
  Strings                ████████████████████  18/18  COMPLETE
  stdio                  █████████████████░░░  24/27  88% (+3 stubs)
  stdlib                 ████████████████░░░░  14/17  82% (+3 stubs)
  User/Group             ██████████████░░░░░░  10/14  71% (+4 stubs)
  Environment            ████████████████████   4/4   COMPLETE
  Time                   ████████████████░░░░  10/12  83% (+2 stubs)
  System Info            ████████████░░░░░░░░   6/10  60% (+4 stubs)
  I/O Multiplexing       ████████░░░░░░░░░░░░   2/5   40% (+3 stubs)
  Threads (pthreads)     ░░░░░░░░░░░░░░░░░░░░   0/19  0%
  Semaphores             ░░░░░░░░░░░░░░░░░░░░   0/7   0%
  Dynamic Loading        ░░░░░░░░░░░░░░░░░░░░   0/4   0%
  Nonlocal Jumps         ░░░░░░░░░░░░░░░░░░░░   0/4   0% (+4 stubs)
  Regex & Glob           ██████░░░░░░░░░░░░░░   2/6   33% (+4 stubs)
  Logging                ░░░░░░░░░░░░░░░░░░░░   0/3   0% (+3 stubs)
  Misc                   ███████████████░░░░░   3/4   75% (+1 stubs)

------------------------------------------------------------
  TOTAL:          242/272 functions present (88%)
  Full impl:      169/272 (62%)
  Partial/wrap:   15
  Stubs only:     58
  Missing:        30
  Real coverage:  184/272 (67%)
------------------------------------------------------------

  Missing functions:

    Threads (pthreads):
      pthread_create, pthread_join, pthread_detach, pthread_exit, pthread_mutex_init, pthread_mutex_lock
      pthread_mutex_unlock, pthread_mutex_destroy, pthread_cond_init, pthread_cond_wait, pthread_cond_signal, pthread_cond_broadcast
      pthread_rwlock_init, pthread_rwlock_rdlock, pthread_rwlock_wrlock, pthread_rwlock_unlock, pthread_key_create, pthread_setspecific
      pthread_getspecific
    Semaphores:
      sem_init, sem_wait, sem_post, sem_destroy, sem_open, sem_close
      sem_unlink
    Dynamic Loading:
      dlopen, dlsym, dlclose, dlerror
```


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
  - (optional for FAT16) mtools (mformat, mcopy) - brew install mtools or apt install mtools

## AIOS Development Roadmap 
(see docs/ROADMAP.md)

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
