# Porting dash (Debian Almquist Shell) to AIOS

Tag: DASH_PORT_V1

## Overview

dash is a POSIX-compliant /bin/sh implementation. At ~13K SLOC it is
the smallest production Unix shell, designed for speed and minimal
dependencies. Ubuntu and Debian use it as /bin/sh.

- Source: git://git.kernel.org/pub/scm/utils/dash/dash.git
- Mirror: https://github.com/tklauser/dash
- License: BSD (3-clause) -- compatible with AIOS
- Version target: 0.5.13.x (latest stable)
- Size: ~13K SLOC, ~35 .c files, 80% C / 16% roff / 4% other

## Why dash (not bash, mksh, or rc)

| Shell   | SLOC   | Dependencies | POSIX | Notes                          |
|---------|--------|-------------|-------|--------------------------------|
| bash    | ~176K  | Heavy       | Yes+  | Job control, readline, history |
| mksh    | ~25K   | Medium      | Yes   | Korn shell, Android uses it    |
| dash    | ~13K   | Minimal     | Yes   | Debian /bin/sh, best fit       |
| rc      | ~5K    | Minimal     | No    | Plan 9 syntax, non-POSIX       |

dash is the clear winner: smallest, most portable, strict POSIX, BSD
licensed, and exercises exactly the syscalls AIOS already implements.

## What dash unlocks for AIOS

