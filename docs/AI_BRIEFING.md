# AIOS AI Development Briefing

This document briefs AI assistants on the AIOS project for smooth collaboration.

## Project Summary

**AIOS** is a microkernel operating system built on seL4 (Microkit 2.1.0), targeting AArch64 (QEMU, Raspberry Pi). It aims to be POSIX-compatible with AI-native development - external AI generates/reviews code, with long-term goal of self-hosted AI development.

**Current Version**: 0.3.x (in progress)
**Platform**: QEMU virt AArch64 (Cortex-A53)
**License**: MIT

## Current Status

- **POSIX compliance**: 88% (242/272 functions)
- **Missing**: pthreads, semaphores, dynamic loading (dlopen)
- **Blocked on**: 0.3.x sandbox kernel redesign

Run `python3 tools/posix_audit.py` to see current compliance report.

## Architecture (0.3.x)

The 0.2.x design hit limitations with threading across separate sandbox PDs.
The 0.3.x redesign consolidates to 8 Protection Domains:

| PD              | Priority | Role                              |
|-----------------|----------|-----------------------------------|
| serial_driver   | 254      | UART hardware                     |
| blk_driver      | 250      | Disk (virtio-blk)                 |
| fs_server       | 240      | ext2 filesystem                   |
| net_driver      | 230      | Network (virtio-net)              |
| net_server      | 210      | TCP/IP stack                      |
| auth_server     | 210      | Authentication                    |
| orchestrator    | 200      | Service router, preemption tick   |
| sandbox         | 150      | User-space kernel, all processes  |

**Key insight**: Single sandbox PD with internal user-space kernel. All user
processes/threads managed inside sandbox. Enables pthreads within sandbox.

See: `docs/sandbox_kernel_design.md`, `docs/ARCHITECTURE.md`

## Repository Structure

    AIOS/
    ├── src/                    # Kernel and PD sources
    │   ├── orch/               # Orchestrator (orch_syscall.inc has syscalls)
    │   ├── sandbox.c           # Sandbox PD (being rewritten for 0.3.x)
    │   └── arch/aarch64/       # Architecture-specific (setjmp.S, etc.)
    ├── libc/                   # POSIX libc implementation
    │   ├── src/                # C sources
    │   └── include/            # Headers (sys/, netinet/, arpa/)
    ├── include/
    │   └── posix.h             # Main POSIX wrapper header (2000+ lines)
    ├── programs/               # User programs (shell, utilities)
    │   └── aios.h              # Program-facing header
    ├── tools/
    │   └── posix_audit.py      # POSIX compliance checker
    ├── docs/
    │   ├── ARCHITECTURE.md     # System architecture
    │   ├── ROADMAP.md          # Development roadmap
    │   ├── POSIX_COMPLIANCE.md # Auto-generated compliance report
    │   └── sandbox_kernel_design.md  # 0.3.x design spec
    ├── Makefile                # Build system
    └── aios.system             # Microkit system description

## Development Workflow

**Environment**: macOS terminal with brew

**Code changes**: Use sed, grep, or Python scripts
- Scripts go in `/tmp/` with `python3 /tmp/script.py` execution
- Make changes incremental (not monolithic)
- Scripts must be idempotent (guard against double execution)
- Verify changes after each script
- No single quotes in code comments (breaks heredoc copy/paste)

**Example script pattern**:

    cat > /tmp/fix_something.py << 'ENDSCRIPT'
    #!/usr/bin/env python3
    import sys
    
    TARGET = "path/to/file"
    
    with open(TARGET, "r", encoding="utf-8") as f:
        content = f.read()
    
    # Guard: already fixed?
    if "expected_after_fix" in content:
        print("SKIP: Already fixed")
        sys.exit(0)
    
    # Make changes
    content = content.replace("old", "new")
    
    with open(TARGET, "w", encoding="utf-8") as f:
        f.write(content)
    
    # Verify
    with open(TARGET, "r", encoding="utf-8") as f:
        if "expected_after_fix" not in f.read():
            print("ERROR: Fix not applied")
            sys.exit(1)
    
    print("OK: Fixed something")
    ENDSCRIPT
    python3 /tmp/fix_something.py

**Git workflow**:
- `make bump-patch` for version increment
- Check `git log --oneline -5` for commit message style
- Changes applied via GitHub Desktop
- Hold git commits until related changes complete

## Key Files to Know

| File | Purpose |
|------|---------|
| `include/posix.h` | POSIX wrappers, static inline functions |
| `src/orch/orch_syscall.inc` | Syscall handlers (case SYS_XXX) |
| `src/sandbox.c` | Sandbox PD (being rewritten) |
| `libc/src/*.c` | libc implementation |
| `programs/aios.h` | User program interface |
| `tools/posix_audit.py` | Compliance audit tool |
| `docs/sandbox_kernel_design.md` | 0.3.x design specification |

## Current Priorities

1. **0.3.x Sandbox Kernel** - Implement design from sandbox_kernel_design.md
   - Phase 2a: Consolidate aios.system
   - Phase 2b: Rewrite sandbox.c with process/thread tables
   - Phase 2c: Move scheduler into sandbox kernel
   - Phase 2d: Simplify orchestrator
   - Phase 2e: Add preemption tick
   - Phase 2f: Add pthread API

2. **pthreads** - Blocked on 0.3.x, high priority once sandbox kernel works

3. **semaphores** - Also blocked on 0.3.x

## Useful Commands

    # Build
    make clean && make
    
    # Run in QEMU
    make run
    
    # Check POSIX compliance
    python3 tools/posix_audit.py
    
    # Version bump
    make bump-patch
    
    # View recent commits
    git log --oneline -10

## Communication Style

- Be direct and technical
- Provide working code/scripts, not just explanations
- When in doubt, ask for file contents rather than assuming
- Test changes can be verified (scripts should verify themselves)
- Keep changes atomic and reversible

## Important Notes

- LLM server is deferred until 0.3.x architecture stabilizes
- No symlinks or hard links in AIOS ext2 implementation
- No kernel threads - all threading within sandbox PD
- Single address space within sandbox (no MMU isolation between processes)
- seL4 provides isolation between PDs only
