# AIOS Sandbox Architecture

## Overview

AIOS runs on seL4 14.0.0 via Microkit 2.1.0. Each user program executes inside
a sandbox protection domain (PD) with isolated memory. The orchestrator PD
manages all sandboxes and mediates IPC.

## Memory Layout

Each sandbox PD has three memory regions:

    Region      Size     Vaddr          Purpose
    -------     ------   -----------    --------------------------
    sbx_io      4 KB     0x20000000     IPC command/status buffer
    sbx_heap    16 MB    0x20100000     Heap (malloc, proc state)
    sbx_code    4 MB     0x21100000     Program code (loaded by orch)

The orchestrator maps all sandbox regions for code loading and state management.

## Program Entry Point

Programs are compiled as flat binaries (objcopy -O binary) and loaded at
0x21100000. The linker script (programs/link.ld) places sections in order:

    1. .text._start   - entry point (must be first)
    2. .text.zmain    - aios_main
    3. .text*         - all other code
    4. .rodata*       - constants
    5. .data*         - initialized data
    6. .bss*          - zero-initialized data

CRITICAL: Every program must use the AIOS_ENTRY macro (include/aios/aios.h):

    AIOS_ENTRY {
        // program code here
        return 0;
    }

This macro expands to define both _start() and aios_main(). The _start function
is placed in .text._start to guarantee it appears at 0x21100000. If a program
defines aios_main() directly without AIOS_ENTRY, the linker may place other
functions at the entry point, causing an immediate crash.

Symptom of missing AIOS_ENTRY: FAULT at IP=0x21100004 with a low ADDR value
(e.g., 0x108), because a helper function landed at the entry point instead of
_start.

## Syscall Interface

The sandbox runtime (src/sandbox.c) provides a syscall table (aios_syscalls_t)
passed to _start as the first argument. Programs access OS services through
this table:

    sys->write(fd, buf, len)    - write to console or file
    sys->read(fd, buf, len)     - read from console or file
    sys->open(path, flags)      - open a file
    sys->exec(path, args)       - execute another program
    sys->fork()                 - fork current process
    sys->spawn(path, args)      - spawn a new process
    sys->exit(code)             - terminate

POSIX wrappers in include/posix.h map standard function names to these syscalls.

## Stack Management

The sandbox runtime sets up the stack before jumping to user code. The stack
pointer is recorded in stack_top at entry. Stack snapshots are used for
fork/suspend/resume:

    FORK_STACK_SAVE_MAX = 1024 bytes (max stack saved during fork)
    PROC_STACK_MAX      = stack snapshot limit for suspend/resume

## Process Model: fork vs Threads

AIOS uses a fork-based process model, NOT threads:

    - fork() creates a new process in a SEPARATE sandbox PD
    - Parent and child have ISOLATED memory (copy-on-fork)
    - No shared address space between processes
    - Each sandbox PD has exactly ONE execution thread (Microkit constraint)

This means POSIX threads (pthreads) cannot be implemented as true kernel
threads. Options considered:

    1. Green threads (cooperative, single-core) - feasible but limited
    2. Shared memory between PDs - requires static aios.system config changes
    3. Multiple TCBs per PD - not supported by Microkit 2.1.0

Current decision: pthreads are NOT implemented. Programs requiring concurrency
should use fork() + IPC or the spawn() model.

## Context Save/Restore (src/arch/aarch64/context.h)

The sandbox uses arch_save_context() and arch_restore_context() for
fork/suspend/resume. These are inline assembly functions in the sandbox PD
(kernel side), NOT available to user programs.

    arch_context_t: saves x19-x30, sp, d8-d15 (callee-saved registers)
    arch_save_context():    returns 0 on save, 1 on resume
    arch_restore_context(): jumps back to save point

These are distinct from setjmp/longjmp which are user-space only.

## setjmp/longjmp (src/arch/aarch64/setjmp.S)

Real aarch64 assembly implementation for user programs. Linked into every
program via setjmp.o in the programs/Makefile.

    jmp_buf layout (22 x 8 = 176 bytes):
        [0-9]   x19-x28 (callee-saved GPRs)
        [10]    x29 (frame pointer)
        [11]    x30 (link register / return address)
        [12]    sp (stack pointer)
        [13-20] d8-d15 (callee-saved FP/SIMD registers)
        [21]    reserved

    sigjmp_buf layout (23 x 8 = 184 bytes):
        [0-21]  same as jmp_buf
        [22]    savesigs flag (unused, no real signals)

Key behaviors:
    - setjmp returns 0 on initial call, non-zero on longjmp return
    - longjmp(env, 0) returns 1 (not 0) per POSIX spec
    - sigsetjmp/siglongjmp are aliases (no signal mask to save in AIOS)

## Building User Programs

Programs are built in the programs/ directory:

    make              - builds all .c files into .BIN flat binaries
    make clean        - removes build artifacts

Architecture support objects (e.g., setjmp.o) are built from src/arch/aarch64/
and linked into every program. When adding a new architecture:

    1. Create src/arch/<arch>/ directory
    2. Implement setjmp.S (and any other arch-specific code)
    3. Update ARCH_DIR in programs/Makefile

## IPC Logging

PDs cannot write to the serial console directly (no filesystem access).
Logging options:

    - microkit_dbg_puts(): writes to debug console, races with orchestrator
    - IPC logging: write to shared memory, notify orchestrator, which prints
      via ser_puts() (used by echo_server, planned for other PDs)

The orchestrator is the only PD with serial console access (via serial_driver).

## Adding New Programs

    1. Create programs/myprogram.c
    2. Use AIOS_ENTRY macro for entry point
    3. Include "aios.h" and <posix.h>
    4. Run make in programs/
    5. Inject into disk: python3 tools/ext2_inject.py disk_ext2.img programs/*.BIN
    6. Run from shell: myprogram (or exec myprogram.bin from osh)