- Shell scripts (#!/bin/sh) for automated testing
- POSIX control flow: if/then/else/fi, for/do/done, while, case/esac
- Proper multi-stage pipelines: cmd1 | cmd2 | cmd3
- Command substitution: result=$(cmd)
- Arithmetic expansion: $((1 + 2))
- Here-documents: cat <<EOF ... EOF
- Functions, local variables, traps
- The 400-line sbase_test.c becomes a 50-line sbase_test.sh

## Syscall Audit: What AIOS Has vs What dash Needs

### Already implemented (all working)

| Syscall         | dash uses for                                  |
|-----------------|------------------------------------------------|
| fork/clone      | Subshells, pipelines, command substitution      |
| execve          | Running external commands via PATH              |
| waitpid/wait4   | Child reaping, exit status, $?                  |
| pipe/pipe2      | Pipelines: cmd1 | cmd2                          |
| dup/dup3        | fd redirection >, <, 2>&1                       |
| open/openat     | File redirection, reading scripts               |
| read/write      | All I/O                                         |
| close           | fd cleanup after redirect/fork                  |
| lseek           | Here-doc temp file positioning                  |
| fstat/fstatat   | test -f, test -d, command lookup                |
| statx           | musl AArch64 stat path (already implemented)    |
| access          | PATH search (-x test on each candidate)         |
| getpid/getppid  | $$, $PPID shell variables                       |
| getuid/geteuid  | Prompt $ vs # selection                         |
| getcwd/chdir    | cd builtin, $PWD, $OLDPWD                       |
| kill            | kill builtin, signal delivery to children       |
| sigaction       | SIGINT, SIGCHLD, SIGQUIT handlers               |
| sigprocmask     | Block signals during critical sections           |
| fcntl           | F_GETFD, F_SETFD (CLOEXEC), F_DUPFD             |
| umask           | umask builtin                                   |
| getdents64      | Glob expansion: *.c, /bin/*, etc.               |
| unlink          | Here-doc temp file cleanup                      |
| exit/exit_group | Shell exit                                      |
| nanosleep       | sleep (if wired as builtin)                     |
| getgid/getegid  | Group identity for prompts                      |
| uname           | Available for scripts                           |

Total: 26+ syscalls dash needs, all present in AIOS.

### Must add or fix (blockers)

| Syscall/Feature   | Priority | Difficulty | Details                        |
|-------------------|----------|-----------|--------------------------------|
| dup2              | P0       | Trivial   | musl maps to dup3(old,new,0).  |
|                   |          |           | Wire __NR_dup2 to aios_sys_dup3.|
|                   |          |           | May already work via musl.     |
| setpgid           | P1       | Easy stub | Job control: setpgid(0,0).     |
|                   |          |           | Return 0 (flat process model). |
| getpgrp/getpgid   | P1       | Done      | Return getpid() -- v0.4.62.    |
| tcsetpgrp         | P1       | Easy stub | Terminal fg group. Return 0.   |
| tcgetpgrp         | P1       | Easy stub | Return getpid().               |
| isatty            | P1       | Easy      | Return 1 for fd 0/1/2, else 0. |
|                   |          |           | Used for interactive detection. |
| /dev/null         | P1       | Easy      | Hardcode in open: if path is   |
|                   |          |           | /dev/null, return fd with      |
|                   |          |           | infinite sink/empty source.    |
| O_APPEND          | P1       | Medium    | Shell >> redirect. openat must |
|                   |          |           | set f->pos = f->size on open.  |
| select/poll       | P2       | Stub      | Only for line editing. dash    |
|                   |          |           | works without it in -c mode.   |
| /tmp writable     | P1       | Done      | Already works in AIOS.         |

### AIOS infrastructure blockers

| Item                        | Priority | Status  | Notes                      |
|-----------------------------|----------|---------|----------------------------|
| Allocator budget            | P0       | BLOCKER | echo foo | grep bar = 3    |
|                             |          |         | forks. Limit now 18. Need  |
|                             |          |         | 40+ minimum for dash.      |
| ext2 dir cache invalidation | P1       | BLOCKER | Here-docs create temp      |
|                             |          |         | files. Must be visible     |
|                             |          |         | immediately after create.  |
| /dev/null                   | P1       | Missing | cmd > /dev/null 2>&1 is    |
|                             |          |         | extremely common.          |
| /dev/fd/N (optional)        | P3       | Defer   | Process substitution.      |

## dash Source Architecture

### Source files (~35 total, all in src/)

Core parser and evaluator (~8 files, pure computation, no OS deps):
- parser.c     -- tokenizer + recursive descent parser
- eval.c       -- AST evaluator, command dispatch
- expand.c     -- variable expansion, glob, tilde, arithmetic
- arith_yacc.c -- arithmetic expression parser
- arith_yylex.c -- arithmetic lexer
- syntax.c     -- character classification tables (generated)
- nodes.c      -- AST node constructors (generated)
- builtins.c   -- builtin command dispatch table (generated)

Builtins (~5 files, use syscalls we already have):
- cd.c     -- cd, pwd builtins (chdir, getcwd)
- echo.c   -- echo builtin (write)
- test.c   -- test / [ builtin (stat, access)
- printf.c -- printf builtin (write, formatting)
- kill.c   -- kill builtin (kill syscall)

Job control and signals (~3 files, heaviest syscall users):
- jobs.c  -- fork, waitpid, setpgid, tcsetpgrp, SIGCHLD
- trap.c  -- signal handlers, trap builtin
- error.c -- error handling, longjmp recovery

I/O and redirection (~4 files):
- input.c  -- input buffering, script file reading
- output.c -- output buffering, printf-like formatting
- redir.c  -- >, >>, <, 2>&1, here-docs (dup2, open, close, unlink)

Exec and PATH (~3 files):
- exec.c -- command lookup, PATH search, execve
- var.c  -- shell variables, $PATH, $HOME, $IFS, export
- show.c -- debug: AST pretty-printer

Infrastructure (~8 files, mostly self-contained):
- main.c      -- entry point, option parsing, main loop
- init.c      -- initialization (generated by mkinit)
- options.c   -- set, shopt, flag parsing
- memalloc.c  -- stack-based memory allocator (internal, no mmap)
- mystring.c  -- string utilities
- miscbltin.c -- read, umask, ulimit builtins
- mail.c      -- mail check (can disable)
- system.c    -- platform abstraction layer

Generated files (must be produced on host before cross-compile):
- syntax.h              -- from mksyntax.c (character classification)
- nodes.h + nodes.c     -- from mknodes.c + nodetypes (AST node types)
- init.c                -- from mkinit.c (scans all .c for INIT blocks)
- builtins.h + builtins.c -- from mkbuiltins + builtins.def

## Build Strategy

### Phase 1: Generate headers on macOS host

dash uses 4 small generator programs that run at build time to produce
headers. These run on the HOST (macOS), not the target:

    cd ~/Desktop/github_repos/dash/src

    # 1. syntax.h -- character classification
    cc -o mksyntax mksyntax.c
    ./mksyntax > syntax.h

    # 2. nodes.h + nodes.c -- AST node types
    cc -o mknodes mknodes.c
    ./mknodes nodetypes nodes.c.pat

    # 3. builtins.h + builtins.c -- builtin dispatch
    sh mkbuiltins builtins.def

    # 4. init.c -- initialization
    cc -o mkinit mkinit.c
    ./mkinit *.c

### Phase 2: Cross-compile with aios-cc

Single invocation gathering all .c files, similar to sbase:

    cd ~/Desktop/github_repos/AIOS
    DASH=~/Desktop/github_repos/dash/src

    ./scripts/aios-cc \
        $DASH/main.c $DASH/eval.c $DASH/parser.c $DASH/expand.c \
        $DASH/exec.c $DASH/jobs.c $DASH/trap.c $DASH/redir.c \
        $DASH/input.c $DASH/output.c $DASH/var.c $DASH/cd.c \
        $DASH/echo.c $DASH/test.c $DASH/printf.c $DASH/kill.c \
        $DASH/error.c $DASH/options.c $DASH/memalloc.c \
        $DASH/mystring.c $DASH/syntax.c $DASH/nodes.c \
        $DASH/builtins.c $DASH/init.c $DASH/show.c \
        $DASH/arith_yacc.c $DASH/arith_yylex.c \
        $DASH/miscbltin.c $DASH/system.c \
        -I $DASH -DSHELL -DSMALL -DGLOB_BROKEN \
        -o build-04/sbase/dash

### Phase 3: Install and test

    python3 scripts/mkdisk.py disk/disk_ext2.img 128 \
        --rootfs disk/rootfs \
        --install-elfs build-04/sbase \
        --aios-elfs build-04/projects/aios/

Test progression:
1. dash -c "echo hello"                  -- basic execution
2. dash -c "echo $((1+2))"               -- arithmetic
3. dash -c "true && echo yes"             -- control flow
4. dash -c "echo foo | cat"              -- pipeline
5. dash -c "for i in 1 2 3; do echo $i; done" -- loops
6. dash /tmp/test.sh                      -- script file
7. dash (interactive)                      -- prompt + builtins

## Defines and Config

Key -D flags for AIOS build:
- -DSHELL       -- required, enables shell mode
- -DSMALL       -- disable expensive features (history, etc.)
- -DGLOB_BROKEN -- use internal glob (avoid libc glob deps)
- -DJOBS=0      -- disable job control (avoid setpgid/tcsetpgrp)
- -DVTABSIZE=39 -- hash table size (default, can reduce)

Features to disable initially:
- HETIO / WITH_LINENO -- line editing, line numbers
- HISTORY -- command history
- MAIL -- mail checking

## Estimated Session Plan

| Session | Deliverable                                             |
|---------|---------------------------------------------------------|
| 1       | Allocator audit: instrument vka_alloc, measure per-fork |
|         | cost, implement proper child cleanup in exec_server     |
|         | reaper. Target: 40+ fork/exec budget.                   |
| 2       | Prerequisites: /dev/null in open, O_APPEND, setpgid     |
|         | stub, isatty, ext2 dir cache invalidation. Clone dash,  |
|         | generate headers on macOS host.                         |
| 3       | First compile with aios-cc. Fix compile errors (musl    |
|         | compat, missing defines). Boot test: dash -c echo hello  |
| 4       | Pipe, redirect, script execution. Fix runtime failures. |
|         | Target: dash -c with if/for/while/case working.         |
| 5       | Interactive mode, prompt, cd/pwd builtins. Convert      |
|         | sbase_test.c to sbase_test.sh. Full interactive dash.   |

## Risk Assessment

| Risk                        | Impact | Mitigation                    |
|-----------------------------|--------|-------------------------------|
| Allocator not fixable       | High   | Increase pool to 3000+ pages  |
|                             |        | as band-aid. Proper fix is    |
|                             |        | resource reclamation.         |
| musl/seL4 ABI mismatch     | Medium | Same approach as sbase: fix   |
|                             |        | in aios_posix.c shim layer.   |
| Job control assumptions     | Medium | -DJOBS=0 disables entirely.   |
|                             |        | Stubs for setpgid/tcsetpgrp.  |
| Here-doc temp file visible  | Medium | ext2 dir cache fix resolves.  |
| glob/fnmatch dependency     | Low    | -DGLOB_BROKEN uses internal.  |
| signal semantics mismatch   | Low    | AIOS signals are polled, not  |
|                             |        | async. dash checks between    |
|                             |        | commands which aligns well.   |

## Success Criteria

Minimum viable:
- dash -c "echo hello" prints hello
- dash -c "echo \$((2+3))" prints 5
- dash -c "for i in 1 2 3; do echo \$i; done" prints 1 2 3
- dash -c "true && echo yes || echo no" prints yes

Full success:
- dash can run a shell script from file
- Pipelines work: echo foo | grep foo | wc -l
- Interactive mode with prompt
- sbase_test.sh runs under dash and passes all tests

## References

- dash source: git://git.kernel.org/pub/scm/utils/dash/dash.git
- POSIX shell spec: IEEE Std 1003.1-2024, Shell Command Language
- Almquist shell history: https://en.wikipedia.org/wiki/Almquist_shell
- AIOS POSIX audit: 81/81 (100%), posix_verify V3: 98/98 PASS
- AIOS current fork budget: 18 (must reach 40+ for dash)
