## Vision
AIOS is a microkernel operating system built on seL4, designed for stability,
security, and AI-native development. External AI (Claude, etc.) generates and
reviews code, which is compiled and deployed to AIOS. The long-term goal is
self-hosted development within AIOS itself. This is also a study on AI coding using Claude (Currently : Opus 4.6).
Adding : Gtok 4.5 and GPT 5.4Pro (currently reviewing)

Security is a high priority topic theme. 

This is in an experimental / research phase, welcome for collaborators.

It currently is only tested in QEMU on aarch64 a53 

:persevere: Update April 1, 2026 : hit a brick wall with the 0.2.x design, so having to redesign it. New design : 0.3.x

The new design is to protect the key services (Orchestrator, blk_driver, net_driver, fs_server, vfs_server, auth_server) as PD, and make the Sandbox (the real userspace) as a single unit with a sandbox kernel to oversee it and handle communication with the other PDs.
Setting aside the LLM for the moment until this design can be fleshed out.
Let's see how this approach goes.


## Current Progress update

This is an unofficial report for tracking.

Created by tools/posix_audit.py
```============================================================
  AIOS POSIX.1 Compliance Audit
  Generated: 2026-04-01 07:43
============================================================

  Syscall handlers:  57
  POSIX wrappers:    205
  Standard headers:  20
  posix.h lines:     2066
  libc lines:        874

  File I/O               ████████████████████  18/18  COMPLETE
  File Status            ████████████████████  21/21  COMPLETE
  Directories            ████████████████████  10/10  COMPLETE
  Process Control        ████████████████████  15/15  COMPLETE
  Signals                ████████████████████  15/15  COMPLETE
  Pipes & FDs            ████████████████████   7/7   COMPLETE
  Sockets                ████████████████████  23/23  COMPLETE
  Memory                 ████████████████████   9/9   COMPLETE
  Strings                ████████████████████  18/18  COMPLETE
  stdio                  ████████████████████  27/27  COMPLETE
  stdlib                 ████████████████████  17/17  COMPLETE
  User/Group             ████████████████████  14/14  COMPLETE
  Environment            ████████████████████   4/4   COMPLETE
  Time                   ████████████████████  12/12  COMPLETE
  System Info            ████████████████████  10/10  COMPLETE
  I/O Multiplexing       ████████████████████   5/5   COMPLETE
  Threads (pthreads)     ░░░░░░░░░░░░░░░░░░░░   0/19  0%
  Semaphores             ░░░░░░░░░░░░░░░░░░░░   0/7   0%
  Dynamic Loading        ░░░░░░░░░░░░░░░░░░░░   0/4   0%
  Nonlocal Jumps         ████████████████████   4/4   COMPLETE
  Regex & Glob           ████████████████████   6/6   COMPLETE
  Logging                ████████████████████   3/3   COMPLETE
  Misc                   ████████████████████   4/4   COMPLETE

------------------------------------------------------------
  TOTAL:          242/272 functions present (88%)
  Full impl:      169/272 (62%)
  Partial/wrap:   73
  Stubs only:     0
  Missing:        30
  Real coverage:  242/272 (88%)
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

  Wiki report: docs/POSIX_COMPLIANCE.md
  (242/272 functions, 88% POSIX.1 compliance)
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

> **TODO**: Layered auth_server PDs for defense in depth

### 0.3.x Architecture (8 Protection Domains)

    +------------------------------------------------------------------+
    |                        SANDBOX PD (150)                          |
    |  +------------------------------------------------------------+  |
    |  |              Sandbox Kernel (user-space)                   |  |
    |  |  +--------+ +--------+ +--------+ +--------+               |  |
    |  |  | shell  | | httpd  | | prog   | |  ...   |   Processes   |  |
    |  |  +--------+ +--------+ +--------+ +--------+               |  |
    |  |  Scheduler, Threads, Memory, Local Syscalls                |  |
    |  +----------------------------+-------------------------------+  |
    |                               | PPC (remote syscalls)            |
    +-------------------------------+----------------------------------+
    |                       ORCHESTRATOR (200)                         |
    |               Service Router, Policy, Preemption Tick            |
    +-----------+-----------+-----------+------------------------------+
    | fs_server | net_server| auth_server                              |
    |   (240)   |   (210)   |   (210)                                  |
    +-----------+-----------+-----------+------------------------------+
    | blk_driver| net_driver| serial_driver                            |
    |   (250)   |   (230)   |   (254)                                  |
    +-----------+-----------+-----------+------------------------------+
    |                  seL4 Microkernel (Microkit)                     |
    +------------------------------------------------------------------+
    |                  Hardware (RPi / QEMU)                           |
    +------------------------------------------------------------------+

**Key changes in 0.3.x:**
- Single sandbox PD with internal user-space kernel
- All user processes/threads managed inside sandbox
- Cooperative + preemptive scheduling via orchestrator tick
- pthreads possible within sandbox (no cross-PD threads)
- Down from 14 PDs to 8

See docs/sandbox_kernel_design.md for full design details.



## License

MIT License. See LICENSE file.

The LLM inference engine is based on llama2.c by Andrej Karpathy
(MIT License). https://github.com/karpathy/llama2.c
